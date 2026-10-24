#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include "auth/tenants.hpp"
#include "http/beast_response_writer.hpp"
#include "config/config.hpp"
#include "limits/rate_limiter.hpp"
#include "metrics/metrics.hpp"
#include "net/listener.hpp"
#include "proxy/pipeline.hpp"
#include "proxy/proxy_metrics.hpp"
#include "upstream/upstream.hpp"

namespace semcache {

// HTTP front-end: connection handling, routing, auth, limits. Delegates chat completions to the
// RequestPipeline.
class ProxyServer {
 public:
  // `cache.embedder` is null when the model failed to load: the proxy still serves (fail open,
  // exact-match caching only) and /readyz reports the embedder as degraded.
  ProxyServer(net::io_context& ioc, const Config& cfg, Upstream& upstream, CacheServices cache,
              MetricsRegistry& registry);

  [[nodiscard]] std::uint16_t port() const { return listener_.port(); }
  void start() { listener_.start(); }
  void stop() { listener_.stop(); }
  [[nodiscard]] std::size_t active_sessions() const { return listener_.active_sessions(); }

 private:
  using Request = http::request<http::string_body>;

  Task<void> session(beast::tcp_stream stream, tcp::endpoint remote);
  // Returns whether the connection may be kept alive.
  Task<bool> route(beast::tcp_stream& stream, Request& req, const tcp::endpoint& remote);
  Task<bool> handle_chat(beast::tcp_stream& stream, Request& req, const tcp::endpoint& remote);
  // POST /admin/purge {"tenant"?, "tag"?, "context"? (64 hex)} with the admin key (design §10).
  Task<bool> handle_purge(BeastResponseWriter& w, Request& req);
  // POST /demo/reset (demo mode): purges the public demo tenant's entries.
  Task<bool> handle_demo_reset(BeastResponseWriter& w, Request& req, const tcp::endpoint& remote);
  // GET /dashboard[/<file>]: the static dashboard, with a strict Content-Security-Policy.
  Task<bool> serve_dashboard(BeastResponseWriter& w, Request& req, std::string_view path);
  // GET /stats/stream: /stats as server-sent events every stats.stream_interval_ms.
  Task<bool> stream_stats(BeastResponseWriter& w, Request& req);

  struct PurgeResult {
    bool queued = false;     // false: no writer, or its queue is full
    bool cancelled = false;  // shutting down
    std::size_t purged = 0;
  };
  // Runs a purge on the writer thread and waits until it has been applied (design §3).
  Task<PurgeResult> run_purge(PurgeFilter filter);
  // Demo mode: per-IP token bucket; false => answered 429.
  [[nodiscard]] bool ip_allowed(const tcp::endpoint& remote, double& retry_after_s);

  struct Recent {  // one request in the /stats ring: never prompt or response text
    std::int64_t at_ms = 0;
    std::string result;
    unsigned status = 0;
    double latency_ms = 0;
    std::optional<float> similarity;
    bool streamed = false;
  };
  void remember(Recent r);
  [[nodiscard]] std::string stats_json() const;
  Task<bool> reject(BeastResponseWriter& w, const Request& req, std::uint64_t request_id,
                    const tcp::endpoint& remote, unsigned status, std::string_view message,
                    std::string_view code, const HeaderList& headers);

  const Config& cfg_;
  MetricsRegistry& registry_;
  CacheServices cache_;
  ProxyMetrics metrics_;
  TenantDirectory tenants_;
  std::optional<Sha256Digest> admin_key_digest_;  // unset => /admin/* disabled (404)
  RateLimiter tenant_limiter_;
  std::unique_ptr<RateLimiter> ip_limiter_;  // demo mode only
  mutable std::mutex recent_mu_;
  std::deque<Recent> recent_;
  std::atomic<std::size_t> stream_clients_{0};
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  RequestPipeline pipeline_;
  std::atomic<int> inflight_{0};
  std::atomic<std::uint64_t> next_request_id_{1};
  Listener listener_;
};

}  // namespace semcache
