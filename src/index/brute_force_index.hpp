#pragma once

// Exact search: one contiguous row-major float buffer scanned with the SIMD dot kernel (ADR-006).
// Removal swaps the last row into the hole, so the buffer stays dense.

#include <unordered_map>

#include "index/simd.hpp"
#include "index/vector_index.hpp"

namespace semcache {

class BruteForceIndex final : public VectorIndex {
 public:
  explicit BruteForceIndex(std::size_t dim, SimdLevel simd = detected_simd());

  void add(VectorId id, std::span<const float> v) override;
  bool remove(VectorId id) override;
  [[nodiscard]] std::vector<SearchHit> search(std::span<const float> q, std::size_t k) const override;

  [[nodiscard]] std::size_t size() const override { return ids_.size(); }
  [[nodiscard]] std::size_t dim() const override { return dim_; }
  [[nodiscard]] std::size_t memory_bytes() const override;
  [[nodiscard]] std::string_view kind() const override { return "brute"; }

 private:
  std::size_t dim_;
  SimdLevel simd_;
  std::vector<float> rows_;  // size() * dim_
  std::vector<VectorId> ids_;
  std::unordered_map<VectorId, std::size_t> row_of_;
};

}  // namespace semcache
