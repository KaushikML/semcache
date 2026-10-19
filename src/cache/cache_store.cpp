#include "cache/cache_store.hpp"

#include <algorithm>

#include "index/brute_force_index.hpp"

namespace semcache {
namespace {

// Per-entry bookkeeping not covered by string/vector payloads: ids, keys, timestamps, map and list
// nodes. Design §5.1 estimate (metadata ~200 B + container overhead ~150 B).
constexpr std::size_t kEntryOverheadBytes = 350;
// Neighbours fetched per lookup, so a few expired-but-unswept entries do not hide a live match.
constexpr std::size_t kSearchCandidates = 8;

std::size_t accounted_bytes(const NewEntry& e) {
  return kEntryOverheadBytes + e.tenant.size() + e.prompt.size() + e.response.size() +
         (e.embedding ? sizeof(Embedding) : 0);
}

}  // namespace

struct CacheStore::Partition {
  mutable std::shared_mutex mu;  // L3
  std::unordered_map<VectorId, EntryPtr> entries;
  std::unordered_map<PromptHash, VectorId, DigestHash> exact;
  std::unique_ptr<VectorIndex> index;
};

CacheStore::CacheStore(const CacheConfig& cfg, const std::vector<TenantConfig>& tenants, MetricsRegistry& m,
                       IndexFactory index_factory)
    : cfg_(cfg),
      index_factory_(index_factory ? std::move(index_factory)
                                   : IndexFactory([](std::size_t dim) { return std::make_unique<BruteForceIndex>(dim); })),
      hand_(clock_.end()),
      entries_gauge_(m.gauge("semcache_index_entries", "Cached entries")),
      bytes_gauge_(m.gauge("semcache_index_bytes", "Accounted bytes of cached entries (budget: cache.max_bytes)")),
      partitions_gauge_(m.gauge("semcache_partitions", "Context partitions")),
      evicted_ttl_(m.counter("semcache_evictions_total", "Entries removed by the cache", R"(reason="ttl")")),
      evicted_capacity_(m.counter("semcache_evictions_total", "Entries removed by the cache", R"(reason="capacity")")),
      evicted_quota_(m.counter("semcache_evictions_total", "Entries removed by the cache", R"(reason="tenant_quota")")),
      rejected_too_large_(m.counter("semcache_store_skipped_total", "Cacheable responses not stored",
                                    R"(reason="entry_over_budget")")) {
  for (const auto& t : tenants) {
    if (t.max_bytes) tenant_quota_[t.id] = *t.max_bytes;
    if (t.ttl) tenant_ttl_[t.id] = *t.ttl;
  }
}

CacheStore::~CacheStore() = default;

std::size_t CacheStore::quota_for(const std::string& tenant) const {
  auto it = tenant_quota_.find(tenant);
  return it == tenant_quota_.end() ? cfg_.max_bytes : std::min(it->second, cfg_.max_bytes);
}

CacheClock::duration CacheStore::ttl_for(const std::string& tenant) const {
  auto it = tenant_ttl_.find(tenant);
  return it == tenant_ttl_.end() ? CacheClock::duration(cfg_.default_ttl) : it->second;
}

CacheStore::PartitionPtr CacheStore::find_partition(const ContextKey& ctx) const {
  std::shared_lock lock(map_mu_);
  auto it = partitions_.find(ctx);
  return it == partitions_.end() ? nullptr : it->second;
}

EntryPtr CacheStore::exact_lookup(const ContextKey& ctx, const PromptHash& prompt, CacheClock::time_point now) const {
  auto p = find_partition(ctx);
  if (!p) return nullptr;
  EntryPtr e;
  {
    std::shared_lock lock(p->mu);
    auto it = p->exact.find(prompt);
    if (it == p->exact.end()) return nullptr;
    e = p->entries.at(it->second);
  }
  return e->expired(now) ? nullptr : e;
}

NearestMatch CacheStore::nearest(const ContextKey& ctx, const Embedding& q, CacheClock::time_point now) const {
  auto p = find_partition(ctx);
  if (!p) return {};
  std::shared_lock lock(p->mu);
  if (!p->index) return {};
  for (const auto& hit : p->index->search(q, kSearchCandidates)) {
    const auto& e = p->entries.at(hit.id);
    if (!e->expired(now)) return {e, hit.score};
  }
  return {};
}

InsertOutcome CacheStore::insert(NewEntry ne, CacheClock::time_point now) {
  const auto bytes = accounted_bytes(ne);
  const auto quota = quota_for(ne.tenant);
  if (bytes > quota) {
    rejected_too_large_.inc();
    return InsertOutcome::too_large;
  }

  std::lock_guard w(writer_mu_);
  if (auto old = exact_lookup(ne.context, ne.prompt_hash, CacheClock::time_point::min())) {
    remove_locked(old, Reason::replaced);  // newest answer wins
  }
  // Only an explicit tenant quota evicts per tenant; otherwise the global budget below applies.
  if (tenant_quota_.contains(ne.tenant)) {
    while (tenant_bytes_[ne.tenant] + bytes > quota && evict_one_locked(&ne.tenant, now)) {
    }
  }
  while (total_bytes_.load(std::memory_order_relaxed) + bytes > cfg_.max_bytes && evict_one_locked(nullptr, now)) {
  }

  auto e = std::make_shared<CacheEntry>();
  e->id = next_id_++;
  e->context = ne.context;
  e->prompt_hash = ne.prompt_hash;
  e->tenant = std::move(ne.tenant);
  e->prompt = std::move(ne.prompt);
  e->embedding = ne.embedding;
  e->response = std::move(ne.response);
  e->total_tokens = ne.total_tokens;
  e->created = now;
  e->expires = now + ttl_for(e->tenant);
  e->bytes = bytes;
  EntryPtr entry = std::move(e);

  PartitionPtr p = find_partition(entry->context);
  if (!p) {
    std::unique_lock lock(map_mu_);
    auto& slot = partitions_[entry->context];
    if (!slot) {
      slot = std::make_shared<Partition>();
      partition_count_.fetch_add(1, std::memory_order_relaxed);
    }
    p = slot;
  }
  {
    std::unique_lock lock(p->mu);
    if (entry->embedding) {
      if (!p->index) p->index = index_factory_(kEmbeddingDim);
      p->index->add(entry->id, *entry->embedding);
    }
    p->entries.emplace(entry->id, entry);
    p->exact[entry->prompt_hash] = entry->id;
  }

  clock_pos_[entry->id] = clock_.insert(hand_, entry);  // just behind the hand: visited last
  tenant_bytes_[entry->tenant] += bytes;
  total_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  total_entries_.fetch_add(1, std::memory_order_relaxed);
  publish_gauges_locked();
  return InsertOutcome::stored;
}

void CacheStore::remove_locked(const EntryPtr& e, Reason why) {
  auto p = find_partition(e->context);
  bool now_empty = false;
  if (p) {
    std::unique_lock lock(p->mu);
    p->entries.erase(e->id);
    if (auto it = p->exact.find(e->prompt_hash); it != p->exact.end() && it->second == e->id) p->exact.erase(it);
    if (p->index) p->index->remove(e->id);
    now_empty = p->entries.empty();
  }
  if (now_empty) {
    // Only writers mutate partitions and we hold W, so the partition is still empty here.
    std::unique_lock lock(map_mu_);
    partitions_.erase(e->context);
    partition_count_.fetch_sub(1, std::memory_order_relaxed);
  }

  if (auto it = clock_pos_.find(e->id); it != clock_pos_.end()) {
    if (hand_ == it->second) ++hand_;
    clock_.erase(it->second);
    clock_pos_.erase(it);
  }
  tenant_bytes_[e->tenant] -= e->bytes;
  total_bytes_.fetch_sub(e->bytes, std::memory_order_relaxed);
  total_entries_.fetch_sub(1, std::memory_order_relaxed);

  switch (why) {
    case Reason::ttl: evicted_ttl_.inc(); break;
    case Reason::capacity: evicted_capacity_.inc(); break;
    case Reason::tenant_quota: evicted_quota_.inc(); break;
    case Reason::replaced: break;
  }
  publish_gauges_locked();
}

bool CacheStore::evict_one_locked(const std::string* only_tenant, CacheClock::time_point now) {
  // Two full turns guarantee a victim: the first may only clear referenced bits.
  const std::size_t max_steps = 2 * clock_.size() + 1;
  for (std::size_t step = 0; step < max_steps && !clock_.empty(); ++step) {
    if (hand_ == clock_.end()) hand_ = clock_.begin();
    EntryPtr e = *hand_;
    if (only_tenant != nullptr && e->tenant != *only_tenant) {
      ++hand_;
      continue;
    }
    if (e->expired(now)) {
      remove_locked(e, Reason::ttl);
      return true;
    }
    if (e->referenced.exchange(false, std::memory_order_relaxed)) {
      ++hand_;  // second chance
      continue;
    }
    remove_locked(e, only_tenant != nullptr ? Reason::tenant_quota : Reason::capacity);
    return true;
  }
  return false;
}

std::size_t CacheStore::expire(CacheClock::time_point now) {
  std::lock_guard w(writer_mu_);
  std::vector<EntryPtr> dead;
  for (const auto& e : clock_) {
    if (e->expired(now)) dead.push_back(e);
  }
  for (const auto& e : dead) remove_locked(e, Reason::ttl);
  return dead.size();
}

void CacheStore::publish_gauges_locked() {
  entries_gauge_.set(static_cast<std::int64_t>(total_entries_.load(std::memory_order_relaxed)));
  bytes_gauge_.set(static_cast<std::int64_t>(total_bytes_.load(std::memory_order_relaxed)));
  partitions_gauge_.set(static_cast<std::int64_t>(partition_count_.load(std::memory_order_relaxed)));
}

CacheStore::Stats CacheStore::stats() const {
  return {total_entries_.load(std::memory_order_relaxed), total_bytes_.load(std::memory_order_relaxed),
          partition_count_.load(std::memory_order_relaxed)};
}

std::size_t CacheStore::tenant_bytes(const std::string& tenant) const {
  std::lock_guard w(writer_mu_);
  auto it = tenant_bytes_.find(tenant);
  return it == tenant_bytes_.end() ? 0 : it->second;
}

}  // namespace semcache
