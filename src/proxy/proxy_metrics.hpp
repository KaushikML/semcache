#pragma once

#include <array>

#include "cache/context_key.hpp"
#include "metrics/metrics.hpp"
#include "upstream/upstream.hpp"

namespace semcache {

// How a chat request was served. Values map to the X-Semcache header.
enum class CacheResult { hit, miss, coalesced, bypass };
inline constexpr std::array<std::string_view, 4> kCacheResultNames = {"HIT", "MISS", "COALESCED", "BYPASS"};
[[nodiscard]] inline std::string_view to_string(CacheResult r) { return kCacheResultNames[static_cast<std::size_t>(r)]; }

// All metrics the proxy exports, registered once at startup. Names follow design §9.
struct ProxyMetrics {
  explicit ProxyMetrics(MetricsRegistry& r);

  std::array<Counter*, 4> requests{};  // indexed by CacheResult
  Counter& bad_requests;
  Counter& auth_failures;
  Counter& rate_limited;
  Counter& overloaded;
  Counter& client_disconnects;
  std::array<Counter*, 6> upstream_errors{};  // indexed by UpstreamError; [none] stays null
  std::array<Counter*, 5> upstream_responses{};  // 1xx..5xx
  Histogram& request_seconds;
  Histogram& upstream_seconds;
  Gauge& inflight;
  Gauge& embedder_ready;

  // Cache path (design §9).
  std::array<Counter*, 7> bypass{};  // indexed by NotCacheable
  Histogram& lookup_seconds;         // vector search only
  Histogram& embed_seconds;          // tokenize + ORT Run
  Histogram& best_similarity;        // best score per semantic lookup, for threshold tuning (Phase 7)
  Counter& tokens_saved;
  Counter& embed_errors;
  Counter& embed_rejected;
  Counter& internal_errors;
  Counter& coalesce_independent;  // duplicates served alone: follower cap reached, or leader vanished

  Counter& store_skipped(std::string_view reason) {
    return registry_.counter("semcache_store_skipped_total", "Cacheable responses not stored",
                             "reason=\"" + std::string(reason) + "\"");
  }

  void count_upstream_status(unsigned status) {
    if (status >= 100 && status < 600) upstream_responses[status / 100 - 1]->inc();
  }

 private:
  MetricsRegistry& registry_;
};

inline ProxyMetrics::ProxyMetrics(MetricsRegistry& r)
    : bad_requests(r.counter("semcache_bad_requests_total", "Requests rejected as malformed")),
      auth_failures(r.counter("semcache_auth_failures_total", "Requests with a missing or unknown API key")),
      rate_limited(r.counter("semcache_rejected_total", "Requests rejected by limits", R"(reason="rate_limit")")),
      overloaded(r.counter("semcache_rejected_total", "Requests rejected by limits", R"(reason="overload")")),
      client_disconnects(r.counter("semcache_client_disconnects_total", "Clients that went away mid-response")),
      request_seconds(r.histogram("semcache_request_seconds", "Time from request read to response headers",
                                  latency_buckets_seconds())),
      upstream_seconds(r.histogram("semcache_upstream_seconds", "Upstream exchange duration",
                                   latency_buckets_seconds())),
      inflight(r.gauge("semcache_inflight", "Chat requests currently being handled")),
      embedder_ready(r.gauge("semcache_embedder_ready", "1 if the embedding model is loaded")),
      lookup_seconds(r.histogram("semcache_lookup_seconds", "Vector search time per lookup", latency_buckets_seconds())),
      embed_seconds(r.histogram("semcache_embed_seconds", "Embedding time per prompt", latency_buckets_seconds())),
      best_similarity(r.histogram("semcache_best_similarity", "Best cosine similarity found per semantic lookup",
                                  {0.5, 0.6, 0.7, 0.75, 0.8, 0.85, 0.9, 0.925, 0.95, 0.975, 0.99, 1.0})),
      tokens_saved(r.counter("semcache_tokens_saved_total", "Upstream tokens (usage.total_tokens) served from cache")),
      embed_errors(r.counter("semcache_embed_errors_total", "Embedding failures (request failed open)")),
      embed_rejected(r.counter("semcache_embed_queue_rejected_total", "Embed queue full (request failed open)")),
      internal_errors(r.counter("semcache_cache_internal_errors_total", "Cache-layer exceptions (request failed open)")),
      coalesce_independent(r.counter("semcache_coalesce_independent_total",
                                     "Duplicate requests served without coalescing (follower cap, leader gone)")),
      registry_(r) {
  for (std::size_t i = 0; i < bypass.size(); ++i) {
    bypass[i] = &r.counter("semcache_bypass_total", "Requests not eligible for caching",
                           "reason=\"" + std::string(to_string(static_cast<NotCacheable>(i))) + "\"");
  }
  for (std::size_t i = 0; i < requests.size(); ++i) {
    requests[i] = &r.counter("semcache_requests_total", "Chat completion requests by cache result",
                             "result=\"" + std::string(kCacheResultNames[i]) + "\"");
  }
  for (std::size_t i = 1; i < upstream_errors.size(); ++i) {
    upstream_errors[i] = &r.counter("semcache_upstream_errors_total", "Failed upstream exchanges",
                                    "kind=\"" + std::string(to_string(static_cast<UpstreamError>(i))) + "\"");
  }
  for (std::size_t i = 0; i < upstream_responses.size(); ++i) {
    upstream_responses[i] = &r.counter("semcache_upstream_responses_total", "Upstream responses by status class",
                                       "class=\"" + std::to_string(i + 1) + "xx\"");
  }
}

}  // namespace semcache
