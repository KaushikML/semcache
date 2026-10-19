#include "embed/onnx_embedder.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <thread>

#include "support/model_paths.hpp"

namespace semcache {
namespace {

float dot(const Embedding& a, const Embedding& b) {
  float s = 0;
  for (std::size_t i = 0; i < kEmbeddingDim; ++i) s += a[i] * b[i];
  return s;
}

struct GoldenRow {
  std::string text;
  Embedding expected;
};

std::vector<GoldenRow> load_golden() {
  std::ifstream in(test::data_dir() + "/embedding_golden.jsonl");
  std::vector<GoldenRow> rows;
  for (std::string line; std::getline(in, line);) {
    auto j = nlohmann::json::parse(line);
    GoldenRow r{j["text"].get<std::string>(), {}};
    auto v = j["embedding"].get<std::vector<float>>();
    if (v.size() != kEmbeddingDim) throw std::runtime_error("bad golden row");
    std::copy(v.begin(), v.end(), r.expected.begin());
    rows.push_back(std::move(r));
  }
  return rows;
}

// One model load shared by all tests in this file (loading takes ~100 ms).
class EmbedderTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    try {
      embedder_ = new OnnxEmbedder(test::embedder_config());
    } catch (const EmbedderLoadError& e) {
      load_error_ = e.what();
    }
  }
  static void TearDownTestSuite() {
    delete embedder_;
    embedder_ = nullptr;
  }
  void SetUp() override {
    ASSERT_NE(embedder_, nullptr) << load_error_ << " (run scripts/fetch-model.sh)";
  }

  static Embedding one(std::string_view text) {
    std::string_view in[] = {text};
    auto r = embedder_->embed(in);
    EXPECT_EQ(r.error, EmbedError::none) << r.detail;
    return r.embeddings.at(0);
  }

  static inline OnnxEmbedder* embedder_ = nullptr;
  static inline std::string load_error_;
};

// Phase 2 gate: cosine > 0.999 against sentence-transformers on every golden sentence.
TEST_F(EmbedderTest, MatchesSentenceTransformersGolden) {
  auto rows = load_golden();
  ASSERT_GT(rows.size(), 100u);
  float worst = 1.0f;
  std::string worst_text;
  for (const auto& r : rows) {
    float c = dot(one(r.text), r.expected);
    if (c < worst) {
      worst = c;
      worst_text = r.text;
    }
  }
  std::printf("min cosine vs sentence-transformers over %zu sentences: %.7f\n", rows.size(), worst);
  EXPECT_GT(worst, 0.999f) << nlohmann::json(worst_text).dump();
}

TEST_F(EmbedderTest, BatchedMatchesSingle) {
  auto rows = load_golden();
  std::vector<std::string_view> texts;
  for (std::size_t i = 0; i < 16 && i < rows.size(); ++i) texts.push_back(rows[i].text);
  auto batch = embedder_->embed(texts);
  ASSERT_EQ(batch.error, EmbedError::none) << batch.detail;
  ASSERT_EQ(batch.embeddings.size(), texts.size());
  for (std::size_t i = 0; i < texts.size(); ++i) {
    EXPECT_GT(dot(batch.embeddings[i], one(texts[i])), 0.99999f) << "padding must not change the result, row " << i;
  }
}

TEST_F(EmbedderTest, OutputIsUnitLength) {
  for (auto text : {"", "a", "What is your refund policy?", "😀"}) {
    auto v = one(text);
    EXPECT_NEAR(std::sqrt(dot(v, v)), 1.0f, 1e-5f) << text;
  }
}

TEST_F(EmbedderTest, ParaphrasesAreCloserThanUnrelatedText) {
  auto a = one("How do I get a refund?");
  auto b = one("What is your refund policy?");
  auto c = one("Explain how TCP congestion control works.");
  EXPECT_GT(dot(a, b), dot(a, c) + 0.3f);
}

TEST_F(EmbedderTest, RejectsTextOverMaxTokensInsteadOfTruncating) {
  const auto max = test::embedder_config().max_tokens;
  std::string fits;
  for (std::size_t i = 0; i + 2 < max; ++i) fits += "a ";
  ASSERT_EQ(embedder_->tokenizer().encode(fits).size(), max);
  std::string_view ok[] = {fits};
  EXPECT_EQ(embedder_->embed(ok).error, EmbedError::none);

  const std::string over = fits + "a";
  std::string_view batch[] = {"short", over};
  auto r = embedder_->embed(batch);
  EXPECT_EQ(r.error, EmbedError::too_long);
  EXPECT_TRUE(r.embeddings.empty()) << "a batch fails as a whole";
  EXPECT_EQ(r.detail.find("a a"), std::string::npos) << "detail must not echo the prompt";
}

TEST_F(EmbedderTest, EmptyBatchIsEmptyResult) {
  auto r = embedder_->embed({});
  EXPECT_EQ(r.error, EmbedError::none);
  EXPECT_TRUE(r.embeddings.empty());
}

TEST_F(EmbedderTest, ConcurrentCallsAreDeterministic) {
  constexpr int kThreads = 4;
  constexpr int kIters = 10;
  const std::string_view texts[] = {"How do I reset my password?", "Do you ship internationally?"};
  const Embedding ref0 = one(texts[0]);
  const Embedding ref1 = one(texts[1]);
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kIters; ++i) {
        auto r = embedder_->embed(texts);
        if (r.error != EmbedError::none || dot(r.embeddings[0], ref0) < 0.99999f ||
            dot(r.embeddings[1], ref1) < 0.99999f) {
          bad.fetch_add(1);
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(bad.load(), 0);
}

TEST(EmbedderLoad, MissingFilesAreLoadErrors) {
  auto cfg = test::embedder_config();
  cfg.model_path = "/nonexistent/model.onnx";
  EXPECT_THROW(OnnxEmbedder{cfg}, EmbedderLoadError);
  cfg = test::embedder_config();
  cfg.vocab_path = "/nonexistent/vocab.txt";
  EXPECT_THROW(OnnxEmbedder{cfg}, EmbedderLoadError);
}

TEST(EmbedderLoad, CorruptModelIsLoadError) {
  auto path = std::string(::testing::TempDir()) + "/corrupt.onnx";
  std::ofstream(path) << "not an onnx model";
  auto cfg = test::embedder_config();
  cfg.model_path = path;
  EXPECT_THROW(OnnxEmbedder{cfg}, EmbedderLoadError);
}

}  // namespace
}  // namespace semcache
