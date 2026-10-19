// Vector search cost (Phase 3): dot kernels and brute-force scan vs partition size, to check the
// design §5.3 estimate (~1.5 ms per 10K vectors) and pick the HNSW switch point later (Phase 5).

#include <benchmark/benchmark.h>

#include <cmath>
#include <random>

#include "embed/embedder.hpp"
#include "index/brute_force_index.hpp"

namespace semcache {
namespace {

std::vector<float> unit(std::mt19937& rng) {
  std::normal_distribution<float> d;
  std::vector<float> v(kEmbeddingDim);
  float n = 0;
  for (auto& x : v) {
    x = d(rng);
    n += x * x;
  }
  for (auto& x : v) x /= std::sqrt(n);
  return v;
}

void BM_Dot(benchmark::State& state) {
  const auto level = static_cast<SimdLevel>(state.range(0));
  if (level == SimdLevel::avx2 && detected_simd() != SimdLevel::avx2) return state.SkipWithError("no AVX2");
  std::mt19937 rng(1);
  auto a = unit(rng);
  auto b = unit(rng);
  for (auto _ : state) {
    float s = level == SimdLevel::avx2 ? dot_avx2(a.data(), b.data(), kEmbeddingDim)
                                       : dot_scalar(a.data(), b.data(), kEmbeddingDim);
    benchmark::DoNotOptimize(s);
  }
  state.SetLabel(std::string(to_string(level)));
}
BENCHMARK(BM_Dot)->Arg(0)->Arg(1);

void BM_BruteForceSearch(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  const auto level = static_cast<SimdLevel>(state.range(1));
  if (level == SimdLevel::avx2 && detected_simd() != SimdLevel::avx2) return state.SkipWithError("no AVX2");
  std::mt19937 rng(2);
  BruteForceIndex idx(kEmbeddingDim, level);
  for (std::size_t i = 0; i < n; ++i) idx.add(i, unit(rng));
  std::vector<std::vector<float>> queries;
  for (int i = 0; i < 16; ++i) queries.push_back(unit(rng));
  std::size_t q = 0;
  for (auto _ : state) {
    auto hits = idx.search(queries[q++ % queries.size()], 8);
    benchmark::DoNotOptimize(hits);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * n * kEmbeddingDim * sizeof(float)));
  state.SetLabel(std::string(to_string(level)));
}
BENCHMARK(BM_BruteForceSearch)
    ->ArgsProduct({{1'000, 10'000, 20'000, 100'000}, {0, 1}})
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace semcache

BENCHMARK_MAIN();
