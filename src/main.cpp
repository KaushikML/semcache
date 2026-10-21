// semcache entry point: composition root. Loads config, wires modules together, runs the I/O pool.

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "cache/cache_store.hpp"
#include "cache/cache_writer.hpp"
#include "coalesce/coalescer.hpp"
#include "common/worker_pool.hpp"
#include "config/config.hpp"
#include "embed/onnx_embedder.hpp"
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

// Active TTL expiry (lazy expiry on reads handles correctness; this reclaims memory). The sweep
// itself runs on the writer thread, the only mutator of the store.
semcache::Task<void> sweep_expired(semcache::CacheWriter& writer, std::chrono::milliseconds interval,
                                   const std::atomic<bool>& stopping) {
  using namespace semcache;
  net::steady_timer timer(co_await net::this_coro::executor);
  while (!stopping.load()) {
    timer.expires_after(interval);
    auto [ec] = co_await timer.async_wait(use_nothrow);
    if (ec || stopping.load()) co_return;
    writer.submit_expire();
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

  // A missing or broken model degrades to passthrough instead of failing startup (fail open).
  std::unique_ptr<OnnxEmbedder> embedder;
  try {
    const auto t0 = std::chrono::steady_clock::now();
    embedder = std::make_unique<OnnxEmbedder>(cfg.embedder);
    log::info("embedder loaded", {{"model", cfg.embedder.model_path},
                                  {"load_ms", std::chrono::duration<double, std::milli>(
                                                  std::chrono::steady_clock::now() - t0).count()}});
  } catch (const EmbedderLoadError& e) {
    log::warn("embedder unavailable; exact-match caching only", {{"error", e.what()}});
  }

  CacheStore store(cfg.cache, cfg.tenants, registry, cfg.index);
  std::unique_ptr<WorkerPool> embed_pool;
  if (embedder) embed_pool = std::make_unique<WorkerPool>("embed", cfg.embedder.threads, cfg.embedder.queue_capacity);
  // Declared before the writer so it outlives the writer thread's after-insert callbacks.
  std::unique_ptr<Coalescer> coalescer;
  if (cfg.coalescer.enabled) coalescer = std::make_unique<Coalescer>(cfg.coalescer, cfg.upstream.max_response_bytes, registry);
  CacheWriter writer(store, cfg.cache.writer_queue_capacity, registry);
  const CacheServices cache{&store, embedder.get(), embed_pool.get(), &writer, coalescer.get()};

  std::unique_ptr<ProxyServer> server;
  try {
    server = std::make_unique<ProxyServer>(ioc, cfg, upstream, cache, registry);
  } catch (const std::exception& e) {
    log::error("failed to start listener", {{"error", e.what()}});
    return 1;
  }
  server->start();
  std::atomic<bool> stopping{false};
  // The sweeper runs on its own strand so the shutdown cancel below is delivered on its executor.
  auto sweep_strand = net::make_strand(ioc);
  net::cancellation_signal sweep_cancel;
  net::co_spawn(sweep_strand, sweep_expired(writer, cfg.cache.sweep_interval, stopping),
                net::bind_cancellation_slot(sweep_cancel.slot(), net::detached));

  // Graceful shutdown: stop accepting, cancel live sessions, wait for them to drain (bounded by
  // server.shutdown_timeout), then let run() return because no work is left.
  net::signal_set signals(ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const beast::error_code& ec, int sig) {
    if (ec) return;
    log::info("shutting down", {{"signal", sig}});
    server->stop();
    stopping = true;
    net::post(sweep_strand, [&] { sweep_cancel.emit(net::cancellation_type::terminal); });
    if (embed_pool) embed_pool->stop();  // queued embeds complete as rejected => requests fail open
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
  writer.stop();  // applies inserts still queued (persisted from Phase 6)
  log::info("stopped");
  // If the drain timed out, abandoned coroutine frames are destroyed with `ioc` after `server`;
  // exit without unwinding so they never touch freed state.
  std::fflush(nullptr);
  std::_Exit(0);
}
