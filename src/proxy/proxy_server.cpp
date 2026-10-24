#include "proxy/proxy_server.hpp"

#include <boost/asio/experimental/concurrent_channel.hpp>

#include <cmath>
#include <cstdio>

#include "http/beast_response_writer.hpp"
#include "web/assets.hpp"
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
  if (!cfg.admin_key.empty()) admin_key_digest_ = sha256(cfg.admin_key.reveal());
  if (cfg.mode == Mode::demo) {
    ip_limiter_ = std::make_unique<RateLimiter>(cfg.demo.per_ip_rps, cfg.demo.per_ip_burst,
                                                [] { return RateLimiter::Clock::now(); }, cfg.demo.max_tracked_ips);
  }
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
  } else if (path == "/dashboard" || path.starts_with("/dashboard/")) {
    co_return co_await serve_dashboard(w, req, path);
  } else if (path == "/stats/stream" && req.method() == http::verb::get) {
    co_return co_await stream_stats(w, req);
  } else if (path == "/demo/reset" && cfg_.mode == Mode::demo && req.method() == http::verb::post) {
    co_return co_await handle_demo_reset(w, req, remote);
  } else if (path == "/admin/purge" && admin_key_digest_) {
    if (req.method() != http::verb::post) {
      const HeaderList allow = {{"Allow", "POST"}};
      co_await w.send(405, kJson, openai_error_json("use POST", "invalid_request_error"), allow);
    } else {
      co_return co_await handle_purge(w, req);
    }
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

  if (double retry = 0; !ip_allowed(remote, retry)) {
    metrics_.rate_limited.inc();
    auto retry_s = std::to_string(static_cast<long long>(std::ceil(retry)));
    const HeaderList headers = {{"Retry-After", retry_s}};
    co_return co_await reject(w, req, request_id, remote, 429, "demo rate limit exceeded for this address",
                              "rate_limit_exceeded", headers);
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
  ChatOptions opts{.bypass = bypass == "1" || bypass == "true",
                   .tags = std::string(req["X-Semcache-Tags"]),
                   .max_prompt_chars = cfg_.mode == Mode::demo ? cfg_.demo.max_prompt_chars : 0};
  auto outcome = co_await pipeline_.handle_chat(*tenant, std::move(req.body()), std::move(opts), w, received);
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
  remember({std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count(),
            std::string(to_string(outcome.result)), outcome.status, ms_since(received), outcome.similarity,
            outcome.streamed});
  if (outcome.similarity) fields["similarity"] = *outcome.similarity;
  if (!outcome.bypass_reason.empty()) fields["bypass_reason"] = outcome.bypass_reason;
  if (!outcome.store.empty()) fields["store"] = outcome.store;
  log::info("request", std::move(fields));
  co_return outcome.keep_alive && w.healthy() && req.keep_alive();
}

namespace {

// Parses the purge request body; returns an error message, or empty on success.
std::string parse_purge_filter(const std::string& raw, PurgeFilter& filter) {
  auto body = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (!body.is_object()) return "body must be a JSON object";
  for (const auto& [k, v] : body.items()) {
    if (!v.is_string()) return "filter values must be strings";
    const auto value = v.get<std::string>();
    if (k == "tenant") {
      filter.tenant = value;
    } else if (k == "tag") {
      filter.tag = value;
    } else if (k == "context") {
      ContextKey ctx{};
      if (value.size() != 2 * ctx.size()) return "context must be 64 hex characters";
      for (std::size_t i = 0; i < ctx.size(); ++i) {
        unsigned byte = 0;
        if (std::sscanf(value.c_str() + 2 * i, "%2x", &byte) != 1) return "context must be hex";
        ctx[i] = static_cast<std::uint8_t>(byte);
      }
      filter.context = ctx;
    } else {
      return "unknown filter field: " + k;
    }
  }
  if (filter.empty()) return "give at least one of tenant, tag, context";
  return {};
}

}  // namespace

// Named locals before each co_await: GCC 13 ICEs on temporaries inside co_await expressions.
Task<bool> ProxyServer::handle_purge(BeastResponseWriter& w, Request& req) {
  constexpr std::string_view kBearer = "Bearer ";
  const auto auth = std::string_view(req[http::field::authorization]);
  if (!auth.starts_with(kBearer) || sha256(auth.substr(kBearer.size())) != *admin_key_digest_) {
    metrics_.auth_failures.inc();
    const auto msg = openai_error_json("invalid admin key", "invalid_request_error", "invalid_api_key");
    co_await w.send(401, kJson, msg, kNoHeaders);
    co_return w.healthy() && req.keep_alive();
  }
  PurgeFilter filter;
  if (const auto error = parse_purge_filter(req.body(), filter); !error.empty()) {
    const auto msg = openai_error_json(error, "invalid_request_error");
    co_await w.send(400, kJson, msg, kNoHeaders);
    co_return w.healthy() && req.keep_alive();
  }

  const auto result = co_await run_purge(filter);
  if (result.cancelled) co_return false;  // shutting down
  if (!result.queued) {
    const HeaderList retry = {{"Retry-After", "1"}};
    const auto msg = openai_error_json("cache writer unavailable or busy, retry later", "server_error");
    co_await w.send(503, kJson, msg, retry);
    co_return w.healthy() && req.keep_alive();
  }
  const auto purged = result.purged;
  log::info("purge", {{"purged", purged},
                      {"tenant", filter.tenant.value_or("")},
                      {"tag", filter.tag.value_or("")},
                      {"by_context", filter.context.has_value()}});
  nlohmann::json reply;
  reply["purged"] = purged;
  const auto reply_body = reply.dump();
  co_await w.send(200, kJson, reply_body, kNoHeaders);
  co_return w.healthy() && req.keep_alive();
}

Task<ProxyServer::PurgeResult> ProxyServer::run_purge(PurgeFilter filter) {
  PurgeResult r;
  // Runs on the writer thread (the only mutator); reply once applied, so later lookups cannot
  // see purged entries (design §3, consistency).
  using Done = net::experimental::concurrent_channel<void(boost::system::error_code, std::size_t)>;
  auto done = std::make_shared<Done>(co_await net::this_coro::executor, 1);
  r.queued = cache_.writer != nullptr &&
             cache_.writer->submit_purge(std::move(filter),
                                         [done](std::size_t n) { done->try_send(boost::system::error_code{}, n); });
  if (!r.queued) co_return r;
  auto received = co_await done->async_receive(use_nothrow);
  r.cancelled = static_cast<bool>(std::get<0>(received));
  r.purged = std::get<1>(received);
  co_return r;
}

bool ProxyServer::ip_allowed(const tcp::endpoint& remote, double& retry_after_s) {
  if (!ip_limiter_) return true;
  auto d = ip_limiter_->acquire(remote.address().to_string());
  retry_after_s = d.retry_after_s;
  return d.allowed;
}

Task<bool> ProxyServer::handle_demo_reset(BeastResponseWriter& w, Request& req, const tcp::endpoint& remote) {
  if (double retry = 0; !ip_allowed(remote, retry)) {
    const HeaderList headers = {{"Retry-After", std::to_string(static_cast<long long>(std::ceil(retry)))}};
    const auto msg = openai_error_json("demo rate limit exceeded for this address", "invalid_request_error",
                                       "rate_limit_exceeded");
    co_await w.send(429, kJson, msg, headers);
    co_return w.healthy() && req.keep_alive();
  }
  PurgeFilter filter;
  filter.tenant = "demo";
  const auto result = co_await run_purge(filter);
  if (result.cancelled) co_return false;
  nlohmann::json reply;
  reply["purged"] = result.purged;
  const auto body = reply.dump();
  const unsigned status = result.queued ? 200 : 503;
  co_await w.send(status, kJson, body, kNoHeaders);
  co_return w.healthy() && req.keep_alive();
}

Task<bool> ProxyServer::serve_dashboard(BeastResponseWriter& w, Request& req, std::string_view path) {
  // /dashboard and /dashboard/ are the page; /dashboard/<file> its assets (absolute URLs in HTML).
  std::string_view name = path.size() <= std::string_view("/dashboard/").size() ? "index.html"
                                                                                : path.substr(std::string_view("/dashboard/").size());
  auto file = req.method() == http::verb::get ? web::load(name, cfg_.server.dashboard_dir) : std::nullopt;
  if (!file) {
    const auto msg = openai_error_json("not found", "invalid_request_error");
    co_await w.send(404, kJson, msg, kNoHeaders);
    co_return w.healthy() && req.keep_alive();
  }
  // Same-origin only: no third-party scripts, styles, fonts or frames (ADR-010).
  const HeaderList headers = {
      {"Content-Security-Policy",
       "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; "
       "base-uri 'none'; form-action 'none'; frame-ancestors 'none'"},
      {"X-Content-Type-Options", "nosniff"},
      {"Referrer-Policy", "no-referrer"},
      {"Cache-Control", "no-cache"}};
  const auto type = web::content_type(name);
  co_await w.send(200, type, std::move(*file), headers);
  co_return w.healthy() && req.keep_alive();
}

Task<bool> ProxyServer::stream_stats(BeastResponseWriter& w, Request& req) {
  if (stream_clients_.fetch_add(1) >= cfg_.stats.max_stream_clients) {
    stream_clients_.fetch_sub(1);
    const HeaderList retry = {{"Retry-After", "5"}};
    const auto msg = openai_error_json("too many /stats/stream clients", "server_error");
    co_await w.send(503, kJson, msg, retry);
    co_return w.healthy() && req.keep_alive();
  }
  struct Release {
    std::atomic<std::size_t>& n;
    ~Release() { n.fetch_sub(1); }
  } release{stream_clients_};

  const HeaderList headers = {{"Cache-Control", "no-cache"}};
  if (!co_await w.begin_stream(200, "text/event-stream", headers)) co_return false;
  net::steady_timer timer(co_await net::this_coro::executor);
  for (;;) {
    const auto event = "data: " + stats_json() + "\n\n";
    if (!co_await w.write_chunk(event)) co_return false;  // client gone
    timer.expires_after(cfg_.stats.stream_interval);
    auto [ec] = co_await timer.async_wait(use_nothrow);
    if (ec) break;  // shutting down: the listener cancels sessions
  }
  co_await w.end_stream();
  co_return false;  // a finished event stream closes the connection
}

void ProxyServer::remember(Recent r) {
  std::lock_guard lock(recent_mu_);
  recent_.push_front(std::move(r));
  while (recent_.size() > cfg_.stats.recent_requests) recent_.pop_back();
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
  j["mode"] = cfg_.mode == Mode::demo ? "demo" : "live";
  j["uptime_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
  if (cfg_.mode == Mode::demo) {
    // Public by design in demo mode (design §10), so the dashboard can call the API.
    j["demo"] = {{"public_key", cfg_.demo.public_key}, {"max_prompt_chars", cfg_.demo.max_prompt_chars},
                 {"per_ip_rps", cfg_.demo.per_ip_rps}, {"per_ip_burst", cfg_.demo.per_ip_burst}};
  }
  {
    std::lock_guard lock(recent_mu_);
    j["recent"] = nlohmann::json::array();
    for (const auto& r : recent_) {
      nlohmann::json e = {{"at_ms", r.at_ms}, {"result", r.result}, {"status", r.status},
                          {"latency_ms", r.latency_ms}, {"streamed", r.streamed}};
      if (r.similarity) e["similarity"] = *r.similarity;
      j["recent"].push_back(std::move(e));
    }
  }
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
