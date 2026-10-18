#include <gtest/gtest.h>

#include "mock_upstream.hpp"

namespace semcache::mock {
namespace {

nlohmann::json req(const std::string& prompt, const std::string& model = "m") {
  return {{"model", model}, {"messages", {{{"role", "system"}, {"content", "be brief"}}, {{"role", "user"}, {"content", prompt}}}}};
}

TEST(MockResponse, Deterministic) {
  auto a = make_completion(req("hello world"), 10);
  auto b = make_completion(req("hello world"), 10);
  EXPECT_EQ(completion_json(a), completion_json(b));
  EXPECT_EQ(a.completion_tokens, 12);  // "Mock" " answer:" + 10 words
  EXPECT_EQ(a.prompt_tokens, 4);       // "be brief" + "hello world"
}

TEST(MockResponse, DependsOnPromptAndModel) {
  auto a = make_completion(req("hello world"), 10);
  EXPECT_NE(a.id, make_completion(req("hello there"), 10).id);
  EXPECT_NE(a.id, make_completion(req("hello world", "other"), 10).id);
}

TEST(MockResponse, StreamEventsReassembleToContent) {
  auto c = make_completion(req("q"), 5);
  auto events = stream_events(c, /*include_usage=*/false);
  ASSERT_EQ(events.size(), c.tokens.size() + 3);  // role + tokens + finish + [DONE]
  EXPECT_EQ(events.back(), "data: [DONE]\n\n");
  std::string assembled;
  for (std::size_t i = 1; i + 2 < events.size(); ++i) {
    auto j = nlohmann::json::parse(events[i].substr(6));
    assembled += j["choices"][0]["delta"]["content"].get<std::string>();
  }
  EXPECT_EQ(assembled, c.content());
  auto finish = nlohmann::json::parse(events[events.size() - 2].substr(6));
  EXPECT_EQ(finish["choices"][0]["finish_reason"], "stop");

  auto with_usage = stream_events(c, true);
  auto usage = nlohmann::json::parse(with_usage[with_usage.size() - 2].substr(6));
  EXPECT_EQ(usage["usage"]["completion_tokens"], c.completion_tokens);
}

TEST(MockResponse, ParseFault) {
  auto f = parse_fault("status=503;reset-after=3;delay=50;hang");
  EXPECT_EQ(f.status, 503u);
  EXPECT_EQ(f.reset_after, 3);
  EXPECT_EQ(f.delay_ms, 50);
  EXPECT_TRUE(f.hang);
  auto none = parse_fault("");
  EXPECT_EQ(none.status, 0u);
  EXPECT_EQ(none.reset_after, -1);
  EXPECT_FALSE(none.hang);
}

}  // namespace
}  // namespace semcache::mock
