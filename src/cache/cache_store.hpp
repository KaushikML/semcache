#pragma once

// CacheStore: cached responses partitioned by context key (design §6, §7.2, ADR-007).
//
// Reads (exact_lookup, nearest) take the partition map lock (L2) shared, then one partition lock
// (L3) shared, and return shared_ptr<const CacheEntry> so callers serialize without any lock held.
// Mutations (insert, expire) serialize on the writer mutex (W) and then take L2/L3 exclusively.
// Lock order: W -> L2 -> L3; readers never take W. Phase 4 replaces W with a writer thread.
//
// Eviction: per-entry TTL (lazy on read + expire() sweep) and CLOCK under a global byte budget and
// a per-tenant quota. A hit sets the entry's atomic `referenced` bit, nothing more.

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "cache/context_key.hpp"
#include "config/config.hpp"
#include "embed/embedder.hpp"
#include "index/vector_index.hpp"
#include "metrics/metrics.hpp"

namespace semcache {

struct DigestHash {
  std::size_t operator()(const Sha256Digest& d) const noexcept {
    std::size_t h;
    std::memcpy(&h, d.data(), sizeof(h));  // already uniformly distributed
    return h;
  }
};

using CacheClock = std::chrono::system_clock;  // wall clock: entries outlive restarts (Phase 6)

struct CacheEntry {
  VectorId id = 0;
  ContextKey context{};
  PromptHash prompt_hash{};
  std::string tenant;
  std::string prompt;                   // embed text; never logged unless logging.log_prompts
  std::optional<Embedding> embedding;   // absent => exact-match only (too long, or no embedder)
  std::string response;                 // assembled chat.completion JSON
  std::int64_t total_tokens = -1;       // upstream usage; -1 if unknown
  CacheClock::time_point created;
  CacheClock::time_point expires;
  std::size_t bytes = 0;                // accounted size
  mutable std::atomic<bool> referenced{false};  // CLOCK bit, set on every hit
  mutable std::atomic<std::uint64_t> hits{0};

  [[nodiscard]] bool expired(CacheClock::time_point now) const { return now >= expires; }
};

using EntryPtr = std::shared_ptr<const CacheEntry>;

struct NewEntry {
  ContextKey context{};
  PromptHash prompt_hash{};
  std::string tenant;
  std::string prompt;
  std::optional<Embedding> embedding;
  std::string response;
  std::int64_t total_tokens = -1;
};

enum class InsertOutcome { stored, too_large, failed };  // failed: exception while applying

struct NearestMatch {
  EntryPtr entry;     // best live entry in the partition, or null if none
  float score = 0.0f;
};

using IndexFactory = std::function<std::unique_ptr<VectorIndex>(std::size_t dim)>;

class CacheStore {
 public:
  CacheStore(const CacheConfig& cfg, const std::vector<TenantConfig>& tenants, MetricsRegistry& metrics,
             IndexFactory index_factory = {});
  ~CacheStore();
  CacheStore(const CacheStore&) = delete;
  CacheStore& operator=(const CacheStore&) = delete;

  [[nodiscard]] EntryPtr exact_lookup(const ContextKey& ctx, const PromptHash& prompt, CacheClock::time_point now) const;
  [[nodiscard]] NearestMatch nearest(const ContextKey& ctx, const Embedding& q, CacheClock::time_point now) const;
  // Records a hit for CLOCK and stats.
  static void touch(const CacheEntry& e) {
    e.referenced.store(true, std::memory_order_relaxed);
    e.hits.fetch_add(1, std::memory_order_relaxed);
  }

  InsertOutcome insert(NewEntry e, CacheClock::time_point now);
  // Removes expired entries; returns how many.
  std::size_t expire(CacheClock::time_point now);

  struct Stats {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::size_t partitions = 0;
  };
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::size_t tenant_bytes(const std::string& tenant) const;

 private:
  struct Partition;
  using PartitionPtr = std::shared_ptr<Partition>;
  enum class Reason { ttl, capacity, tenant_quota, replaced };

  [[nodiscard]] PartitionPtr find_partition(const ContextKey& ctx) const;
  [[nodiscard]] std::size_t quota_for(const std::string& tenant) const;
  [[nodiscard]] CacheClock::duration ttl_for(const std::string& tenant) const;
  // Callers hold writer_mu_.
  void remove_locked(const EntryPtr& e, Reason why);
  bool evict_one_locked(const std::string* only_tenant, CacheClock::time_point now);
  void publish_gauges_locked();

  const CacheConfig cfg_;
  std::unordered_map<std::string, std::size_t> tenant_quota_;
  std::unordered_map<std::string, CacheClock::duration> tenant_ttl_;
  IndexFactory index_factory_;

  mutable std::shared_mutex map_mu_;  // L2
  std::unordered_map<ContextKey, PartitionPtr, DigestHash> partitions_;

  mutable std::mutex writer_mu_;  // W: serializes all mutations
  std::list<EntryPtr> clock_;
  std::list<EntryPtr>::iterator hand_;
  std::unordered_map<VectorId, std::list<EntryPtr>::iterator> clock_pos_;
  std::unordered_map<std::string, std::size_t> tenant_bytes_;
  VectorId next_id_ = 1;
  std::atomic<std::size_t> total_bytes_{0};
  std::atomic<std::size_t> total_entries_{0};
  std::atomic<std::size_t> partition_count_{0};

  Gauge& entries_gauge_;
  Gauge& bytes_gauge_;
  Gauge& partitions_gauge_;
  Counter& evicted_ttl_;
  Counter& evicted_capacity_;
  Counter& evicted_quota_;
  Counter& rejected_too_large_;
};

}  // namespace semcache
