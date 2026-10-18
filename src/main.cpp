// semcache entry point: composition root. Loads config, wires modules together, runs the I/O pool.

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "config/config.hpp"
#include "log/log.hpp"
#include "metrics/metrics.hpp"
#include "net/asio.hpp"
#include "proxy/proxy_server.hpp"
#include "upstream/http_upstream.hpp"

namespace {

void usage() { std::fprintf(stderr, "usage: semcache --config <file.toml>\n"); }

semcache::Task<void> wait_for_drain(const semcache::ProxyServer& server, semcache::net::io_context& ioc,
                                    std::chrono::milliseconds timeout) {
  using namespace semcache;
  constexpr auto kPollInterval = std::chrono::milliseconds(20);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  net::steady_timer timer(co_await net::this_coro::executor);
  while (server.active_sessions() > 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      log::warn("shutdown timeout; abandoning live sessions", {{"sessions", server.active_sessions()}});
      ioc.stop();
      co_return;
    }
    timer.expires_after(kPollInterval);
    co_await timer.async_wait(use_nothrow);
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace semcache;

  std::string config_path;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else {
      usage();
      return 2;
    }
  }
  if (config_path.empty()) {
    usage();
    return 2;
  }

  Config cfg;
  try {
    cfg = load_config_file(config_path, process_env());
  } catch (const ConfigError& e) {
    std::fprintf(stderr, "semcache: config error: %s\n", e.what());
    return 2;
  }

  log::init(cfg.logging.level);
  if (cfg.upstream.api_key.empty()) {
    log::warn("SEMCACHE_UPSTREAM_API_KEY is not set; upstream requests carry no Authorization header");
  }

  const int threads = cfg.server.io_threads > 0
                          ? cfg.server.io_threads
                          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  net::io_context ioc(threads);
  auto tls_ctx = make_client_tls_context(cfg.upstream.verify_tls);
  HttpUpstream upstream(cfg.upstream, tls_ctx);
  MetricsRegistry registry;

  std::unique_ptr<ProxyServer> server;
  try {
    server = std::make_unique<ProxyServer>(ioc, cfg, upstream, registry);
  } catch (const std::exception& e) {
    log::error("failed to start listener", {{"error", e.what()}});
    return 1;
  }
  server->start();

  // Graceful shutdown: stop accepting, cancel live sessions, wait for them to drain (bounded by
  // server.shutdown_timeout), then let run() return because no work is left.
  net::signal_set signals(ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const beast::error_code& ec, int sig) {
    if (ec) return;
    log::info("shutting down", {{"signal", sig}});
    server->stop();
    net::co_spawn(ioc, wait_for_drain(*server, ioc, cfg.server.shutdown_timeout), net::detached);
  });

  log::info("semcache listening", {{"host", cfg.server.host},
                                   {"port", server->port()},
                                   {"mode", cfg.mode == Mode::demo ? "demo" : "live"},
                                   {"upstream", cfg.upstream.base_url},
                                   {"io_threads", threads},
                                   {"tenants", cfg.tenants.size()}});

  std::vector<std::thread> pool;
  for (int i = 1; i < threads; ++i) pool.emplace_back([&ioc] { ioc.run(); });
  ioc.run();
  for (auto& t : pool) t.join();
  log::info("stopped");
  // If the drain timed out, abandoned coroutine frames are destroyed with `ioc` after `server`;
  // exit without unwinding so they never touch freed state.
  std::fflush(nullptr);
  std::_Exit(0);
}
