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
#include <vector>

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
  std::vector<std::string> tags;        // purge handles (X-Semcache-Tags)
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
  std::vector<std::string> tags;
  // When the request that produced it arrived: a purge after this point drops the insert, so a
  // response that was in flight during a purge cannot bring purged content back.
  std::optional<CacheClock::time_point> request_started;
  // Recovery only: keep the original timestamps instead of now / now + TTL.
  std::optional<CacheClock::time_point> created;
  std::optional<CacheClock::time_point> expires;
};

// Selects entries to purge; every set field must match. At least one field must be set.
struct PurgeFilter {
  std::optional<std::string> tenant;
  std::optional<std::string> tag;
  std::optional<ContextKey> context;

  [[nodiscard]] bool empty() const { return !tenant && !tag && !context; }
  [[nodiscard]] bool matches(const std::string& entry_tenant, const std::vector<std::string>& entry_tags,
                             const ContextKey& entry_context) const;
};

// Notified of every applied mutation, on the writer thread, in order (persistence: the WAL).
// Replacing an entry with the same (context, prompt) is reported as an insert only.
class MutationObserver {
 public:
  virtual ~MutationObserver() = default;
  virtual void on_insert(const CacheEntry& e) = 0;
  virtual void on_remove(const CacheEntry& e) = 0;
};

// failed: exception while applying; purged: a purge newer than the request matched; expired:
// a recovered entry whose TTL already passed.
enum class InsertOutcome { stored, too_large, failed, purged, expired };

struct NearestMatch {
  EntryPtr entry;     // best live entry in the partition, or null if none
  float score = 0.0f;
};

class CacheStore {
 public:
  // `index` picks each partition's VectorIndex: brute force, HNSW, or auto (brute force until the
  // partition reaches index.hnsw_min_partition_size, then HNSW). Index (re)builds run on the
  // writer, outside the partition lock, and are swapped in afterwards.
  CacheStore(const CacheConfig& cfg, const std::vector<TenantConfig>& tenants, MetricsRegistry& metrics,
             IndexConfig index = {});
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
  // Removes every entry matching `f`; returns how many. Inserts from requests that started before
  // `now` and match `f` are dropped for cache.purge_guard_window afterwards.
  std::size_t purge(const PurgeFilter& f, CacheClock::time_point now);
  // Removes the entry for (context, prompt) if present, without counting it as an eviction.
  // Recovery uses it to replay WAL deletes.
  bool erase(const ContextKey& ctx, const PromptHash& prompt);
  // Every live entry, for snapshots. Cheap: entries are immutable and shared.
  [[nodiscard]] std::vector<EntryPtr> all_entries() const;
  // Set once, before the writer starts (not while mutations run).
  void set_observer(MutationObserver* o) { observer_ = o; }

  struct Stats {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::size_t partitions = 0;
    std::size_t hnsw_partitions = 0;
  };
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::size_t tenant_bytes(const std::string& tenant) const;

 private:
  struct Partition;
  using PartitionPtr = std::shared_ptr<Partition>;
  enum class Reason { ttl, capacity, tenant_quota, purge, replaced };

  [[nodiscard]] PartitionPtr find_partition(const ContextKey& ctx) const;
  [[nodiscard]] std::size_t quota_for(const std::string& tenant) const;
  [[nodiscard]] CacheClock::duration ttl_for(const std::string& tenant) const;
  // Callers hold writer_mu_.
  void remove_locked(const EntryPtr& e, Reason why);
  bool evict_one_locked(const std::string* only_tenant, CacheClock::time_point now);
  void publish_gauges_locked();
  [[nodiscard]] std::unique_ptr<VectorIndex> make_index(std::size_t vectors) const;
  // Rebuilds the partitions queued by insert/remove (auto upgrade, too many tombstones).
  void rebuild_pending_locked();
  void rebuild_locked(const PartitionPtr& p);

  const CacheConfig cfg_;
  std::unordered_map<std::string, std::size_t> tenant_quota_;
  std::unordered_map<std::string, CacheClock::duration> tenant_ttl_;
  const IndexConfig index_cfg_;

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
  std::atomic<std::size_t> hnsw_count_{0};
  std::vector<std::weak_ptr<Partition>> pending_rebuild_;
  struct RecentPurge {
    PurgeFilter filter;
    CacheClock::time_point at;
  };
  std::vector<RecentPurge> recent_purges_;
  MutationObserver* observer_ = nullptr;

  Gauge& entries_gauge_;
  Gauge& bytes_gauge_;
  Gauge& partitions_gauge_;
  Counter& evicted_ttl_;
  Counter& evicted_capacity_;
  Counter& evicted_quota_;
  Counter& rejected_too_large_;
  Gauge& hnsw_gauge_;
  Counter& rebuilds_;
  Counter& purged_;
  Histogram& rebuild_seconds_;
};

}  // namespace semcache
