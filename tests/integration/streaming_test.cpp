#include <gtest/gtest.h>

#include "support/test_env.hpp"

namespace semcache::test {
namespace {

TEST(Streaming, EventsArriveIncrementallyAndReassemble) {
  auto m = fast_mock();
  m.token_delay = std::chrono::milliseconds(30);
  m.response_words = 10;  // 12 content tokens => >= ~360 ms of streaming
  TestEnv env(m);
  const auto body = chat_body("Explain caching", /*stream=*/true);
  auto r = request(env.port(), {.body = body});
  ASSERT_EQ(r.status, 200u);
  EXPECT_TRUE(r.error.empty()) << r.error;
  EXPECT_EQ(r.header("content-type"), "text/event-stream");
  EXPECT_EQ(r.header("transfer-encoding"), "chunked");
  EXPECT_EQ(r.header("x-semcache"), "BYPASS");

  // Headers must be relayed when upstream starts streaming, not after it finishes.
  EXPECT_LT(r.header_ms, r.total_ms / 2) << "header " << r.header_ms << "ms, total " << r.total_ms << "ms";
  EXPECT_GE(r.total_ms, 300);

  auto events = sse_events(r.body);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), "[DONE]");
  const auto expected = mock::make_completion(nlohmann::json::parse(body), 10);
  EXPECT_EQ(assembled_content(events), expected.content());
}

TEST(Streaming, IncludeUsageChunkIsRelayed) {
  TestEnv env;
  auto r = chat(env.port(), "q", true, {{"stream_options", {{"include_usage", true}}}});
  ASSERT_EQ(r.status, 200u);
  auto events = sse_events(r.body);
  ASSERT_GE(events.size(), 2u);
  const auto& usage = events[events.size() - 2];
  ASSERT_TRUE(usage.contains("usage"));
  EXPECT_EQ(usage["usage"]["completion_tokens"], 14);
}

TEST(Streaming, KeepAliveConnectionServesSequentialStreams) {
  TestEnv env;
  for (int i = 0; i < 3; ++i) {
    auto r = chat(env.port(), "q" + std::to_string(i), true);
    ASSERT_EQ(r.status, 200u);
    EXPECT_EQ(sse_events(r.body).back(), "[DONE]");
  }
  EXPECT_EQ(env.mock().chat_requests(), 3u);
}

}  // namespace
}  // namespace semcache::test
