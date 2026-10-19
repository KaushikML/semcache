#pragma once

// Nearest-neighbour index over L2-normalized vectors, scored by dot product (= cosine).
// Implementations are not thread-safe; CacheStore guards each index with its partition lock.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace semcache {

using VectorId = std::uint64_t;

struct SearchHit {
  VectorId id;
  float score;
};

class VectorIndex {
 public:
  virtual ~VectorIndex() = default;

  // Adds or replaces the vector for `id`. `v.size()` must equal dim().
  virtual void add(VectorId id, std::span<const float> v) = 0;
  // Returns false if `id` was not present.
  virtual bool remove(VectorId id) = 0;
  // Up to k hits, best first.
  [[nodiscard]] virtual std::vector<SearchHit> search(std::span<const float> q, std::size_t k) const = 0;

  [[nodiscard]] virtual std::size_t size() const = 0;
  [[nodiscard]] virtual std::size_t dim() const = 0;
  [[nodiscard]] virtual std::size_t memory_bytes() const = 0;
};

}  // namespace semcache
