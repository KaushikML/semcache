#pragma once

// BERT uncased text normalization and pre-tokenizer character classes, matching HuggingFace
// `tokenizers` (BertNormalizer + BertPreTokenizer). Character classes come from tables generated
// from HF itself (tools/gen_bert_tables.py); NFD and lowercase mappings come from utf8proc, applied
// only to the code points HF maps.

#include <string>
#include <string_view>

namespace semcache {

// HF order: drop control chars and map whitespace to ' ' (clean_text), pad CJK ideographs with
// spaces, NFD the whole string, drop combining marks (strip_accents), lowercase.
// Invalid UTF-8 bytes are dropped.
[[nodiscard]] std::u32string bert_normalize(std::string_view utf8);

// Pre-tokenizer classes, applied to normalized text.
[[nodiscard]] bool is_bert_split_space(char32_t c);
[[nodiscard]] bool is_bert_punctuation(char32_t c);

}  // namespace semcache
