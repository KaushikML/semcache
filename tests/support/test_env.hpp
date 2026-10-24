#pragma once

// In-process test harness: mock upstream + proxy on ephemeral ports, plus a small async HTTP
// client driven on its own io_context.

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "log/log.hpp"
#include "fake_embedder.hpp"
#include "metrics/metrics.hpp"
#include "mock_upstream.hpp"
#include "net/asio.hpp"
#include "persist/persistence.hpp"
#include "proxy/proxy_server.hpp"
#include "upstream/http_upstream.hpp"

namespace semcache::test {

inline constexpr std::string_view kTestKey = "sk-sc-test-key";
inline constexpr std::string_view kOtherKey = "sk-sc-other-key";

inline mock::MockConfig fast_mock() {
  mock::MockConfig m;
  m.port = 0;
  m.latency = std::chrono::milliseconds(0);
  m.token_delay = std::chrono::milliseconds(0);
  m.response_words = 12;
  m.allow_faults = true;
  return m;
}

inline Config base_config(std::uint16_t upstream_port) {
  Config cfg;
  cfg.server.host = "127.0.0.1";
  cfg.server.port = 0;
  cfg.server.io_threads = 2;
  cfg.upstream.base_url = "http://127.0.0.1:" + std::to_string(upstream_port);
  cfg.upstream.connect_timeout = std::chrono::milliseconds(1000);
  cfg.upstream.first_byte_timeout = std::chrono::milliseconds(5000);
  cfg.upstream.idle_timeout = std::chrono::milliseconds(5000);
  cfg.upstream.total_timeout = std::chrono::milliseconds(10000);
  cfg.persistence.enabled = false;  // tests that need it pass a temp dir via the tweak
  cfg.limits.rps = 10000;
  cfg.limits.burst = 10000;
  cfg.tenants.push_back({"test", "UNUSED", sha256(kTestKey)});
  cfg.tenants.push_back({"other", "UNUSED", sha256(kOtherKey)});
  return cfg;
}

// Returns a TCP port that nothing listens on (bound then released).
inline std::uint16_t closed_port() {
  net::io_context ioc;
  tcp::acceptor a(ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  auto p = a.local_endpoint().port();
  a.close();
  return p;
}

struct ClientResponse;
struct ClientRequest;

// Which embedder the proxy gets: a deterministic fake, none (model failed to load), or one the
// test passes in (e.g. the real OnnxEmbedder).
enum class WithEmbedder { fake, none, custom };

class TestEnv {
 public:
  explicit TestEnv(mock::MockConfig mock_cfg = fast_mock(), const std::function<void(Config&)>& tweak = {},
                   WithEmbedder embedder = WithEmbedder::fake, Embedder* custom = nullptr)
      : work_(net::make_work_guard(ioc_)) {
    log::init("info");  // like main(): before any I/O thread exists (see log.hpp)
    mock_ = std::make_unique<mock::MockUpstream>(ioc_, mock_cfg);
    mock_->start();
    cfg_ = base_config(mock_->port());
    if (tweak) tweak(cfg_);
    finalize_config(cfg_);
    upstream_ = std::make_unique<HttpUpstream>(cfg_.upstream, tls_);
    store_ = std::make_unique<CacheStore>(cfg_.cache, cfg_.tenants, registry_, cfg_.index);
    if (cfg_.coalescer.enabled) {
      coalescer_ = std::make_unique<Coalescer>(cfg_.coalescer, cfg_.upstream.max_response_bytes, registry_);
    }
    if (cfg_.persistence.enabled) {
      persistence_ = std::make_unique<Persistence>(cfg_.persistence, Sha256Digest{}, registry_);
      recovery_ = persistence_->recover(*store_);
      store_->set_observer(persistence_.get());
    }
    writer_ = std::make_unique<CacheWriter>(*store_, cfg_.cache.writer_queue_capacity, registry_, persistence_.get());
    Embedder* e = embedder == WithEmbedder::fake ? &embedder_ : embedder == WithEmbedder::custom ? custom : nullptr;
    if (e != nullptr) pool_ = std::make_unique<WorkerPool>("test-embed", cfg_.embedder.threads, cfg_.embedder.queue_capacity);
    proxy_ = std::make_unique<ProxyServer>(ioc_, cfg_, *upstream_, CacheServices{store_.get(), e, pool_.get(), writer_.get(), coalescer_.get()},
                                           registry_);
    proxy_->start();
    for (int i = 0; i < 2; ++i) threads_.emplace_back([this] { ioc_.run(); });
  }

  // Drains like production shutdown: cancel sessions, wait for them, then let run() return.
  ~TestEnv() {
    proxy_->stop();
    mock_->stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (proxy_->active_sessions() + mock_->active_sessions() > 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool drained = proxy_->active_sessions() + mock_->active_sessions() == 0;
    embedder_.set_gate(false);
    if (pool_) pool_->stop();
    writer_->stop();
    work_.reset();
    if (!drained) ioc_.stop();
    for (auto& t : threads_) t.join();
    EXPECT_TRUE(drained) << "sessions did not drain within 5s";
  }

  TestEnv(const TestEnv&) = delete;
  TestEnv& operator=(const TestEnv&) = delete;

  [[nodiscard]] std::uint16_t port() const { return proxy_->port(); }
  [[nodiscard]] mock::MockUpstream& mock() { return *mock_; }
  [[nodiscard]] const Config& config() const { return cfg_; }
  [[nodiscard]] FakeEmbedder& fake_embedder() { return embedder_; }
  [[nodiscard]] CacheStore& store() { return *store_; }
  [[nodiscard]] Coalescer* coalescer() { return coalescer_.get(); }
  [[nodiscard]] CacheWriter& writer() { return *writer_; }
  [[nodiscard]] Persistence* persistence() { return persistence_.get(); }
  [[nodiscard]] const RecoveryReport& recovery() const { return recovery_; }
  // Waits until every cache insert submitted so far is applied (inserts are asynchronous).
  void flush() { writer_->flush(); }
  // After a response arrives, the proxy may still be finishing that request: submitting its insert
  // and retiring its coalescing flight happen after the last byte is written. Waits until the
  // sessions opened since `sessions_before` have ended, then until their inserts are applied, so a
  // following request is not served COALESCED from a flight that has already finished.
  void settle(std::size_t sessions_before) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (proxy_->active_sessions() > sessions_before && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    flush();
  }

  // Request helpers that also flush, so a following request sees the stored entry.
  ClientResponse chat(const std::string& prompt, bool stream = false, nlohmann::json extra = nlohmann::json::object());
  ClientResponse request(ClientRequest r);
  [[nodiscard]] std::string metrics() const { return registry_.render_prometheus(); }

 private:
  net::io_context ioc_;
  net::executor_work_guard<net::io_context::executor_type> work_;
  ssl::context tls_{make_client_tls_context(false)};
  MetricsRegistry registry_;
  FakeEmbedder embedder_;
  Config cfg_;
  std::unique_ptr<mock::MockUpstream> mock_;
  std::unique_ptr<HttpUpstream> upstream_;
  std::unique_ptr<CacheStore> store_;
  std::unique_ptr<Coalescer> coalescer_;  // before writer_: outlives its after-insert callbacks
  std::unique_ptr<Persistence> persistence_;  // before writer_: the writer's hook
  RecoveryReport recovery_;
  std::unique_ptr<CacheWriter> writer_;
  std::unique_ptr<WorkerPool> pool_;
  std::unique_ptr<ProxyServer> proxy_;
  std::vector<std::thread> threads_;
};

struct ClientResponse {
  unsigned status = 0;
  std::map<std::string, std::string> headers;  // lower-cased names
  std::string body;
  std::string error;  // transport error while reading the body, if any
  double header_ms = 0;
  double total_ms = 0;

  [[nodiscard]] std::string header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? std::string{} : it->second;
  }
};

struct ClientRequest {
  http::verb method = http::verb::post;
  std::string target = "/v1/chat/completions";
  std::string body;
  std::string authorization = "Bearer " + std::string(kTestKey);
  HeaderList headers;
};

inline Task<ClientResponse> request_async(std::uint16_t port, ClientRequest r) {
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };
  ClientResponse out;
  auto ex = co_await net::this_coro::executor;
  beast::tcp_stream stream(ex);
  stream.expires_after(std::chrono::seconds(10));
  auto [cec] = co_await stream.async_connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port), use_nothrow);
  if (cec) {
    out.error = "connect: " + cec.message();
    co_return out;
  }
  http::request<http::string_body> req{r.method, r.target, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!r.authorization.empty()) req.set(http::field::authorization, r.authorization);
  req.set(http::field::content_type, "application/json");
  for (const auto& [k, v] : r.headers) req.set(k, v);
  req.body() = r.body;
  req.prepare_payload();
  const auto start = Clock::now();
  auto [wec, wn] = co_await http::async_write(stream, req, use_nothrow);
  if (wec) {
    out.error = "write: " + wec.message();
    co_return out;
  }
  beast::flat_buffer buf;
  http::response_parser<http::string_body> parser;
  parser.body_limit(64 << 20);
  auto [hec, hn] = co_await http::async_read_header(stream, buf, parser, use_nothrow);
  if (hec) {
    out.error = "read header: " + hec.message();
    co_return out;
  }
  out.header_ms = ms(start);
  out.status = parser.get().result_int();
  for (const auto& f : parser.get()) {
    std::string name(f.name_string());
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out.headers[name] = std::string(f.value());
  }
  auto [bec, bn] = co_await http::async_read(stream, buf, parser, use_nothrow);
  if (bec) out.error = "read body: " + bec.message();
  out.body = parser.get().body();
  out.total_ms = ms(start);
  co_return out;
}

// Runs `count` requests concurrently and returns their responses in order.
inline std::vector<ClientResponse> request_many(std::uint16_t port, const std::vector<ClientRequest>& reqs) {
  net::io_context ctx;
  std::vector<std::future<ClientResponse>> futures;
  for (const auto& r : reqs) futures.push_back(net::co_spawn(ctx, request_async(port, r), net::use_future));
  ctx.run();
  std::vector<ClientResponse> out;
  for (auto& f : futures) out.push_back(f.get());
  return out;
}

inline ClientResponse request(std::uint16_t port, ClientRequest r) { return request_many(port, {std::move(r)}).front(); }

inline std::string chat_body(const std::string& prompt, bool stream = false, nlohmann::json extra = nlohmann::json::object()) {
  nlohmann::json j = {{"model", "mock-model"},
                      {"messages", nlohmann::json::array({{{"role", "user"}, {"content", prompt}}})},
                      {"stream", stream}};
  for (auto& [k, v] : extra.items()) j[k] = v;
  return j.dump();
}

inline ClientResponse chat(std::uint16_t port, const std::string& prompt, bool stream = false,
                           nlohmann::json extra = nlohmann::json::object()) {
  return request(port, {.body = chat_body(prompt, stream, std::move(extra))});
}

// Parses an SSE body into the JSON payloads of its data events; "[DONE]" becomes the string "[DONE]".
inline std::vector<nlohmann::json> sse_events(const std::string& body) {
  std::vector<nlohmann::json> out;
  std::size_t pos = 0;
  while ((pos = body.find("data: ", pos)) != std::string::npos) {
    auto end = body.find("\n\n", pos);
    auto payload = body.substr(pos + 6, end == std::string::npos ? std::string::npos : end - pos - 6);
    out.push_back(payload == "[DONE]" ? nlohmann::json("[DONE]") : nlohmann::json::parse(payload, nullptr, false));
    if (end == std::string::npos) break;
    pos = end + 2;
  }
  return out;
}

inline std::string assembled_content(const std::vector<nlohmann::json>& events) {
  std::string out;
  for (const auto& e : events) {
    if (!e.is_object() || !e.contains("choices")) continue;
    for (const auto& c : e["choices"]) {
      if (c.contains("delta") && c["delta"].contains("content")) out += c["delta"]["content"].get<std::string>();
    }
  }
  return out;
}

inline ClientResponse TestEnv::chat(const std::string& prompt, bool stream, nlohmann::json extra) {
  const auto before = proxy_->active_sessions();
  auto r = test::chat(port(), prompt, stream, std::move(extra));
  settle(before);
  return r;
}

inline ClientResponse TestEnv::request(ClientRequest r) {
  const auto before = proxy_->active_sessions();
  auto res = test::request(port(), std::move(r));
  settle(before);
  return res;
}

}  // namespace semcache::test
