#pragma once

#include <atomic>
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
  RequestPipeline pipeline_;
  std::atomic<int> inflight_{0};
  std::atomic<std::uint64_t> next_request_id_{1};
  Listener listener_;
};

}  // namespace semcache
