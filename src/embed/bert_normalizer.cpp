#include "embed/bert_normalizer.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <iterator>
#include <span>

namespace semcache {
namespace {

struct CodepointRange {
  char32_t lo;
  char32_t hi;
};

#include "embed/bert_tables.inc"

bool in(std::span<const CodepointRange> table, char32_t c) {
  auto it = std::upper_bound(table.begin(), table.end(), c,
                             [](char32_t v, const CodepointRange& r) { return v < r.lo; });
  return it != table.begin() && c <= std::prev(it)->hi;
}

// NFD + strip marks + lowercase for one code point HF maps (kMapped). Restricting utf8proc to
// kMapped pins behaviour to HF's Unicode version: utf8proc also knows mappings for newer
// characters (e.g. Unicode 16 scripts) that HF leaves unchanged.
void append_mapped(std::u32string& out, char32_t c) {
  utf8proc_int32_t buf[8];
  int boundclass = 0;
  auto n = utf8proc_decompose_char(static_cast<utf8proc_int32_t>(c), buf, std::size(buf), UTF8PROC_DECOMPOSE,
                                   &boundclass);
  if (n <= 0 || static_cast<std::size_t>(n) > std::size(buf)) {
    buf[0] = static_cast<utf8proc_int32_t>(c);
    n = 1;
  }
  for (utf8proc_ssize_t i = 0; i < n; ++i) {
    const auto d = static_cast<char32_t>(buf[i]);
    if (in(kRemoved, d)) continue;  // strip_accents
    out.push_back(static_cast<char32_t>(utf8proc_tolower(buf[i])));
  }
}

int combining_class(char32_t c) { return utf8proc_get_property(static_cast<utf8proc_int32_t>(c))->combining_class; }

// NFD canonical ordering: stable-sort each run of non-starters by combining class. Only marks
// that survive stripping remain, so runs are rare and short.
void canonical_reorder(std::u32string& s) {
  std::size_t i = 0;
  while (i < s.size()) {
    if (combining_class(s[i]) == 0) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < s.size() && combining_class(s[j]) != 0) ++j;
    std::stable_sort(s.begin() + static_cast<std::ptrdiff_t>(i), s.begin() + static_cast<std::ptrdiff_t>(j),
                     [](char32_t a, char32_t b) { return combining_class(a) < combining_class(b); });
    i = j;
  }
}

}  // namespace

std::u32string bert_normalize(std::string_view utf8) {
  std::u32string out;
  out.reserve(utf8.size());
  const auto* p = reinterpret_cast<const utf8proc_uint8_t*>(utf8.data());
  auto remaining = static_cast<utf8proc_ssize_t>(utf8.size());
  while (remaining > 0) {
    utf8proc_int32_t cp = 0;
    auto n = utf8proc_iterate(p, remaining, &cp);
    if (n <= 0) {  // invalid byte: skip it
      ++p;
      --remaining;
      continue;
    }
    p += n;
    remaining -= n;

    const auto c = static_cast<char32_t>(cp);
    if (in(kRemoved, c)) continue;  // clean_text
    if (in(kWhitespace, c)) {
      out.push_back(U' ');
      continue;
    }
    const bool pad = in(kCjkPadded, c);  // handle_chinese_chars
    if (pad) out.push_back(U' ');
    if (in(kMapped, c)) {
      append_mapped(out, c);
    } else {
      out.push_back(c);
    }
    if (pad) out.push_back(U' ');
  }
  canonical_reorder(out);
  return out;
}

bool is_bert_split_space(char32_t c) { return in(kSplitSpace, c); }
bool is_bert_punctuation(char32_t c) { return in(kPunctuation, c); }

}  // namespace semcache
