#pragma once

// Deterministic Embedder for tests that must not depend on the model: same text => same unit
// vector, different texts => (almost certainly) different vectors.

#include <atomic>
#include <cmath>
#include <functional>
#include <random>

#include "embed/embedder.hpp"

namespace semcache::test {

class FakeEmbedder final : public Embedder {
 public:
  EmbedResult embed(std::span<const std::string_view> texts) override {
    calls_.fetch_add(1, std::memory_order_relaxed);
    EmbedResult r;
    for (auto t : texts) {
      std::mt19937 rng(static_cast<std::uint32_t>(std::hash<std::string_view>{}(t)));
      std::normal_distribution<float> dist;
      Embedding v{};
      float norm = 0;
      for (auto& x : v) {
        x = dist(rng);
        norm += x * x;
      }
      for (auto& x : v) x /= std::sqrt(norm);
      r.embeddings.push_back(v);
    }
    return r;
  }

  [[nodiscard]] int calls() const { return calls_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int> calls_{0};
};

}  // namespace semcache::test
