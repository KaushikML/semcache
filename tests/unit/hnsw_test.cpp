#include "index/hnsw_index.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <set>
#include <thread>

#include "index/brute_force_index.hpp"

namespace semcache {
namespace {

constexpr std::size_t kDim = 64;

void normalize(std::vector<float>& v) {
  float n = 0;
  for (float x : v) n += x * x;
  for (auto& x : v) x /= std::sqrt(n);
}

// Gaussian clusters on the unit sphere: closer to real sentence embeddings (topics) than uniform
// random vectors, for which every point is nearly equidistant.
std::vector<std::vector<float>> clustered(std::size_t n, std::size_t clusters, float spread, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> g;
  std::vector<std::vector<float>> centers(clusters, std::vector<float>(kDim));
  for (auto& c : centers) {
    for (auto& x : c) x = g(rng);
    normalize(c);
  }
  std::vector<std::vector<float>> out(n, std::vector<float>(kDim));
  for (std::size_t i = 0; i < n; ++i) {
    const auto& c = centers[i % clusters];
    for (std::size_t d = 0; d < kDim; ++d) out[i][d] = c[d] + spread * g(rng);
    normalize(out[i]);
  }
  return out;
}

double recall_at_k(const VectorIndex& approx, const VectorIndex& exact, const std::vector<std::vector<float>>& queries,
                   std::size_t k) {
  std::size_t found = 0;
  for (const auto& q : queries) {
    std::set<VectorId> truth;
    for (const auto& h : exact.search(q, k)) truth.insert(h.id);
    for (const auto& h : approx.search(q, k)) found += truth.count(h.id);
  }
  return static_cast<double>(found) / static_cast<double>(queries.size() * k);
}

TEST(Hnsw, RecallAt10AgainstBruteForce) {
  const auto data = clustered(5000, 50, 0.08f, 1);
  const auto queries = clustered(200, 50, 0.08f, 2);
  HnswIndex hnsw(kDim, {});
  BruteForceIndex exact(kDim);
  for (VectorId i = 0; i < data.size(); ++i) {
    hnsw.add(i, data[i]);
    exact.add(i, data[i]);
  }
  const double r = recall_at_k(hnsw, exact, queries, 10);
  std::printf("recall@10 (n=5000, M=16, efC=200, ef=64): %.4f\n", r);
  EXPECT_GE(r, 0.95);
}

TEST(Hnsw, LargerEfSearchDoesNotLowerRecall) {
  const auto data = clustered(3000, 30, 0.1f, 3);
  const auto queries = clustered(100, 30, 0.1f, 4);
  HnswIndex hnsw(kDim, {.m = 8, .ef_construction = 64, .ef_search = 10});
  BruteForceIndex exact(kDim);
  for (VectorId i = 0; i < data.size(); ++i) {
    hnsw.add(i, data[i]);
    exact.add(i, data[i]);
  }
  const double low = recall_at_k(hnsw, exact, queries, 10);
  hnsw.set_ef_search(200);
  const double high = recall_at_k(hnsw, exact, queries, 10);
  EXPECT_GE(high, low);
  EXPECT_GE(high, 0.97);
}

TEST(Hnsw, TombstonesAreNeverReturnedAndSignalRebuild) {
  const auto data = clustered(1000, 10, 0.1f, 5);
  HnswIndex hnsw(kDim, {.max_tombstone_ratio = 0.25});
  for (VectorId i = 0; i < data.size(); ++i) hnsw.add(i, data[i]);
  for (VectorId i = 0; i < 200; ++i) EXPECT_TRUE(hnsw.remove(i));
  EXPECT_EQ(hnsw.size(), 800u);
  EXPECT_EQ(hnsw.tombstones(), 200u);
  EXPECT_FALSE(hnsw.wants_rebuild()) << "20% < 25%";
  for (VectorId i = 0; i < 200; ++i) {
    for (const auto& h : hnsw.search(data[i], 10)) EXPECT_GE(h.id, 200u) << "deleted id " << h.id << " returned";
  }
  for (VectorId i = 200; i < 300; ++i) hnsw.remove(i);
  EXPECT_TRUE(hnsw.wants_rebuild()) << "30% > 25%";
  // Live vectors stay findable through the tombstones.
  for (VectorId i = 300; i < 400; ++i) EXPECT_EQ(hnsw.search(data[i], 1).at(0).id, i);
}

TEST(Hnsw, SameSeedSameGraph) {
  const auto data = clustered(500, 5, 0.1f, 6);
  HnswIndex a(kDim, {.seed = 7});
  HnswIndex b(kDim, {.seed = 7});
  for (VectorId i = 0; i < data.size(); ++i) {
    a.add(i, data[i]);
    b.add(i, data[i]);
  }
  EXPECT_EQ(a.max_level(), b.max_level());
  for (std::size_t q = 0; q < 20; ++q) {
    auto ra = a.search(data[q], 10);
    auto rb = b.search(data[q], 10);
    ASSERT_EQ(ra.size(), rb.size());
    for (std::size_t i = 0; i < ra.size(); ++i) EXPECT_EQ(ra[i].id, rb[i].id);
  }
}

TEST(Hnsw, MemoryIsVectorsPlusBoundedLinks) {
  const auto data = clustered(2000, 20, 0.1f, 8);
  HnswIndex hnsw(kDim, {.m = 16});
  for (VectorId i = 0; i < data.size(); ++i) hnsw.add(i, data[i]);
  const std::size_t vectors = data.size() * kDim * sizeof(float);
  EXPECT_GE(hnsw.memory_bytes(), vectors);
  EXPECT_LE(hnsw.memory_bytes(), vectors * 4) << "links stay within a small multiple at M=16, dim=64";
}

// Searches are const and run concurrently under a shared lock in CacheStore (TSan target).
TEST(Hnsw, ConcurrentSearchesAgreeWithSequential) {
  const auto data = clustered(2000, 20, 0.1f, 9);
  HnswIndex hnsw(kDim, {});
  for (VectorId i = 0; i < data.size(); ++i) hnsw.add(i, data[i]);
  std::vector<std::vector<SearchHit>> expected;
  for (std::size_t q = 0; q < 50; ++q) expected.push_back(hnsw.search(data[q], 5));
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int rep = 0; rep < 5; ++rep) {
        for (std::size_t q = 0; q < 50; ++q) {
          auto r = hnsw.search(data[q], 5);
          if (r.size() != expected[q].size()) {
            mismatches++;
            continue;
          }
          for (std::size_t i = 0; i < r.size(); ++i) mismatches += r[i].id != expected[q][i].id;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(mismatches.load(), 0);
}

}  // namespace
}  // namespace semcache
