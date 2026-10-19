#include "cache/cache_store.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <thread>

namespace semcache {
namespace {

using namespace std::chrono_literals;

const CacheClock::time_point T0 = CacheClock::time_point(std::chrono::hours(500'000));

Embedding unit_vec(std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> d;
  Embedding v{};
  float n = 0;
  for (auto& x : v) {
    x = d(rng);
    n += x * x;
  }
  for (auto& x : v) x /= std::sqrt(n);
  return v;
}

// A vector at cosine ~`cos` from `base`.
Embedding near(const Embedding& base, float cos, std::uint32_t seed) {
  auto noise = unit_vec(seed);
  float d = 0;
  for (std::size_t i = 0; i < kEmbeddingDim; ++i) d += noise[i] * base[i];
  Embedding v{};
  float n = 0;
  for (std::size_t i = 0; i < kEmbeddingDim; ++i) {
    const float ortho = noise[i] - d * base[i];
    v[i] = ortho;
    n += ortho * ortho;
  }
  const float s = std::sqrt(1 - cos * cos);
  for (std::size_t i = 0; i < kEmbeddingDim; ++i) v[i] = cos * base[i] + s * v[i] / std::sqrt(n);
  return v;
}

ContextKey ctx(std::uint8_t b) {
  ContextKey k{};
  k.fill(b);
  return k;
}
PromptHash ph(std::uint8_t b) {
  PromptHash h{};
  h.fill(b);
  h[0] = 0xEE;
  return h;
}

NewEntry entry(std::uint8_t c, std::uint8_t p, std::optional<Embedding> v = std::nullopt, std::string tenant = "acme",
               std::size_t response_bytes = 100) {
  NewEntry e;
  e.context = ctx(c);
  e.prompt_hash = ph(p);
  e.tenant = std::move(tenant);
  e.prompt = "prompt";
  e.embedding = v;
  e.response = std::string(response_bytes, 'r');
  e.total_tokens = 42;
  return e;
}

class CacheStoreTest : public ::testing::Test {
 protected:
  CacheStore make(CacheConfig cfg = {}, std::vector<TenantConfig> tenants = {}) {
    return CacheStore(cfg, tenants, registry_);
  }
  MetricsRegistry registry_;
};

TEST_F(CacheStoreTest, ExactLookupHitsOnlySameContextAndPrompt) {
  CacheStore s(CacheConfig{}, {}, registry_);
  ASSERT_EQ(s.insert(entry(1, 1), T0), InsertOutcome::stored);
  auto e = s.exact_lookup(ctx(1), ph(1), T0);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->total_tokens, 42);
  EXPECT_FALSE(s.exact_lookup(ctx(1), ph(2), T0));
  EXPECT_FALSE(s.exact_lookup(ctx(2), ph(1), T0)) << "another context (e.g. another tenant) never sees it";
}

TEST_F(CacheStoreTest, NearestReturnsBestLiveMatchInPartitionOnly) {
  CacheStore s(CacheConfig{}, {}, registry_);
  const auto q = unit_vec(1);
  s.insert(entry(1, 1, near(q, 0.95f, 2)), T0);
  s.insert(entry(1, 2, near(q, 0.80f, 3)), T0);
  s.insert(entry(2, 3, q), T0);  // identical vector, other context
  auto m = s.nearest(ctx(1), q, T0);
  ASSERT_TRUE(m.entry);
  EXPECT_EQ(m.entry->prompt_hash, ph(1));
  EXPECT_NEAR(m.score, 0.95f, 1e-3f);
  EXPECT_FALSE(s.nearest(ctx(3), q, T0).entry);
}

TEST_F(CacheStoreTest, ExactOnlyEntriesAreNotInTheVectorIndex) {
  CacheStore s(CacheConfig{}, {}, registry_);
  s.insert(entry(1, 1), T0);  // no embedding
  EXPECT_TRUE(s.exact_lookup(ctx(1), ph(1), T0));
  EXPECT_FALSE(s.nearest(ctx(1), unit_vec(1), T0).entry);
}

TEST_F(CacheStoreTest, ExpiredEntriesAreInvisibleThenSwept) {
  CacheConfig cfg;
  cfg.default_ttl = 60s;
  CacheStore s(cfg, {}, registry_);
  const auto v = unit_vec(1);
  s.insert(entry(1, 1, v), T0);
  EXPECT_TRUE(s.exact_lookup(ctx(1), ph(1), T0 + 59s));
  EXPECT_FALSE(s.exact_lookup(ctx(1), ph(1), T0 + 60s));
  EXPECT_FALSE(s.nearest(ctx(1), v, T0 + 61s).entry);
  EXPECT_EQ(s.stats().entries, 1u) << "lazy: still stored until swept";
  EXPECT_EQ(s.expire(T0 + 61s), 1u);
  EXPECT_EQ(s.stats().entries, 0u);
  EXPECT_EQ(s.stats().partitions, 0u) << "empty partitions are dropped";
  EXPECT_EQ(s.stats().bytes, 0u);
}

TEST_F(CacheStoreTest, PerTenantTtlOverride) {
  CacheConfig cfg;
  cfg.default_ttl = 3600s;
  TenantConfig short_lived;
  short_lived.id = "fast";
  short_lived.ttl = 10s;
  CacheStore s(cfg, {short_lived}, registry_);
  s.insert(entry(1, 1, std::nullopt, "fast"), T0);
  s.insert(entry(2, 1, std::nullopt, "acme"), T0);
  EXPECT_FALSE(s.exact_lookup(ctx(1), ph(1), T0 + 11s));
  EXPECT_TRUE(s.exact_lookup(ctx(2), ph(1), T0 + 11s));
}

TEST_F(CacheStoreTest, SamePromptReplacesOldEntry) {
  CacheStore s(CacheConfig{}, {}, registry_);
  auto a = entry(1, 1);
  a.response = "old";
  auto b = entry(1, 1);
  b.response = "new";
  s.insert(a, T0);
  s.insert(b, T0);
  EXPECT_EQ(s.stats().entries, 1u);
  EXPECT_EQ(s.exact_lookup(ctx(1), ph(1), T0)->response, "new");
}

TEST_F(CacheStoreTest, ByteBudgetEvictsUnreferencedFirst) {
  const auto one = 350 + 4 + 6 + 1000;  // overhead + tenant + prompt + response
  CacheConfig cfg;
  cfg.max_bytes = 3 * one;
  CacheStore s(cfg, {}, registry_);
  for (std::uint8_t i = 1; i <= 3; ++i) s.insert(entry(1, i, std::nullopt, "acme", 1000), T0);
  ASSERT_EQ(s.stats().bytes, 3u * one);
  CacheStore::touch(*s.exact_lookup(ctx(1), ph(1), T0));  // 1 is hot
  s.insert(entry(1, 4, std::nullopt, "acme", 1000), T0);
  EXPECT_EQ(s.stats().entries, 3u);
  EXPECT_LE(s.stats().bytes, cfg.max_bytes);
  EXPECT_TRUE(s.exact_lookup(ctx(1), ph(1), T0)) << "referenced entry gets a second chance";
  EXPECT_FALSE(s.exact_lookup(ctx(1), ph(2), T0)) << "oldest unreferenced entry is the victim";
  EXPECT_TRUE(s.exact_lookup(ctx(1), ph(4), T0));
  EXPECT_NE(registry_.render_prometheus().find(R"(semcache_evictions_total{reason="capacity"} 1)"), std::string::npos)
      << registry_.render_prometheus();
}

TEST_F(CacheStoreTest, TenantQuotaOnlyEvictsThatTenant) {
  const auto one = 350 + 4 + 6 + 1000;
  CacheConfig cfg;
  cfg.max_bytes = 100 * one;
  TenantConfig small;
  small.id = "smol";
  small.max_bytes = 2 * one;
  CacheStore s(cfg, {small}, registry_);
  s.insert(entry(1, 1, std::nullopt, "acme", 1000), T0);
  for (std::uint8_t i = 1; i <= 5; ++i) s.insert(entry(2, i, std::nullopt, "smol", 1000), T0);
  EXPECT_LE(s.tenant_bytes("smol"), 2u * one);
  EXPECT_TRUE(s.exact_lookup(ctx(1), ph(1), T0)) << "other tenants are untouched";
  EXPECT_TRUE(s.exact_lookup(ctx(2), ph(5), T0));
  EXPECT_NE(registry_.render_prometheus().find(R"(semcache_evictions_total{reason="tenant_quota"} 3)"), std::string::npos);
}

TEST_F(CacheStoreTest, EntryLargerThanBudgetIsNotStored) {
  CacheConfig cfg;
  cfg.max_bytes = 1000;
  CacheStore s(cfg, {}, registry_);
  EXPECT_EQ(s.insert(entry(1, 1, std::nullopt, "acme", 2000), T0), InsertOutcome::too_large);
  EXPECT_EQ(s.stats().entries, 0u);
}

TEST_F(CacheStoreTest, EvictedEntryStaysValidForReadersHoldingIt) {
  const auto one = 350 + 4 + 6 + 1000;
  CacheConfig cfg;
  cfg.max_bytes = one;
  CacheStore s(cfg, {}, registry_);
  s.insert(entry(1, 1, std::nullopt, "acme", 1000), T0);
  EntryPtr held = s.exact_lookup(ctx(1), ph(1), T0);
  s.insert(entry(1, 2, std::nullopt, "acme", 1000), T0);  // evicts 1
  EXPECT_FALSE(s.exact_lookup(ctx(1), ph(1), T0));
  EXPECT_EQ(held->response.size(), 1000u);
}

// Many readers + one writer, for TSan. Also checks the accounting invariant at the end.
TEST_F(CacheStoreTest, ConcurrentReadersAndWriter) {
  CacheConfig cfg;
  cfg.max_bytes = 50 * (350 + 4 + 6 + 100 + sizeof(Embedding));
  CacheStore s(cfg, {}, registry_);
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t] {
      std::uint32_t i = 0;
      while (!stop.load()) {
        const auto c = static_cast<std::uint8_t>(i % 4);
        if (auto e = s.exact_lookup(ctx(c), ph(static_cast<std::uint8_t>(i % 200)), T0)) CacheStore::touch(*e);
        auto m = s.nearest(ctx(c), unit_vec(static_cast<std::uint32_t>(t * 1000 + i % 50)), T0);
        if (m.entry) EXPECT_FALSE(m.entry->response.empty());
        ++i;
      }
    });
  }
  for (std::uint32_t i = 0; i < 2000; ++i) {
    s.insert(entry(static_cast<std::uint8_t>(i % 4), static_cast<std::uint8_t>(i % 200), unit_vec(i)), T0);
    if (i % 500 == 0) s.expire(T0);
  }
  stop = true;
  for (auto& t : readers) t.join();
  EXPECT_LE(s.stats().bytes, cfg.max_bytes);
  EXPECT_EQ(s.stats().entries, 50u);
}

}  // namespace
}  // namespace semcache
