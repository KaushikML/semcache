#include "proxy/proxy_server.hpp"

#include <cmath>

#include "http/beast_response_writer.hpp"
#include "http/errors.hpp"
#include "log/log.hpp"

namespace semcache {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kJson = "application/json";

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// RAII slot in the in-flight budget.
class InflightSlot {
 public:
  InflightSlot(std::atomic<int>& counter, Gauge& gauge, int limit) : counter_(counter), gauge_(gauge) {
    acquired_ = counter_.fetch_add(1, std::memory_order_acq_rel) < limit;
    if (!acquired_) {
      counter_.fetch_sub(1, std::memory_order_acq_rel);
    } else {
      gauge_.add(1);
    }
  }
  ~InflightSlot() {
    if (acquired_) {
      counter_.fetch_sub(1, std::memory_order_acq_rel);
      gauge_.add(-1);
    }
  }
  InflightSlot(const InflightSlot&) = delete;
  InflightSlot& operator=(const InflightSlot&) = delete;
  [[nodiscard]] bool acquired() const { return acquired_; }

 private:
  std::atomic<int>& counter_;
  Gauge& gauge_;
  bool acquired_;
};

}  // namespace

ProxyServer::ProxyServer(net::io_context& ioc, const Config& cfg, Upstream& upstream, CacheServices cache,
                         MetricsRegistry& registry)
    : cfg_(cfg),
      registry_(registry),
      cache_(cache),
      metrics_(registry),
      tenants_(cfg.tenants),
      tenant_limiter_(cfg.limits.rps, cfg.limits.burst),
      pipeline_(cfg, upstream, metrics_, cache),
      listener_(ioc, tcp::endpoint(net::ip::make_address(cfg.server.host), cfg.server.port),
                [this](beast::tcp_stream s, tcp::endpoint r) { return session(std::move(s), r); }) {
  metrics_.embedder_ready.set(cache_.embedder != nullptr ? 1 : 0);
}

Task<void> ProxyServer::session(beast::tcp_stream stream, tcp::endpoint remote) {
  beast::flat_buffer buffer;
  for (;;) {
    http::request_parser<http::string_body> parser;
    parser.body_limit(cfg_.server.max_body_bytes);
    stream.expires_after(cfg_.server.read_timeout);
    auto [ec, n] = co_await http::async_read(stream, buffer, parser, use_nothrow);
    if (ec == http::error::body_limit) {
      BeastResponseWriter w(stream, 11, false, cfg_.server.write_timeout);
      metrics_.bad_requests.inc();
      co_await w.send(413, kJson,
                      openai_error_json("request body exceeds server.max_body_bytes", "invalid_request_error",
                                        "request_too_large"),
                      kNoHeaders);
      break;
    }
    if (ec) break;  // client closed, timeout, or malformed HTTP: just drop the connection
    auto req = parser.release();
    if (!co_await route(stream, req, remote)) break;
  }
  beast::error_code ignored;
  stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
}

Task<bool> ProxyServer::route(beast::tcp_stream& stream, Request& req, const tcp::endpoint& remote) {
  const auto target = std::string_view(req.target());
  const auto path = target.substr(0, target.find('?'));
  BeastResponseWriter w(stream, req.version(), req.keep_alive(), cfg_.server.write_timeout);

  if (path == "/v1/chat/completions") {
    if (req.method() != http::verb::post) {
      const HeaderList allow = {{"Allow", "POST"}};
      co_await w.send(405, kJson, openai_error_json("use POST", "invalid_request_error"), allow);
      co_return w.healthy() && req.keep_alive();
    }
    co_return co_await handle_chat(stream, req, remote);
  }
  if (path == "/healthz" && req.method() == http::verb::get) {
    co_await w.send(200, kJson, R"({"status":"ok"})", kNoHeaders);
  } else if (path == "/readyz" && req.method() == http::verb::get) {
    // Upstream is validated at startup; only the embedder can be missing. Degraded is still 200:
    // the proxy serves every request (fail open), it just cannot make semantic hits.
    co_await w.send(200, kJson,
                    cache_.embedder != nullptr ? R"({"status":"ready","degraded":[]})"
                                         : R"({"status":"degraded","degraded":["embedder"]})",
                    kNoHeaders);
  } else if (path == "/stats" && req.method() == http::verb::get) {
    co_await w.send(200, kJson, stats_json(), kNoHeaders);
  } else if (path == "/metrics" && req.method() == http::verb::get) {
    co_await w.send(200, "text/plain; version=0.0.4", registry_.render_prometheus(), kNoHeaders);
  } else {
    co_await w.send(404, kJson, openai_error_json("not found", "invalid_request_error"), kNoHeaders);
  }
  co_return w.healthy() && req.keep_alive();
}

Task<bool> ProxyServer::reject(BeastResponseWriter& w, const Request& req, std::uint64_t request_id,
                               const tcp::endpoint& remote, unsigned status, std::string_view message,
                               std::string_view code, const HeaderList& headers) {
  co_await w.send(status, kJson, openai_error_json(message, "invalid_request_error", code), headers);
  log::info("request", {{"req_id", request_id}, {"status", status}, {"code", code},
                        {"remote", remote.address().to_string()}});
  co_return w.healthy() && req.keep_alive();
}

Task<bool> ProxyServer::handle_chat(beast::tcp_stream& stream, Request& req, const tcp::endpoint& remote) {
  const auto received = Clock::now();
  const auto request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
  BeastResponseWriter w(stream, req.version(), req.keep_alive(), cfg_.server.write_timeout);

  auto tenant = tenants_.authenticate(req[http::field::authorization]);
  if (!tenant) {
    metrics_.auth_failures.inc();
    co_return co_await reject(w, req, request_id, remote, 401, "invalid or missing API key", "invalid_api_key", kNoHeaders);
  }

  if (auto d = tenant_limiter_.acquire(tenant->id); !d.allowed) {
    metrics_.rate_limited.inc();
    auto retry = std::to_string(static_cast<long long>(std::ceil(d.retry_after_s)));
    const HeaderList headers = {{"Retry-After", retry}};
    co_return co_await reject(w, req, request_id, remote, 429, "rate limit exceeded", "rate_limit_exceeded", headers);
  }

  InflightSlot slot(inflight_, metrics_.inflight, cfg_.server.max_inflight);
  if (!slot.acquired()) {
    metrics_.overloaded.inc();
    const HeaderList headers = {{"Retry-After", "1"}};
    co_return co_await reject(w, req, request_id, remote, 503, "server overloaded, retry later", "overloaded", headers);
  }

  const auto body_bytes = req.body().size();
  const auto bypass = req["X-Semcache-Bypass"];
  const bool bypass_header = bypass == "1" || bypass == "true";
  auto outcome = co_await pipeline_.handle_chat(*tenant, std::move(req.body()), bypass_header, w, received);
  metrics_.requests[static_cast<std::size_t>(outcome.result)]->inc();
  metrics_.request_seconds.observe(ms_since(received) / 1000.0);

  nlohmann::json fields = {{"req_id", request_id},
                            {"tenant", tenant->id},
                            {"status", outcome.status},
                            {"cache", to_string(outcome.result)},
                            {"streamed", outcome.streamed},
                            {"latency_ms", ms_since(received)},
                            {"upstream_ms", outcome.upstream_ms},
                            {"upstream_error", to_string(outcome.upstream_error)},
                            {"bytes_in", body_bytes}};
  if (outcome.similarity) fields["similarity"] = *outcome.similarity;
  if (!outcome.bypass_reason.empty()) fields["bypass_reason"] = outcome.bypass_reason;
  if (!outcome.store.empty()) fields["store"] = outcome.store;
  log::info("request", std::move(fields));
  co_return outcome.keep_alive && w.healthy() && req.keep_alive();
}

std::string ProxyServer::stats_json() const {
  // Snapshot for the dashboard (design §9). Never contains prompt or response text.
  nlohmann::json j;
  std::uint64_t total = 0;
  nlohmann::json by_result = nlohmann::json::object();
  for (std::size_t i = 0; i < metrics_.requests.size(); ++i) {
    const auto n = metrics_.requests[i]->value();
    by_result[std::string(kCacheResultNames[i])] = n;
    total += n;
  }
  const auto hits = metrics_.requests[static_cast<std::size_t>(CacheResult::hit)]->value();
  j["requests"] = by_result;
  j["requests_total"] = total;
  j["hit_rate"] = total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
  j["tokens_saved"] = metrics_.tokens_saved.value();
  j["inflight"] = metrics_.inflight.value();
  j["embedder_ready"] = cache_.embedder != nullptr;
  if (cache_.store != nullptr) {
    auto st = cache_.store->stats();
    j["cache"] = {{"entries", st.entries},
                  {"bytes", st.bytes},
                  {"max_bytes", cfg_.cache.max_bytes},
                  {"partitions", st.partitions},
                  // float -> double would print 0.8999999761...; config values have <= 4 decimals
                  {"similarity_threshold", std::round(cfg_.cache.similarity_threshold * 1e4) / 1e4}};
  }
  auto quantiles = [](const Histogram& h) {
    auto snap = h.snapshot();
    return nlohmann::json{{"count", snap.count},
                          {"p50_ms", snap.quantile(0.5) * 1000},
                          {"p90_ms", snap.quantile(0.9) * 1000},
                          {"p99_ms", snap.quantile(0.99) * 1000}};
  };
  j["latency"] = {{"request", quantiles(metrics_.request_seconds)},
                  {"upstream", quantiles(metrics_.upstream_seconds)},
                  {"embed", quantiles(metrics_.embed_seconds)},
                  {"lookup", quantiles(metrics_.lookup_seconds)}};
  return j.dump();
}

}  // namespace semcache
