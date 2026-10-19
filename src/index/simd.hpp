#pragma once

// Dot-product kernels for L2-normalized vectors (cosine = dot). AVX2+FMA when the CPU has it,
// scalar otherwise, chosen once at startup (ADR-006).

#include <cstddef>
#include <string_view>

namespace semcache {

enum class SimdLevel { scalar, avx2 };

[[nodiscard]] std::string_view to_string(SimdLevel l);
// Best level this CPU supports.
[[nodiscard]] SimdLevel detected_simd();

[[nodiscard]] float dot_scalar(const float* a, const float* b, std::size_t n);
// Requires AVX2 + FMA; callers check detected_simd() first.
[[nodiscard]] float dot_avx2(const float* a, const float* b, std::size_t n);

// Scores `rows` vectors stored contiguously (row-major, `dim` floats each) against `q`.
void dot_rows(SimdLevel level, const float* q, const float* rows, std::size_t n_rows, std::size_t dim, float* out);

}  // namespace semcache
