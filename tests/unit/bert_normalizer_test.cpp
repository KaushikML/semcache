#include "embed/bert_normalizer.hpp"

#include <gtest/gtest.h>
#include <utf8proc.h>

#include <fstream>
#include <sstream>

#include "common/sha256.hpp"
#include "support/model_paths.hpp"

namespace semcache {
namespace {

std::string utf8(std::u32string_view s) {
  std::string out;
  for (char32_t c : s) {
    utf8proc_uint8_t buf[4];
    auto n = utf8proc_encode_char(static_cast<utf8proc_int32_t>(c), buf);
    out.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
  }
  return out;
}

TEST(BertNormalizer, FoldsCaseAndAccentsAndPadsCjk) {
  EXPECT_EQ(utf8(bert_normalize("Café ÀÉ")), "cafe ae");
  EXPECT_EQ(utf8(bert_normalize("你好")), " 你  好 ");
  EXPECT_EQ(utf8(bert_normalize("a\tb c")), "a b c");
  EXPECT_EQ(utf8(bert_normalize(std::string_view("a\x00\x01" "b", 4))), "ab");
}

TEST(BertNormalizer, CjkCompatibilityIdeographIsPaddedThenDecomposed) {
  EXPECT_EQ(utf8(bert_normalize("豈")), " 豈 ");
}

// Every code point, compared with HF in 4096-code-point blocks (tools/gen_bert_tables.py).
TEST(BertNormalizer, MatchesHuggingFaceOnEveryCodePoint) {
  std::ifstream in(test::data_dir() + "/normalizer_blocks.txt");
  ASSERT_TRUE(in) << "missing tests/data/normalizer_blocks.txt";
  std::size_t blocks = 0;
  std::size_t bad = 0;
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string start_hex;
    std::string expected;
    fields >> start_hex >> expected;
    const auto start = static_cast<char32_t>(std::stoul(start_hex, nullptr, 16));
    std::string concat;
    std::ostringstream dump;  // "cp: out out ..." per code point, for tools/gen_bert_tables.py --diff
    for (char32_t cp = start; cp < start + 4096 && cp < 0x110000; ++cp) {
      if (cp >= 0xD800 && cp <= 0xDFFF) continue;
      auto out = bert_normalize(utf8(std::u32string(1, cp)));
      concat += utf8(out);
      concat.push_back('\0');
      dump << std::hex << static_cast<std::uint32_t>(cp) << ':';
      for (char32_t c : out) dump << ' ' << static_cast<std::uint32_t>(c);
      dump << '\n';
    }
    ++blocks;
    if (to_hex(sha256(concat)) != expected && ++bad <= 10) {
      const auto path = ::testing::TempDir() + "normalizer_block_" + start_hex + ".txt";
      std::ofstream(path) << dump.str();
      ADD_FAILURE() << "normalization differs from HF in block U+" << start_hex
                    << "; inspect with: tools/gen_bert_tables.py --diff " << path;
    }
  }
  EXPECT_EQ(blocks, 272u);
  EXPECT_EQ(bad, 0u);
}

TEST(BertNormalizer, PreTokenizerClasses) {
  EXPECT_TRUE(is_bert_split_space(U' '));
  EXPECT_FALSE(is_bert_split_space(U'a'));
  for (char32_t c : {U'!', U'$', U'^', U'`', U'|', U'~', U'、', U'¿'}) EXPECT_TRUE(is_bert_punctuation(c)) << static_cast<std::uint32_t>(c);
  for (char32_t c : {U'a', U'0', U' ', U'一'}) EXPECT_FALSE(is_bert_punctuation(c)) << static_cast<std::uint32_t>(c);
}

}  // namespace
}  // namespace semcache
