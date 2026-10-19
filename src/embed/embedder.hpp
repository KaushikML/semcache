#pragma once

// Embedder: text -> L2-normalized sentence embedding (cosine similarity = dot product).
// Implementations must be safe to call concurrently from several threads (the embed pool).

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace semcache {

// all-MiniLM-L6-v2 output size. Checked against the model when it is loaded.
inline constexpr std::size_t kEmbeddingDim = 384;
using Embedding = std::array<float, kEmbeddingDim>;

enum class EmbedError {
  none,
  too_long,   // a text exceeds embedder.max_tokens; never embed a truncated prompt (design §5.4)
  inference,  // the runtime failed
};

[[nodiscard]] std::string_view to_string(EmbedError e);

struct EmbedResult {
  EmbedError error = EmbedError::none;
  std::string detail;                 // for logs; never contains the input text
  std::vector<Embedding> embeddings;  // one per input, in order, when error == none
};

class Embedder {
 public:
  virtual ~Embedder() = default;
  // Embeds a batch. Fails as a whole: if any text is too long, nothing is embedded.
  [[nodiscard]] virtual EmbedResult embed(std::span<const std::string_view> texts) = 0;
};

}  // namespace semcache
