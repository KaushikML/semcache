// The cache with the real all-MiniLM-L6-v2 embedder end to end. Similarities quoted below were
// measured with sentence-transformers (tools/ environment) at the pinned model revision.

#include <gtest/gtest.h>

#include "embed/onnx_embedder.hpp"
#include "support/model_paths.hpp"
#include "support/test_env.hpp"

namespace semcache::test {
namespace {

class SemanticModel : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { embedder_ = std::make_unique<OnnxEmbedder>(embedder_config()); }
  static void TearDownTestSuite() { embedder_.reset(); }
  static inline std::unique_ptr<OnnxEmbedder> embedder_;
};

TEST_F(SemanticModel, RewordingsHitAndDifferentQuestionsMiss) {
  TestEnv env(fast_mock(), {}, WithEmbedder::custom, embedder_.get());  // default threshold 0.90
  ASSERT_EQ(chat(env.port(), "How do I reset my password?").header("x-semcache"), "MISS");
  ASSERT_EQ(chat(env.port(), "What is your refund policy?").header("x-semcache"), "MISS");

  auto reworded = chat(env.port(), "How can I reset my password?");  // 0.9917
  EXPECT_EQ(reworded.header("x-semcache"), "HIT");
  EXPECT_GT(std::stof(reworded.header("x-semcache-similarity")), 0.98f);

  EXPECT_EQ(chat(env.port(), "what's your refund policy").header("x-semcache"), "HIT");  // 0.9843

  auto different = chat(env.port(), "What is your shipping policy?");  // 0.5309 vs refund
  EXPECT_EQ(different.header("x-semcache"), "MISS");
  EXPECT_LT(std::stof(different.header("x-semcache-similarity")), 0.7f);
  EXPECT_EQ(env.mock().chat_requests(), 3u);
}

}  // namespace
}  // namespace semcache::test
