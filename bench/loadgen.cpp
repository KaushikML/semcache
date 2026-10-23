// Closed-loop HTTP load generator for semcache (Phase 7). C workers, each on its own keep-alive
// connection, send chat completions back to back and record latency and X-Semcache per request.
//
// Modes (prompts from --prompts, a JSONL file of {"text": ...}):
//   cycle  worker w sends prompts[w], prompts[w + C], ... repeatedly        -> exact HITs once warm
//   case   a random prompt with random letter case each time: the uncased tokenizer gives the
//          same embedding, the prompt hash differs                          -> semantic HITs (sim 1.0)
//   once   every prompt exactly once across all workers, then stop         -> MISSes / prepopulation
//   herd   rounds: all C workers send prompts[r] at the same time          -> coalescing
//   mix    per request: exact repeat / case variant / a new prompt from --new-prompts, by --mix
// Usage: semcache_loadgen --port P --key K --prompts F --mode M [--concurrency 32] [--duration-s 15]
//                         [--stream] [--new-prompts F] [--mix 0.5,0.3,0.2] [--rounds N] [--out F]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "net/asio.hpp"

namespace {

using namespace semcache;
using Clock = std::chrono::steady_clock;

struct Sample {
  double ms = 0;
  unsigned status = 0;
  std::string result;  // X-Semcache, or "ERROR"
};

struct Options {
  std::uint16_t port = 8080;
  std::string key;
  std::string mode = "cycle";
  int concurrency = 32;
  double duration_s = 15;
  bool stream = false;
  double mix_exact = 0.5;
  double mix_case = 0.3;
  int rounds = 20;
  std::vector<std::string> prompts;
  std::vector<std::string> new_prompts;
};

std::vector<std::string> read_prompts(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) out.push_back(nlohmann::json::parse(line).at("text").get<std::string>());
  return out;
}

std::string random_case(const std::string& s, std::mt19937& rng) {
  std::string out = s;
  for (auto& c : out) {
    if (std::isalpha(static_cast<unsigned char>(c)) && (rng() & 1)) {
      c = static_cast<char>(std::isupper(static_cast<unsigned char>(c)) ? std::tolower(c) : std::toupper(c));
    }
  }
  return out;
}

std::string body_for(const std::string& prompt, bool stream) {
  return nlohmann::json{{"model", "mock-model"},
                        {"stream", stream},
                        {"messages", nlohmann::json::array({{{"role", "user"}, {"content", prompt}}})}}
      .dump();
}

// One request on a (re)usable connection. Returns false if the connection must be reopened.
Task<bool> one_request(beast::tcp_stream& s, beast::flat_buffer& buf, const Options& o, const std::string& prompt,
                       Sample& out) {
  http::request<http::string_body> req{http::verb::post, "/v1/chat/completions", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::authorization, "Bearer " + o.key);
  req.set(http::field::content_type, "application/json");
  req.keep_alive(true);
  req.body() = body_for(prompt, o.stream);
  req.prepare_payload();
  const auto t0 = Clock::now();
  s.expires_after(std::chrono::seconds(60));
  auto [wec, wn] = co_await http::async_write(s, req, use_nothrow);
  if (wec) co_return false;
  http::response<http::string_body> res;
  auto [rec, rn] = co_await http::async_read(s, buf, res, use_nothrow);
  if (rec) co_return false;
  out.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  out.status = res.result_int();
  out.result = std::string(res["X-Semcache"]);
  co_return res.keep_alive();
}

// cycle / case / once / mix: closed loop until the deadline (or until `once` runs out).
Task<void> worker(int w, const Options& o, Clock::time_point deadline, std::atomic<std::size_t>& once_next,
                  std::vector<Sample>& samples) {
  auto ex = co_await net::this_coro::executor;
  std::mt19937 rng(static_cast<std::uint32_t>(w * 7919 + 1));
  std::uniform_real_distribution<double> u(0, 1);
  beast::tcp_stream s(ex);
  beast::flat_buffer buf;
  bool connected = false;
  std::size_t k = 0;
  std::size_t new_next = static_cast<std::size_t>(w);
  for (;;) {
    std::string prompt;
    if (o.mode == "once") {
      const auto i = once_next.fetch_add(1);
      if (i >= o.prompts.size()) break;
      prompt = o.prompts[i];
    } else {
      if (Clock::now() >= deadline) break;
      if (o.mode == "cycle") {
        prompt = o.prompts[(static_cast<std::size_t>(w) + k * static_cast<std::size_t>(o.concurrency)) % o.prompts.size()];
      } else if (o.mode == "case") {
        prompt = random_case(o.prompts[rng() % o.prompts.size()], rng);
      } else {  // mix
        const double r = u(rng);
        if (r < o.mix_exact || o.new_prompts.empty()) {
          prompt = o.prompts[rng() % o.prompts.size()];
        } else if (r < o.mix_exact + o.mix_case) {
          prompt = random_case(o.prompts[rng() % o.prompts.size()], rng);
        } else {
          prompt = o.new_prompts[new_next % o.new_prompts.size()];
          new_next += static_cast<std::size_t>(o.concurrency);
        }
      }
    }
    if (!connected) {
      s.expires_after(std::chrono::seconds(10));
      auto [ec] = co_await s.async_connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), o.port), use_nothrow);
      if (ec) {
        samples.push_back({0, 0, "ERROR"});
        ++k;
        continue;
      }
      connected = true;
    }
    Sample smp;
    if (!co_await one_request(s, buf, o, prompt, smp)) {
      beast::error_code ignored;
      s.socket().close(ignored);
      buf.clear();
      connected = false;
      if (smp.status == 0) smp.result = "ERROR";
    }
    samples.push_back(std::move(smp));
    ++k;
  }
}

// Reusable barrier: herd rounds start only when every worker finished the previous one.
class Barrier {
 public:
  explicit Barrier(int n) : n_(n) {}
  void arrive_and_wait() {
    std::unique_lock lk(mu_);
    const int gen = generation_;
    if (++count_ == n_) {
      count_ = 0;
      ++generation_;
      cv_.notify_all();
    } else {
      cv_.wait(lk, [&] { return generation_ != gen; });
    }
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  int count_ = 0;
  int generation_ = 0;
  int n_;
};

// herd: every round, all workers send the same new prompt at once.
Task<void> herd_worker(const Options& o, Barrier& barrier, std::vector<Sample>& samples) {
  auto ex = co_await net::this_coro::executor;
  beast::tcp_stream s(ex);
  beast::flat_buffer buf;
  bool connected = false;
  for (int r = 0; r < o.rounds; ++r) {
    const auto prompt = o.prompts[static_cast<std::size_t>(r) % o.prompts.size()] + " (round " + std::to_string(r) + ")";
    if (!connected) {
      s.expires_after(std::chrono::seconds(10));
      auto [ec] = co_await s.async_connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), o.port), use_nothrow);
      connected = !ec;
    }
    Sample smp;
    if (!connected || !co_await one_request(s, buf, o, prompt, smp)) {
      beast::error_code ignored;
      s.socket().close(ignored);
      buf.clear();
      connected = false;
      if (smp.status == 0) smp.result = "ERROR";
    }
    samples.push_back(std::move(smp));
    barrier.arrive_and_wait();  // blocks this worker's own thread only
  }
}

nlohmann::json summarize(std::vector<double> v) {
  if (v.empty()) return nlohmann::json::object();
  std::sort(v.begin(), v.end());
  auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())))]; };
  double sum = 0;
  for (double x : v) sum += x;
  return {{"count", v.size()}, {"p50_ms", pct(0.50)}, {"p90_ms", pct(0.90)}, {"p99_ms", pct(0.99)},
          {"mean_ms", sum / static_cast<double>(v.size())}, {"max_ms", v.back()}};
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  std::string out_path;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--port") o.port = static_cast<std::uint16_t>(std::stoi(next()));
    else if (a == "--key") o.key = next();
    else if (a == "--prompts") o.prompts = read_prompts(next());
    else if (a == "--new-prompts") o.new_prompts = read_prompts(next());
    else if (a == "--mode") o.mode = next();
    else if (a == "--concurrency") o.concurrency = std::stoi(next());
    else if (a == "--duration-s") o.duration_s = std::stod(next());
    else if (a == "--rounds") o.rounds = std::stoi(next());
    else if (a == "--stream") o.stream = true;
    else if (a == "--out") out_path = next();
    else if (a == "--mix") {
      auto v = next();
      std::sscanf(v.c_str(), "%lf,%lf", &o.mix_exact, &o.mix_case);
    }
  }
  if (o.prompts.empty() || o.key.empty()) {
    std::fprintf(stderr, "need --prompts and --key\n");
    return 2;
  }

  std::vector<std::vector<Sample>> per_worker(static_cast<std::size_t>(o.concurrency));
  std::atomic<std::size_t> once_next{0};
  const auto start = Clock::now();
  const auto deadline = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.duration_s));
  net::io_context ioc;
  Barrier barrier(o.concurrency);
  std::vector<std::thread> threads;
  if (o.mode == "herd") {
    // One io_context per worker, so a barrier wait blocks only that worker.
    for (int w = 0; w < o.concurrency; ++w) {
      threads.emplace_back([&, w] {
        net::io_context own;
        net::co_spawn(own, herd_worker(o, barrier, per_worker[static_cast<std::size_t>(w)]), net::detached);
        own.run();
      });
    }
  } else {
    for (int w = 0; w < o.concurrency; ++w) {
      net::co_spawn(ioc, worker(w, o, deadline, once_next, per_worker[static_cast<std::size_t>(w)]), net::detached);
    }
    for (int t = 0; t < 2; ++t) threads.emplace_back([&] { ioc.run(); });
  }
  for (auto& t : threads) t.join();
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

  std::map<std::string, std::vector<double>> by_result;
  std::vector<double> all;
  std::size_t errors = 0;
  std::size_t non200 = 0;
  for (const auto& ws : per_worker) {
    for (const auto& s : ws) {
      if (s.result == "ERROR" || s.status == 0) {
        ++errors;
        continue;
      }
      if (s.status != 200) ++non200;
      all.push_back(s.ms);
      by_result[s.result.empty() ? "NONE" : s.result].push_back(s.ms);
    }
  }
  nlohmann::json j = {{"mode", o.mode}, {"concurrency", o.concurrency}, {"stream", o.stream}, {"elapsed_s", elapsed},
                      {"requests", all.size()}, {"errors", errors}, {"non_200", non200},
                      {"rps", static_cast<double>(all.size()) / elapsed}, {"latency", summarize(all)}};
  for (auto& [k, v] : by_result) j["by_result"][k] = summarize(v);
  const auto text = j.dump(2);
  if (!out_path.empty()) std::ofstream(out_path) << text;
  std::printf("%s\n", text.c_str());
  return errors == 0 ? 0 : 1;
}
