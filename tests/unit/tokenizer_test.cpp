#include "embed/wordpiece_tokenizer.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>

#include "support/model_paths.hpp"

namespace semcache {
namespace {

WordPieceTokenizer tiny() {
  return WordPieceTokenizer({"[PAD]", "[UNK]", "[CLS]", "[SEP]", "un", "##aff", "##able", "hello", ",", "!", "cafe",
                             "你", "好", "a"});
}

std::vector<WordPieceTokenizer::TokenId> ids(std::initializer_list<WordPieceTokenizer::TokenId> l) { return l; }

TEST(Tokenizer, GreedyLongestMatchWithContinuations) {
  EXPECT_EQ(tiny().encode("unaffable"), ids({2, 4, 5, 6, 3}));
}

TEST(Tokenizer, UnmatchableWordBecomesSingleUnk) {
  EXPECT_EQ(tiny().encode("unaffxable"), ids({2, 1, 3})) << "pieces already matched are discarded";
}

TEST(Tokenizer, PunctuationIsSplitAndCaseAndAccentsFolded) {
  EXPECT_EQ(tiny().encode("HELLO,Café!"), ids({2, 7, 8, 10, 9, 3}));
}

TEST(Tokenizer, CjkIdeographsAreSeparateWords) {
  EXPECT_EQ(tiny().encode("你好a"), ids({2, 11, 12, 13, 3}));
}

TEST(Tokenizer, EmptyAndWhitespaceOnlyGiveOnlySpecials) {
  EXPECT_EQ(tiny().encode(""), ids({2, 3}));
  EXPECT_EQ(tiny().encode(" \t\n 　"), ids({2, 3}));
}

TEST(Tokenizer, ControlCharsAndInvalidUtf8AreDropped) {
  EXPECT_EQ(tiny().encode(std::string_view("hel\x01lo\x00!", 8)), ids({2, 7, 9, 3}));
  EXPECT_EQ(tiny().encode("hel\xff\xfelo"), ids({2, 7, 3}));
}

TEST(Tokenizer, WordsOverHundredCharsAreUnk) {
  EXPECT_EQ(tiny().encode(std::string(101, 'a')), ids({2, 1, 3}));
}

TEST(Tokenizer, MissingSpecialTokenIsAnError) {
  EXPECT_THROW(WordPieceTokenizer({"[PAD]", "[UNK]", "[CLS]"}), TokenizerError);
  EXPECT_THROW((void)WordPieceTokenizer::from_file("/nonexistent/vocab.txt"), TokenizerError);
}

std::string escaped(const std::string& s) { return nlohmann::json(s).dump(); }

// ADR-004 gate: exact id equality with HF BertTokenizerFast on every golden string.
TEST(Tokenizer, MatchesHuggingFaceGolden) {
  auto tok = WordPieceTokenizer::from_file(test::vocab_path());
  std::ifstream in(test::data_dir() + "/tokenizer_golden.jsonl");
  ASSERT_TRUE(in) << "missing tests/data/tokenizer_golden.jsonl";
  std::size_t total = 0;
  std::size_t mismatches = 0;
  for (std::string line; std::getline(in, line);) {
    auto row = nlohmann::json::parse(line);
    auto text = row["text"].get<std::string>();
    auto expected = row["ids"].get<std::vector<WordPieceTokenizer::TokenId>>();
    ++total;
    auto got = tok.encode(text);
    if (got != expected && ++mismatches <= 10) {
      ADD_FAILURE() << "text " << escaped(text) << "\n  expected " << nlohmann::json(expected).dump()
                    << "\n  got      " << nlohmann::json(got).dump();
    }
  }
  EXPECT_GT(total, 3000u);
  EXPECT_EQ(mismatches, 0u) << "of " << total;
}

}  // namespace
}  // namespace semcache
