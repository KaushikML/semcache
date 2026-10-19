#pragma once

// Deterministic Embedder for tests that must not depend on the model: same text => same unit
// vector, unrelated texts => near-orthogonal vectors. Tests can make one text an alias of another
// at a chosen cosine, force errors, and hold embeds at a gate to fill the embed queue.

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>

#include "embed/embedder.hpp"

namespace semcache::test {

class FakeEmbedder final : public Embedder {
 public:
  EmbedResult embed(std::span<const std::string_view> texts) override {
    calls_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(mu_);
    cv_.wait(lock, [this] { return !gated_; });
    EmbedResult r;
    if (fail_ != EmbedError::none) {
      r.error = fail_;
      r.detail = "forced by test";
      return r;
    }
    for (auto t : texts) {
      if (auto it = aliases_.find(std::string(t)); it != aliases_.end()) {
        r.embeddings.push_back(near(vec(it->second.first), it->second.second, std::string(t)));
      } else {
        r.embeddings.push_back(vec(t));
      }
    }
    return r;
  }

  // `text` embeds at cosine `cos` from `canonical`.
  void alias(const std::string& text, const std::string& canonical, float cos) {
    std::lock_guard lock(mu_);
    aliases_[text] = {canonical, cos};
  }
  void fail_with(EmbedError e) {
    std::lock_guard lock(mu_);
    fail_ = e;
  }
  void set_gate(bool closed) {
    {
      std::lock_guard lock(mu_);
      gated_ = closed;
    }
    cv_.notify_all();
  }

  [[nodiscard]] int calls() const { return calls_.load(std::memory_order_relaxed); }

  static Embedding vec(std::string_view text) {
    std::mt19937 rng(static_cast<std::uint32_t>(std::hash<std::string_view>{}(text)));
    std::normal_distribution<float> dist;
    Embedding v{};
    float norm = 0;
    for (auto& x : v) {
      x = dist(rng);
      norm += x * x;
    }
    for (auto& x : v) x /= std::sqrt(norm);
    return v;
  }

  // A unit vector at exactly cosine `cos` from `base`.
  static Embedding near(const Embedding& base, float cos, const std::string& seed) {
    auto noise = vec("noise:" + seed);
    float d = 0;
    for (std::size_t i = 0; i < kEmbeddingDim; ++i) d += noise[i] * base[i];
    Embedding ortho{};
    float n = 0;
    for (std::size_t i = 0; i < kEmbeddingDim; ++i) {
      ortho[i] = noise[i] - d * base[i];
      n += ortho[i] * ortho[i];
    }
    const float s = std::sqrt(1 - cos * cos) / std::sqrt(n);
    Embedding v{};
    for (std::size_t i = 0; i < kEmbeddingDim; ++i) v[i] = cos * base[i] + s * ortho[i];
    return v;
  }

 private:
  std::atomic<int> calls_{0};
  std::mutex mu_;
  std::condition_variable cv_;
  bool gated_ = false;
  EmbedError fail_ = EmbedError::none;
  std::map<std::string, std::pair<std::string, float>> aliases_;
};

}  // namespace semcache::test
