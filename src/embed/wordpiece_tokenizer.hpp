#pragma once

// BERT uncased WordPiece tokenizer (ADR-004). Mirrors HuggingFace `tokenizers`:
//   BertNormalizer   bert_normalize() (embed/bert_normalizer.hpp)
//   BertPreTokenizer split on whitespace, isolate punctuation
//   WordPiece        greedy longest match with "##" continuations, [UNK] for unmatchable words
// Parity with HF is enforced by golden tests (tests/data/tokenizer_golden.jsonl,
// tests/data/normalizer_blocks.txt).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace semcache {

class TokenizerError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class WordPieceTokenizer {
 public:
  using TokenId = std::int64_t;

  // One token per entry; the id is the index. Throws TokenizerError if a special token is missing.
  explicit WordPieceTokenizer(std::vector<std::string> vocab);
  // Reads a vocab.txt (one token per line). Throws TokenizerError.
  [[nodiscard]] static WordPieceTokenizer from_file(const std::string& path);

  // Token ids for one sequence: [CLS] tokens... [SEP]. No truncation; callers decide what to do
  // with sequences longer than the model accepts. Invalid UTF-8 bytes are dropped.
  [[nodiscard]] std::vector<TokenId> encode(std::string_view utf8) const;

  [[nodiscard]] std::size_t vocab_size() const { return vocab_.size(); }
  [[nodiscard]] TokenId pad_id() const { return pad_id_; }

 private:
  struct Hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
  };
  using Vocab = std::unordered_map<std::string, TokenId, Hash, std::equal_to<>>;

  void word_pieces(std::u32string_view word, std::vector<TokenId>& out) const;

  Vocab vocab_;
  TokenId unk_id_ = 0;
  TokenId cls_id_ = 0;
  TokenId sep_id_ = 0;
  TokenId pad_id_ = 0;
};

}  // namespace semcache
