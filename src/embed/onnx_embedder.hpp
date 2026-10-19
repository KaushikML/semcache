#pragma once

// Embedder backed by ONNX Runtime running all-MiniLM-L6-v2: WordPiece ids -> transformer ->
// mean pooling over the attention mask -> L2 normalization (same as sentence-transformers).

#include <onnxruntime_cxx_api.h>

#include <stdexcept>

#include "config/config.hpp"
#include "embed/embedder.hpp"
#include "embed/wordpiece_tokenizer.hpp"

namespace semcache {

class EmbedderLoadError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class OnnxEmbedder final : public Embedder {
 public:
  // Loads vocab and model. Throws EmbedderLoadError if either is missing or the model does not
  // have the expected inputs/outputs.
  explicit OnnxEmbedder(const EmbedderConfig& cfg);

  [[nodiscard]] EmbedResult embed(std::span<const std::string_view> texts) override;

  // Exposed for tests and benchmarks.
  [[nodiscard]] const WordPieceTokenizer& tokenizer() const { return tokenizer_; }

 private:
  EmbedderConfig cfg_;
  WordPieceTokenizer tokenizer_;
  Ort::Env env_;
  Ort::Session session_{nullptr};
  Ort::MemoryInfo memory_info_{nullptr};
  bool has_token_type_ids_ = false;
};

}  // namespace semcache
