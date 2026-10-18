#pragma once

#include "config/config.hpp"
#include "upstream/upstream.hpp"

namespace semcache {

// HTTP/1.1 client for the configured upstream. One connection per request in v1: simple and
// correct; a keep-alive pool is added only if the Phase 7 load tests show connection setup
// (mainly the TLS handshake) matters. Timeouts per design §8.
class HttpUpstream final : public Upstream {
 public:
  HttpUpstream(const UpstreamConfig& cfg, ssl::context& tls_ctx);

  Task<UpstreamResult> send(const UpstreamRequest& request, UpstreamSink& sink) override;

 private:
  template <class Stream>
  Task<UpstreamResult> exchange(Stream& stream, const UpstreamRequest& request, UpstreamSink& sink,
                                std::chrono::steady_clock::time_point start);

  const UpstreamConfig& cfg_;
  ssl::context& tls_ctx_;
};

// Builds a client TLS context using the system trust store.
[[nodiscard]] ssl::context make_client_tls_context(bool verify_peer);

}  // namespace semcache
