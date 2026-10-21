#pragma once

// Hierarchical Navigable Small World graph (Malkov & Yashunin, 2016/2018), scored by dot product
// on L2-normalized vectors (= cosine). Approximate: recall@k < 1, traded for search cost that grows
// roughly logarithmically instead of linearly with the partition size (ADR-006).
//
// - Each node gets a random level L = floor(-ln(U) / ln(M)); layer 0 holds every node.
// - Search: greedy descent with beam 1 through the upper layers, then a beam search of width
//   ef_search on layer 0.
// - Insert: the same descent, then on each layer the neighbour-selection heuristic (paper's
//   Algorithm 4) picks diverse neighbours; links are bidirectional and pruned to M (2M on layer 0).
// - Remove: tombstone. Deleted nodes still route searches but are never returned. Once the
//   tombstone share passes max_tombstone_ratio, wants_rebuild() asks the owner to rebuild.
//
// Thread-safety: add/remove need exclusive access; search() is const and may run concurrently
// (its visited set is thread_local).

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "index/simd.hpp"
#include "index/vector_index.hpp"

namespace semcache {

class HnswIndex final : public VectorIndex {
 public:
  struct Params {
    std::size_t m = 16;
    std::size_t ef_construction = 200;
    std::size_t ef_search = 64;
    double max_tombstone_ratio = 0.25;
    std::uint64_t seed = 42;
  };

  HnswIndex(std::size_t dim, Params params, SimdLevel simd = detected_simd());

  void add(VectorId id, std::span<const float> v) override;
  bool remove(VectorId id) override;
  [[nodiscard]] std::vector<SearchHit> search(std::span<const float> q, std::size_t k) const override;

  [[nodiscard]] std::size_t size() const override { return node_of_.size(); }
  [[nodiscard]] std::size_t dim() const override { return dim_; }
  [[nodiscard]] std::size_t memory_bytes() const override;
  [[nodiscard]] std::string_view kind() const override { return "hnsw"; }
  [[nodiscard]] bool wants_rebuild() const override;

  // Introspection for tests and benchmarks.
  [[nodiscard]] std::size_t tombstones() const { return ext_id_.size() - node_of_.size(); }
  [[nodiscard]] int max_level() const { return max_level_; }
  void set_ef_search(std::size_t ef) { params_.ef_search = ef; }

 private:
  using Node = std::uint32_t;
  struct Candidate {
    float score;
    Node node;
  };

  [[nodiscard]] const float* vec(Node n) const { return vectors_.data() + static_cast<std::size_t>(n) * dim_; }
  [[nodiscard]] float dot(const float* a, const float* b) const;
  [[nodiscard]] std::size_t max_links(int layer) const { return layer == 0 ? 2 * params_.m : params_.m; }
  [[nodiscard]] int random_level();
  // Beam search on one layer from `entries`; best first, at most `ef` results.
  [[nodiscard]] std::vector<Candidate> search_layer(const float* q, const std::vector<Candidate>& entries,
                                                    std::size_t ef, int layer) const;
  // Paper's Algorithm 4 (keepPrunedConnections = true): prefer candidates that are closer to the
  // base than to any neighbour already chosen, so links spread in different directions.
  [[nodiscard]] std::vector<Node> select_neighbors(std::vector<Candidate> candidates, std::size_t m) const;
  void link(Node from, Node to, int layer);

  std::size_t dim_;
  Params params_;
  SimdLevel simd_;
  double level_mult_;
  std::mt19937_64 rng_;

  std::vector<float> vectors_;                      // node-major, dim_ floats each
  std::vector<VectorId> ext_id_;                    // node -> external id
  std::vector<std::uint8_t> deleted_;               // tombstones
  std::vector<std::vector<std::vector<Node>>> links_;  // links_[node][layer]
  std::unordered_map<VectorId, Node> node_of_;      // live external id -> node
  Node entry_ = 0;
  int max_level_ = -1;  // -1: empty graph
};

}  // namespace semcache
