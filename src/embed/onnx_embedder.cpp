#include "embed/onnx_embedder.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace semcache {
namespace {

constexpr const char* kInputIds = "input_ids";
constexpr const char* kAttentionMask = "attention_mask";
constexpr const char* kTokenTypeIds = "token_type_ids";
constexpr const char* kLastHiddenState = "last_hidden_state";
// torch.nn.functional.normalize's default eps, so all-zero vectors behave like sentence-transformers.
constexpr float kNormEps = 1e-12f;

Ort::SessionOptions session_options(const EmbedderConfig& cfg) {
  Ort::SessionOptions opts;
  opts.SetIntraOpNumThreads(cfg.intra_op_threads);
  opts.SetInterOpNumThreads(1);
  opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
  opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  // Threads must not busy-wait between Runs: the embed pool already owns the cores it needs.
  opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
  return opts;
}

bool has_name(const Ort::Session& s, bool input, std::string_view name) {
  Ort::AllocatorWithDefaultOptions alloc;
  const auto count = input ? s.GetInputCount() : s.GetOutputCount();
  for (std::size_t i = 0; i < count; ++i) {
    auto n = input ? s.GetInputNameAllocated(i, alloc) : s.GetOutputNameAllocated(i, alloc);
    if (name == n.get()) return true;
  }
  return false;
}

}  // namespace

std::string_view to_string(EmbedError e) {
  switch (e) {
    case EmbedError::none: return "none";
    case EmbedError::too_long: return "too_long";
    case EmbedError::inference: return "inference";
  }
  return "unknown";
}

OnnxEmbedder::OnnxEmbedder(const EmbedderConfig& cfg)
    : cfg_(cfg),
      tokenizer_([&] {
        try {
          return WordPieceTokenizer::from_file(cfg.vocab_path);
        } catch (const TokenizerError& e) {
          throw EmbedderLoadError(e.what());
        }
      }()),
      env_(ORT_LOGGING_LEVEL_WARNING, "semcache") {
  if (!std::filesystem::is_regular_file(cfg.model_path)) {
    throw EmbedderLoadError("model file not found: " + cfg.model_path);
  }
  try {
    session_ = Ort::Session(env_, cfg.model_path.c_str(), session_options(cfg));
    memory_info_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  } catch (const Ort::Exception& e) {
    throw EmbedderLoadError(std::string("cannot load model: ") + e.what());
  }
  if (!has_name(session_, true, kInputIds) || !has_name(session_, true, kAttentionMask) ||
      !has_name(session_, false, kLastHiddenState)) {
    throw EmbedderLoadError("model must take input_ids/attention_mask and output last_hidden_state");
  }
  has_token_type_ids_ = has_name(session_, true, kTokenTypeIds);

  // Check the hidden size with a one-token probe rather than trusting static shape metadata.
  std::string_view probe[] = {""};
  auto r = embed(probe);
  if (r.error != EmbedError::none) throw EmbedderLoadError("model probe failed: " + r.detail);
}

EmbedResult OnnxEmbedder::embed(std::span<const std::string_view> texts) {
  EmbedResult result;
  if (texts.empty()) return result;

  std::vector<std::vector<WordPieceTokenizer::TokenId>> encoded;
  encoded.reserve(texts.size());
  std::size_t seq_len = 0;
  for (auto t : texts) {
    encoded.push_back(tokenizer_.encode(t));
    if (encoded.back().size() > cfg_.max_tokens) {
      result.error = EmbedError::too_long;
      result.detail = std::to_string(encoded.back().size()) + " tokens > embedder.max_tokens";
      return result;
    }
    seq_len = std::max(seq_len, encoded.back().size());
  }

  // Right-pad to the longest sequence; padding is masked out of attention and pooling.
  const auto batch = texts.size();
  std::vector<std::int64_t> ids(batch * seq_len, tokenizer_.pad_id());
  std::vector<std::int64_t> mask(batch * seq_len, 0);
  std::vector<std::int64_t> types(batch * seq_len, 0);
  for (std::size_t b = 0; b < batch; ++b) {
    std::copy(encoded[b].begin(), encoded[b].end(), ids.begin() + static_cast<std::ptrdiff_t>(b * seq_len));
    std::fill_n(mask.begin() + static_cast<std::ptrdiff_t>(b * seq_len), encoded[b].size(), 1);
  }

  const std::array<std::int64_t, 2> shape = {static_cast<std::int64_t>(batch), static_cast<std::int64_t>(seq_len)};
  std::vector<Ort::Value> inputs;
  std::vector<const char*> input_names = {kInputIds, kAttentionMask};
  inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory_info_, ids.data(), ids.size(), shape.data(), shape.size()));
  inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(memory_info_, mask.data(), mask.size(), shape.data(), shape.size()));
  if (has_token_type_ids_) {
    input_names.push_back(kTokenTypeIds);
    inputs.push_back(
        Ort::Value::CreateTensor<std::int64_t>(memory_info_, types.data(), types.size(), shape.data(), shape.size()));
  }
  const char* output_names[] = {kLastHiddenState};

  std::vector<Ort::Value> outputs;
  try {
    outputs = session_.Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(), inputs.size(),
                           output_names, 1);
  } catch (const Ort::Exception& e) {
    result.error = EmbedError::inference;
    result.detail = e.what();
    return result;
  }

  const auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
  if (out_shape.size() != 3 || out_shape[0] != shape[0] || out_shape[1] != shape[1] ||
      out_shape[2] != static_cast<std::int64_t>(kEmbeddingDim)) {
    result.error = EmbedError::inference;
    result.detail = "unexpected last_hidden_state shape";
    return result;
  }

  // Mean pooling over real tokens, then L2 normalization.
  const float* hidden = outputs[0].GetTensorData<float>();
  result.embeddings.resize(batch);
  for (std::size_t b = 0; b < batch; ++b) {
    auto& v = result.embeddings[b];
    v.fill(0.0f);
    const auto n = encoded[b].size();
    for (std::size_t t = 0; t < n; ++t) {
      const float* row = hidden + (b * seq_len + t) * kEmbeddingDim;
      for (std::size_t d = 0; d < kEmbeddingDim; ++d) v[d] += row[d];
    }
    float norm = 0.0f;
    for (auto& x : v) {
      x /= static_cast<float>(n);
      norm += x * x;
    }
    const float inv = 1.0f / std::max(std::sqrt(norm), kNormEps);
    for (auto& x : v) x *= inv;
  }
  return result;
}

}  // namespace semcache
