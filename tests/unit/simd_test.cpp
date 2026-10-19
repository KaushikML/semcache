#include "index/simd.hpp"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace semcache {
namespace {

std::vector<float> random_vec(std::size_t n, std::mt19937& rng) {
  std::normal_distribution<float> d;
  std::vector<float> v(n);
  for (auto& x : v) x = d(rng);
  return v;
}

TEST(Simd, Avx2MatchesScalarForAllTailLengths) {
  if (detected_simd() != SimdLevel::avx2) GTEST_SKIP() << "CPU lacks AVX2+FMA";
  std::mt19937 rng(1);
  for (std::size_t n : {0u, 1u, 7u, 8u, 9u, 31u, 32u, 33u, 100u, 384u, 1000u}) {
    auto a = random_vec(n, rng);
    auto b = random_vec(n, rng);
    const float s = dot_scalar(a.data(), b.data(), n);
    const float v = dot_avx2(a.data(), b.data(), n);
    EXPECT_NEAR(v, s, 1e-4f * (1.0f + std::abs(s))) << "n=" << n;
  }
}

TEST(Simd, UnalignedPointersAreFine) {
  if (detected_simd() != SimdLevel::avx2) GTEST_SKIP() << "CPU lacks AVX2+FMA";
  std::mt19937 rng(2);
  auto a = random_vec(400, rng);
  auto b = random_vec(400, rng);
  EXPECT_NEAR(dot_avx2(a.data() + 1, b.data() + 3, 384), dot_scalar(a.data() + 1, b.data() + 3, 384), 1e-3f);
}

TEST(Simd, DotRowsScoresEveryRow) {
  std::mt19937 rng(3);
  const std::size_t dim = 384;
  const std::size_t rows = 5;
  auto q = random_vec(dim, rng);
  auto data = random_vec(dim * rows, rng);
  std::vector<float> out(rows);
  for (auto level : {SimdLevel::scalar, detected_simd()}) {
    dot_rows(level, q.data(), data.data(), rows, dim, out.data());
    for (std::size_t r = 0; r < rows; ++r) {
      EXPECT_NEAR(out[r], dot_scalar(q.data(), data.data() + r * dim, dim), 1e-3f) << to_string(level);
    }
  }
}

}  // namespace
}  // namespace semcache
