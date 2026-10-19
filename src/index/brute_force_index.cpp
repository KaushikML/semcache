#include "index/brute_force_index.hpp"

#include <algorithm>
#include <stdexcept>

namespace semcache {

BruteForceIndex::BruteForceIndex(std::size_t dim, SimdLevel simd) : dim_(dim), simd_(simd) {
  if (dim_ == 0) throw std::invalid_argument("BruteForceIndex: dim must be > 0");
}

void BruteForceIndex::add(VectorId id, std::span<const float> v) {
  if (v.size() != dim_) throw std::invalid_argument("BruteForceIndex::add: wrong dimension");
  if (auto it = row_of_.find(id); it != row_of_.end()) {
    std::copy(v.begin(), v.end(), rows_.begin() + static_cast<std::ptrdiff_t>(it->second * dim_));
    return;
  }
  row_of_.emplace(id, ids_.size());
  ids_.push_back(id);
  rows_.insert(rows_.end(), v.begin(), v.end());
}

bool BruteForceIndex::remove(VectorId id) {
  auto it = row_of_.find(id);
  if (it == row_of_.end()) return false;
  const std::size_t row = it->second;
  const std::size_t last = ids_.size() - 1;
  if (row != last) {
    std::copy_n(rows_.begin() + static_cast<std::ptrdiff_t>(last * dim_), dim_,
                rows_.begin() + static_cast<std::ptrdiff_t>(row * dim_));
    ids_[row] = ids_[last];
    row_of_[ids_[row]] = row;
  }
  ids_.pop_back();
  rows_.resize(ids_.size() * dim_);
  row_of_.erase(it);
  return true;
}

std::vector<SearchHit> BruteForceIndex::search(std::span<const float> q, std::size_t k) const {
  if (q.size() != dim_) throw std::invalid_argument("BruteForceIndex::search: wrong dimension");
  std::vector<SearchHit> best;
  if (k == 0 || ids_.empty()) return best;

  // Per-thread scratch: concurrent searches run under a shared lock.
  thread_local std::vector<float> scores;
  scores.resize(ids_.size());
  dot_rows(simd_, q.data(), rows_.data(), ids_.size(), dim_, scores.data());

  // Small-k insertion into a sorted array beats a heap for the k <= 10 the cache uses.
  best.reserve(k + 1);
  for (std::size_t r = 0; r < ids_.size(); ++r) {
    if (best.size() == k && scores[r] <= best.back().score) continue;
    auto pos = std::upper_bound(best.begin(), best.end(), scores[r],
                                [](float s, const SearchHit& h) { return s > h.score; });
    best.insert(pos, SearchHit{ids_[r], scores[r]});
    if (best.size() > k) best.pop_back();
  }
  return best;
}

std::size_t BruteForceIndex::memory_bytes() const {
  // Capacity, not size: that is what the allocator holds. Hash map node cost is an estimate.
  constexpr std::size_t kMapNodeBytes = 32;
  return rows_.capacity() * sizeof(float) + ids_.capacity() * sizeof(VectorId) + row_of_.size() * kMapNodeBytes;
}

}  // namespace semcache
