// semcache-mock: deterministic OpenAI-compatible upstream for demos and benchmarks.

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mock_upstream.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: semcache-mock [--host H] [--port P] [--latency-ms N] [--token-delay-ms N]\n"
               "                     [--response-words N] [--allow-faults] [--threads N]\n"
               "                     [--usd-per-1k-input X] [--usd-per-1k-output X]\n");
}

template <class T>
bool parse(std::string_view s, T& out) {
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && p == s.data() + s.size();
}

}  // namespace

int main(int argc, char** argv) {
  using namespace semcache;
  mock::MockConfig cfg;
  int threads = 2;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&](auto& out) {
      if (i + 1 >= argc || !parse(argv[++i], out)) {
        usage();
        std::exit(2);
      }
    };
    long long ms = 0;
    if (a == "--host" && i + 1 < argc) cfg.host = argv[++i];
    else if (a == "--port") next(cfg.port);
    else if (a == "--latency-ms") { next(ms); cfg.latency = std::chrono::milliseconds(ms); }
    else if (a == "--token-delay-ms") { next(ms); cfg.token_delay = std::chrono::milliseconds(ms); }
    else if (a == "--response-words") next(cfg.response_words);
    else if (a == "--allow-faults") cfg.allow_faults = true;
    else if (a == "--threads") next(threads);
    else if (a == "--usd-per-1k-input") next(cfg.usd_per_1k_input_tokens);
    else if (a == "--usd-per-1k-output") next(cfg.usd_per_1k_output_tokens);
    else { usage(); return 2; }
  }

  net::io_context ioc(threads);
  mock::MockUpstream server(ioc, cfg);
  server.start();
  net::signal_set signals(ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const beast::error_code&, int) { ioc.stop(); });
  std::printf("semcache-mock listening on %s:%u (latency=%lldms token_delay=%lldms faults=%s)\n",
              cfg.host.c_str(), server.port(), static_cast<long long>(cfg.latency.count()),
              static_cast<long long>(cfg.token_delay.count()), cfg.allow_faults ? "on" : "off");
  std::fflush(stdout);

  std::vector<std::thread> pool;
  for (int i = 1; i < threads; ++i) pool.emplace_back([&ioc] { ioc.run(); });
  ioc.run();
  for (auto& t : pool) t.join();
  return 0;
}
