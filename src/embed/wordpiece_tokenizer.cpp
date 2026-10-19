#include "embed/wordpiece_tokenizer.hpp"

#include <utf8proc.h>

#include <fstream>

#include "embed/bert_normalizer.hpp"

namespace semcache {
namespace {

// Part of the model's tokenizer definition (HF `max_input_chars_per_word`), not a tunable.
constexpr std::size_t kMaxCharsPerWord = 100;
constexpr std::string_view kContinuationPrefix = "##";

void append_utf8(std::string& out, char32_t c) {
  utf8proc_uint8_t buf[4];
  auto n = utf8proc_encode_char(static_cast<utf8proc_int32_t>(c), buf);
  out.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
}

}  // namespace

WordPieceTokenizer::WordPieceTokenizer(std::vector<std::string> vocab) {
  vocab_.reserve(vocab.size());
  for (std::size_t i = 0; i < vocab.size(); ++i) {
    vocab_.emplace(std::move(vocab[i]), static_cast<TokenId>(i));  // first occurrence wins
  }
  auto special = [this](std::string_view name) {
    auto it = vocab_.find(name);
    if (it == vocab_.end()) throw TokenizerError("vocab is missing special token " + std::string(name));
    return it->second;
  };
  unk_id_ = special("[UNK]");
  cls_id_ = special("[CLS]");
  sep_id_ = special("[SEP]");
  pad_id_ = special("[PAD]");
}

WordPieceTokenizer WordPieceTokenizer::from_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw TokenizerError("cannot open vocab file: " + path);
  std::vector<std::string> vocab;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    vocab.push_back(std::move(line));
  }
  return WordPieceTokenizer(std::move(vocab));
}

std::vector<WordPieceTokenizer::TokenId> WordPieceTokenizer::encode(std::string_view utf8) const {
  const auto text = bert_normalize(utf8);
  std::vector<TokenId> ids;
  ids.reserve(text.size() / 4 + 2);
  ids.push_back(cls_id_);

  // BertPreTokenizer: whitespace separates words, each punctuation char is its own word.
  std::size_t start = 0;
  auto flush = [&](std::size_t end) {
    if (end > start) word_pieces(std::u32string_view(text).substr(start, end - start), ids);
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char32_t c = text[i];
    if (is_bert_split_space(c)) {
      flush(i);
      start = i + 1;
    } else if (is_bert_punctuation(c)) {
      flush(i);
      word_pieces(std::u32string_view(text).substr(i, 1), ids);
      start = i + 1;
    }
  }
  flush(text.size());

  ids.push_back(sep_id_);
  return ids;
}

void WordPieceTokenizer::word_pieces(std::u32string_view word, std::vector<TokenId>& out) const {
  if (word.size() > kMaxCharsPerWord) {
    out.push_back(unk_id_);
    return;
  }
  // Byte offset of each code point so candidate substrings can be sliced from one UTF-8 buffer.
  std::string utf8;
  std::vector<std::size_t> offsets;
  offsets.reserve(word.size() + 1);
  for (char32_t c : word) {
    offsets.push_back(utf8.size());
    append_utf8(utf8, c);
  }
  offsets.push_back(utf8.size());

  const auto first = out.size();
  std::string candidate;
  std::size_t begin = 0;
  while (begin < word.size()) {
    bool matched = false;
    for (std::size_t end = word.size(); end > begin; --end) {
      candidate.clear();
      if (begin > 0) candidate.append(kContinuationPrefix);
      candidate.append(utf8, offsets[begin], offsets[end] - offsets[begin]);
      if (auto it = vocab_.find(candidate); it != vocab_.end()) {
        out.push_back(it->second);
        begin = end;
        matched = true;
        break;
      }
    }
    if (!matched) {  // the whole word becomes [UNK], discarding pieces already emitted
      out.resize(first);
      out.push_back(unk_id_);
      return;
    }
  }
}

}  // namespace semcache
