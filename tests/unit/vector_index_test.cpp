// Contract tests for every VectorIndex implementation (HNSW joins this suite in Phase 5).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <random>

#include "index/brute_force_index.hpp"
#include "index/hnsw_index.hpp"

namespace semcache {
namespace {

using Factory = std::function<std::unique_ptr<VectorIndex>(std::size_t dim)>;

std::vector<float> unit(std::size_t dim, std::mt19937& rng) {
  std::normal_distribution<float> d;
  std::vector<float> v(dim);
  float n = 0;
  for (auto& x : v) {
    x = d(rng);
    n += x * x;
  }
  for (auto& x : v) x /= std::sqrt(n);
  return v;
}

class VectorIndexContract : public ::testing::TestWithParam<std::pair<std::string, Factory>> {
 protected:
  std::unique_ptr<VectorIndex> make(std::size_t dim = 16) { return GetParam().second(dim); }
};

TEST_P(VectorIndexContract, EmptySearchReturnsNothing) {
  auto idx = make();
  std::mt19937 rng(1);
  EXPECT_TRUE(idx->search(unit(16, rng), 5).empty());
  EXPECT_EQ(idx->size(), 0u);
}

TEST_P(VectorIndexContract, FindsExactVectorWithScoreOne) {
  auto idx = make();
  std::mt19937 rng(2);
  std::vector<std::vector<float>> vs;
  for (VectorId i = 0; i < 50; ++i) {
    vs.push_back(unit(16, rng));
    idx->add(i, vs.back());
  }
  for (VectorId i = 0; i < 50; ++i) {
    auto hits = idx->search(vs[i], 1);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].id, i);
    EXPECT_NEAR(hits[0].score, 1.0f, 1e-5f);
  }
}

TEST_P(VectorIndexContract, ResultsAreSortedAndCappedAtK) {
  auto idx = make();
  std::mt19937 rng(3);
  for (VectorId i = 0; i < 100; ++i) idx->add(i, unit(16, rng));
  auto hits = idx->search(unit(16, rng), 10);
  ASSERT_EQ(hits.size(), 10u);
  EXPECT_TRUE(std::is_sorted(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.score > b.score; }));
  EXPECT_EQ(idx->search(unit(16, rng), 1000).size(), 100u);
}

TEST_P(VectorIndexContract, RemoveIsExactAndKeepsOthersFindable) {
  auto idx = make();
  std::mt19937 rng(4);
  std::vector<std::vector<float>> vs;
  for (VectorId i = 0; i < 20; ++i) {
    vs.push_back(unit(16, rng));
    idx->add(i, vs.back());
  }
  EXPECT_TRUE(idx->remove(0));   // first row: the last row moves into its slot
  EXPECT_TRUE(idx->remove(19));  // last row
  EXPECT_FALSE(idx->remove(0));
  EXPECT_FALSE(idx->remove(1234));
  EXPECT_EQ(idx->size(), 18u);
  for (VectorId i = 1; i < 19; ++i) EXPECT_EQ(idx->search(vs[i], 1).at(0).id, i) << i;
  for (const auto& h : idx->search(vs[0], 100)) EXPECT_NE(h.id, 0u);
}

TEST_P(VectorIndexContract, AddingAnExistingIdReplacesItsVector) {
  auto idx = make();
  std::mt19937 rng(5);
  auto a = unit(16, rng);
  auto b = unit(16, rng);
  idx->add(7, a);
  idx->add(7, b);
  EXPECT_EQ(idx->size(), 1u);
  EXPECT_NEAR(idx->search(b, 1).at(0).score, 1.0f, 1e-5f);
}

TEST_P(VectorIndexContract, WrongDimensionIsRejected) {
  auto idx = make(16);
  std::vector<float> v(15, 0.0f);
  EXPECT_THROW(idx->add(1, v), std::invalid_argument);
  EXPECT_THROW((void)idx->search(v, 1), std::invalid_argument);
}

TEST_P(VectorIndexContract, MemoryGrowsWithEntries) {
  auto idx = make(384);
  std::mt19937 rng(6);
  const auto empty = idx->memory_bytes();
  for (VectorId i = 0; i < 100; ++i) idx->add(i, unit(384, rng));
  EXPECT_GE(idx->memory_bytes() - empty, 100u * 384u * sizeof(float));
}

INSTANTIATE_TEST_SUITE_P(
    Impls, VectorIndexContract,
    ::testing::Values(
        std::make_pair(std::string("BruteForceScalar"),
                       Factory([](std::size_t d) { return std::make_unique<BruteForceIndex>(d, SimdLevel::scalar); })),
        std::make_pair(std::string("BruteForceBest"),
                       Factory([](std::size_t d) { return std::make_unique<BruteForceIndex>(d); })),
        std::make_pair(std::string("Hnsw"), Factory([](std::size_t d) {
                         return std::make_unique<HnswIndex>(d, HnswIndex::Params{});
                       })),
        std::make_pair(std::string("HnswSmallM"), Factory([](std::size_t d) {
                         return std::make_unique<HnswIndex>(d, HnswIndex::Params{.m = 4, .ef_construction = 16,
                                                                                  .ef_search = 8});
                       }))),
    [](const auto& info) { return info.param.first; });

}  // namespace
}  // namespace semcache
