#include "index/simd.hpp"

#include <immintrin.h>

namespace semcache {

std::string_view to_string(SimdLevel l) { return l == SimdLevel::avx2 ? "avx2" : "scalar"; }

SimdLevel detected_simd() {
  static const SimdLevel level = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") ? SimdLevel::avx2 : SimdLevel::scalar;
  }();
  return level;
}

float dot_scalar(const float* a, const float* b, std::size_t n) {
  float s = 0.0f;
  for (std::size_t i = 0; i < n; ++i) s += a[i] * b[i];
  return s;
}

__attribute__((target("avx2,fma"))) float dot_avx2(const float* a, const float* b, std::size_t n) {
  // Four independent accumulators hide FMA latency; 384 = 12 iterations of 32 floats.
  __m256 acc0 = _mm256_setzero_ps();
  __m256 acc1 = _mm256_setzero_ps();
  __m256 acc2 = _mm256_setzero_ps();
  __m256 acc3 = _mm256_setzero_ps();
  std::size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
    acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), acc1);
    acc2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), acc2);
    acc3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), acc3);
  }
  for (; i + 8 <= n; i += 8) acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
  __m256 acc = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
  __m128 lo = _mm256_castps256_ps128(acc);
  __m128 hi = _mm256_extractf128_ps(acc, 1);
  __m128 s = _mm_add_ps(lo, hi);
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_movehdup_ps(s));
  float total = _mm_cvtss_f32(s);
  for (; i < n; ++i) total += a[i] * b[i];
  return total;
}

void dot_rows(SimdLevel level, const float* q, const float* rows, std::size_t n_rows, std::size_t dim, float* out) {
  if (level == SimdLevel::avx2) {
    for (std::size_t r = 0; r < n_rows; ++r) out[r] = dot_avx2(q, rows + r * dim, dim);
  } else {
    for (std::size_t r = 0; r < n_rows; ++r) out[r] = dot_scalar(q, rows + r * dim, dim);
  }
}

}  // namespace semcache
