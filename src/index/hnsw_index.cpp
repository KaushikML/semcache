#include "index/hnsw_index.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

namespace semcache {
namespace {

// Levels above this are astronomically unlikely (P = M^-L); the cap only guards the RNG tail.
constexpr int kMaxLevel = 16;

// Per-thread visited marks with a generation counter, so a search never clears an O(n) array.
struct Visited {
  std::vector<std::uint32_t> mark;
  std::uint32_t gen = 0;

  void reset(std::size_t n) {
    if (mark.size() < n) mark.resize(n, 0);
    if (++gen == 0) {  // wrapped: clear once every 2^32 searches
      std::fill(mark.begin(), mark.end(), 0);
      gen = 1;
    }
  }
  bool insert(std::size_t node) {
    if (mark[node] == gen) return false;
    mark[node] = gen;
    return true;
  }
};

struct ByScoreDesc {  // max-heap on score: best candidate first
  template <class C>
  bool operator()(const C& a, const C& b) const { return a.score < b.score; }
};
struct ByScoreAsc {  // min-heap on score: worst kept result first
  template <class C>
  bool operator()(const C& a, const C& b) const { return a.score > b.score; }
};

}  // namespace

HnswIndex::HnswIndex(std::size_t dim, Params params, SimdLevel simd)
    : dim_(dim), params_(params), simd_(simd), level_mult_(1.0 / std::log(static_cast<double>(params.m))),
      rng_(params.seed) {
  if (dim_ == 0) throw std::invalid_argument("HnswIndex: dim must be > 0");
  if (params_.m < 2) throw std::invalid_argument("HnswIndex: m must be >= 2");
}

float HnswIndex::dot(const float* a, const float* b) const {
  return simd_ == SimdLevel::avx2 ? dot_avx2(a, b, dim_) : dot_scalar(a, b, dim_);
}

int HnswIndex::random_level() {
  std::uniform_real_distribution<double> u(std::numeric_limits<double>::min(), 1.0);
  return std::min(kMaxLevel, static_cast<int>(-std::log(u(rng_)) * level_mult_));
}

std::vector<HnswIndex::Candidate> HnswIndex::search_layer(const float* q, const std::vector<Candidate>& entries,
                                                          std::size_t ef, int layer) const {
  thread_local Visited visited;
  visited.reset(ext_id_.size());
  std::priority_queue<Candidate, std::vector<Candidate>, ByScoreDesc> frontier;
  std::priority_queue<Candidate, std::vector<Candidate>, ByScoreAsc> best;
  for (const auto& e : entries) {
    if (!visited.insert(e.node)) continue;
    frontier.push(e);
    best.push(e);
    if (best.size() > ef) best.pop();
  }
  while (!frontier.empty()) {
    const auto c = frontier.top();
    if (best.size() >= ef && c.score < best.top().score) break;  // nothing left can improve the beam
    frontier.pop();
    const auto& nbrs = links_[c.node];
    if (static_cast<std::size_t>(layer) >= nbrs.size()) continue;
    for (Node n : nbrs[static_cast<std::size_t>(layer)]) {
      if (!visited.insert(n)) continue;
      const float s = dot(q, vec(n));
      if (best.size() < ef || s > best.top().score) {
        frontier.push({s, n});
        best.push({s, n});
        if (best.size() > ef) best.pop();
      }
    }
  }
  std::vector<Candidate> out;
  out.reserve(best.size());
  while (!best.empty()) {
    out.push_back(best.top());
    best.pop();
  }
  std::reverse(out.begin(), out.end());  // best first
  return out;
}

std::vector<HnswIndex::Node> HnswIndex::select_neighbors(std::vector<Candidate> candidates, std::size_t m) const {
  std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
  std::vector<Node> chosen;
  std::vector<Node> pruned;
  chosen.reserve(m);
  for (const auto& c : candidates) {
    if (chosen.size() >= m) break;
    // Tombstones stay eligible: they keep the graph connected (e.g. a deleted entry point) until
    // the owner rebuilds. They are filtered out of search results instead.
    bool diverse = true;
    for (Node r : chosen) {
      if (dot(vec(c.node), vec(r)) > c.score) {  // closer to a chosen neighbour than to the base
        diverse = false;
        break;
      }
    }
    (diverse ? chosen : pruned).push_back(c.node);
  }
  for (std::size_t i = 0; i < pruned.size() && chosen.size() < m; ++i) chosen.push_back(pruned[i]);
  return chosen;
}

void HnswIndex::link(Node from, Node to, int layer) {
  auto& list = links_[from][static_cast<std::size_t>(layer)];
  list.push_back(to);
  if (list.size() <= max_links(layer)) return;
  // Over capacity: keep the heuristic's best subset as seen from `from`.
  std::vector<Candidate> cands;
  cands.reserve(list.size());
  for (Node n : list) cands.push_back({dot(vec(from), vec(n)), n});
  list = select_neighbors(std::move(cands), max_links(layer));
}

void HnswIndex::add(VectorId id, std::span<const float> v) {
  if (v.size() != dim_) throw std::invalid_argument("HnswIndex::add: wrong dimension");
  remove(id);  // replacing a vector: tombstone the old node, insert a fresh one

  const auto node = static_cast<Node>(ext_id_.size());
  const int level = random_level();
  vectors_.insert(vectors_.end(), v.begin(), v.end());
  ext_id_.push_back(id);
  deleted_.push_back(0);
  links_.emplace_back(static_cast<std::size_t>(level) + 1);
  node_of_.emplace(id, node);
  if (max_level_ < 0) {
    entry_ = node;
    max_level_ = level;
    return;
  }

  const float* q = vec(node);
  std::vector<Candidate> eps = {{dot(q, vec(entry_)), entry_}};
  for (int l = max_level_; l > level; --l) eps = search_layer(q, eps, 1, l);  // greedy descent
  for (int l = std::min(level, max_level_); l >= 0; --l) {
    eps = search_layer(q, eps, params_.ef_construction, l);
    auto neighbours = select_neighbors(eps, params_.m);
    links_[node][static_cast<std::size_t>(l)] = neighbours;
    for (Node n : neighbours) link(n, node, l);
  }
  if (level > max_level_) {
    entry_ = node;
    max_level_ = level;
  }
}

bool HnswIndex::remove(VectorId id) {
  auto it = node_of_.find(id);
  if (it == node_of_.end()) return false;
  deleted_[it->second] = 1;  // stays in the graph for routing
  node_of_.erase(it);
  return true;
}

std::vector<SearchHit> HnswIndex::search(std::span<const float> q, std::size_t k) const {
  if (q.size() != dim_) throw std::invalid_argument("HnswIndex::search: wrong dimension");
  std::vector<SearchHit> out;
  if (k == 0 || node_of_.empty()) return out;

  std::vector<Candidate> eps = {{dot(q.data(), vec(entry_)), entry_}};
  for (int l = max_level_; l > 0; --l) eps = search_layer(q.data(), eps, 1, l);
  // Widen the beam by the tombstones it may have to skip.
  const auto ef = std::max(params_.ef_search, k) + std::min(tombstones(), k);
  for (const auto& c : search_layer(q.data(), eps, ef, 0)) {
    if (deleted_[c.node]) continue;
    out.push_back({ext_id_[c.node], c.score});
    if (out.size() == k) break;
  }
  return out;
}

bool HnswIndex::wants_rebuild() const {
  return !ext_id_.empty() &&
         static_cast<double>(tombstones()) > params_.max_tombstone_ratio * static_cast<double>(ext_id_.size());
}

std::size_t HnswIndex::memory_bytes() const {
  constexpr std::size_t kMapNodeBytes = 32;  // unordered_map node estimate
  std::size_t links = 0;
  for (const auto& layers : links_) {
    links += layers.capacity() * sizeof(std::vector<Node>);
    for (const auto& l : layers) links += l.capacity() * sizeof(Node);
  }
  return vectors_.capacity() * sizeof(float) + ext_id_.capacity() * sizeof(VectorId) + deleted_.capacity() +
         links_.capacity() * sizeof(links_[0]) + links + node_of_.size() * kMapNodeBytes;
}

}  // namespace semcache
