#pragma once

// RequestPipeline: the one place that composes the modules to serve a chat completion
// (design §6.2): classify -> exact lookup -> embed + nearest (embed pool) -> HIT, or forward
// upstream (MISS) and store the response if cacheable. Every cache-layer failure fails open:
// the request is forwarded as BYPASS and nothing is stored.

#include <chrono>
#include <optional>
#include <string>

#include "auth/tenants.hpp"
#include "cache/cache_store.hpp"
#include "common/worker_pool.hpp"
#include "config/config.hpp"
#include "embed/embedder.hpp"
#include "http/response_writer.hpp"
#include "proxy/proxy_metrics.hpp"
#include "upstream/upstream.hpp"

namespace semcache {

// Cache-side collaborators. Any may be null: no store => every request is BYPASS; no embedder or
// pool => exact-match caching only (design §8, "embedder not loaded").
struct CacheServices {
  CacheStore* store = nullptr;
  Embedder* embedder = nullptr;
  WorkerPool* embed_pool = nullptr;
};

struct PipelineOutcome {
  CacheResult result = CacheResult::bypass;
  unsigned status = 0;
  std::optional<float> similarity;  // HIT: matched score; MISS: best score found, if a search ran
  std::string bypass_reason;        // why the request was BYPASS
  std::string store;                // MISS: "stored" or why not
  UpstreamError upstream_error = UpstreamError::none;
  double upstream_ms = 0;
  bool streamed = false;
  bool keep_alive = true;  // false when the connection must be closed (aborted stream, client gone)
};

class RequestPipeline {
 public:
  using Clock = std::chrono::steady_clock;

  RequestPipeline(const Config& cfg, Upstream& upstream, ProxyMetrics& metrics, CacheServices cache);

  Task<PipelineOutcome> handle_chat(const Tenant& tenant, std::string body, bool bypass_header, ResponseWriter& out,
                                    Clock::time_point received);

 private:
  struct Lookup;  // result of the cache lookup stage

  Task<Lookup> lookup(const CacheableRequest& req);
  Task<void> serve_hit(const CacheEntry& entry, float similarity, const nlohmann::json& request, ResponseWriter& out,
                       Clock::time_point received, PipelineOutcome& outcome);
  // Forwards upstream. When `store_as` is set, a cacheable response is stored under it.
  Task<void> forward(std::string body, CacheResult result, std::optional<NewEntry> store_as, ResponseWriter& out,
                     Clock::time_point received, PipelineOutcome& outcome);

  const Config& cfg_;
  Upstream& upstream_;
  ProxyMetrics& metrics_;
  CacheServices cache_;
};

}  // namespace semcache
