#pragma once

// RequestPipeline: the one place that composes the modules to serve a chat completion.
// Phase 1: validate and pass through to upstream (every request is BYPASS).

#include <chrono>
#include <string>

#include "auth/tenants.hpp"
#include "config/config.hpp"
#include "http/response_writer.hpp"
#include "proxy/proxy_metrics.hpp"
#include "upstream/upstream.hpp"

namespace semcache {

struct PipelineOutcome {
  CacheResult result = CacheResult::bypass;
  unsigned status = 0;
  UpstreamError upstream_error = UpstreamError::none;
  double upstream_ms = 0;
  bool streamed = false;
  bool keep_alive = true;  // false when the connection must be closed (aborted stream, client gone)
};

class RequestPipeline {
 public:
  using Clock = std::chrono::steady_clock;

  RequestPipeline(const Config& cfg, Upstream& upstream, ProxyMetrics& metrics);

  Task<PipelineOutcome> handle_chat(const Tenant& tenant, std::string body, ResponseWriter& out,
                                    Clock::time_point received);

 private:
  const Config& cfg_;
  Upstream& upstream_;
  ProxyMetrics& metrics_;
};

}  // namespace semcache
