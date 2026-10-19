#include "cache/context_key.hpp"

#include <gtest/gtest.h>

namespace semcache {
namespace {

using nlohmann::json;

json base() {
  return json::parse(R"({
    "model": "gpt-4o-mini",
    "messages": [
      {"role": "system", "content": "You are a support bot."},
      {"role": "user", "content": "Hi"},
      {"role": "assistant", "content": "Hello! How can I help?"},
      {"role": "user", "content": "What is your refund policy?"}
    ]
  })");
}

Classification classify(const json& body, std::string_view tenant = "acme", CacheConfig cfg = {},
                        bool bypass = false) {
  return classify_request(body, tenant, cfg, bypass);
}

ContextKey key(const json& body, std::string_view tenant = "acme", CacheConfig cfg = {}) {
  auto c = classify(body, tenant, cfg);
  EXPECT_TRUE(c.request.has_value()) << to_string(c.reason);
  return c.request ? c.request->context : ContextKey{};
}

TEST(ContextKey, SameRequestSameKeyAndPromptHash) {
  auto a = classify(base());
  auto b = classify(base());
  ASSERT_TRUE(a.request && b.request);
  EXPECT_EQ(a.request->context, b.request->context);
  EXPECT_EQ(a.request->prompt_hash, b.request->prompt_hash);
  EXPECT_EQ(a.request->embed_text, "What is your refund policy?");
}

TEST(ContextKey, FinalUserMessageIsNotInTheKey) {
  auto b = base();
  b["messages"].back()["content"] = "How do I get my money back?";
  EXPECT_EQ(key(base()), key(b));
  EXPECT_NE(classify(base()).request->prompt_hash, classify(b).request->prompt_hash);
}

TEST(ContextKey, TenantIsPartOfTheKey) { EXPECT_NE(key(base(), "acme"), key(base(), "globex")); }

// ADR-002: every parameter that can change the answer must change the key.
TEST(ContextKey, EveryGenerationParameterChangesTheKey) {
  const std::vector<std::pair<std::string, json>> params = {
      {"model", "gpt-4o"},
      {"temperature", 0.7},
      {"top_p", 0.9},
      {"max_tokens", 100},
      {"max_completion_tokens", 100},
      {"stop", json::array({"\n"})},
      {"presence_penalty", 0.5},
      {"frequency_penalty", 0.5},
      {"seed", 42},
      {"response_format", {{"type", "json_object"}}},
      {"tools", json::array({{{"type", "function"}, {"function", {{"name", "lookup"}}}}})},
      {"tool_choice", "auto"},
      {"logit_bias", {{"50256", -100}}},
      {"some_future_param", true},
  };
  const auto k0 = key(base());
  for (const auto& [name, value] : params) {
    auto b = base();
    b[name] = value;
    EXPECT_NE(key(b), k0) << name;
  }
}

TEST(ContextKey, AbsentDiffersFromExplicitDefault) {
  auto b = base();
  b["temperature"] = 1.0;  // the API default, but not the same request
  EXPECT_NE(key(base()), key(b));
}

TEST(ContextKey, TransportOnlyFieldsDoNotChangeTheKey) {
  auto b = base();
  b["stream"] = true;
  b["stream_options"] = {{"include_usage", true}};
  b["user"] = "end-user-123";
  b["metadata"] = {{"trace", "x"}};
  b["store"] = false;
  EXPECT_EQ(key(base()), key(b));
}

TEST(ContextKey, JsonKeyOrderAndNumberSpellingDoNotSplitPartitions) {
  auto a = base();
  a["tools"] = json::parse(R"([{"type":"function","function":{"name":"f","parameters":{"a":1,"b":2}}}])");
  a["temperature"] = 1;
  auto b = base();
  b["tools"] = json::parse(R"([{"function":{"parameters":{"b":2,"a":1},"name":"f"},"type":"function"}])");
  b["temperature"] = 1.0;
  EXPECT_EQ(key(a), key(b));
}

TEST(ContextKey, SystemMessagesAlwaysCountInFull) {
  auto b = base();
  b["messages"][0]["content"] = "You are a pirate.";
  CacheConfig k0;
  k0.history_k = 0;
  EXPECT_NE(key(base(), "acme", k0), key(b, "acme", k0));
}

TEST(ContextKey, HistoryWindowIsLastKPriorMessages) {
  auto older = base();  // differs only in a turn before the last 2 prior messages
  older["messages"].insert(older["messages"].begin() + 1, json{{"role", "user"}, {"content", "I am on plan B"}});
  older["messages"].insert(older["messages"].begin() + 2, json{{"role", "assistant"}, {"content", "Noted."}});
  CacheConfig k2;
  k2.history_k = 2;
  EXPECT_EQ(key(base(), "acme", k2), key(older, "acme", k2)) << "documented ADR-002 tradeoff";
  CacheConfig all;
  all.history_k = -1;
  EXPECT_NE(key(base(), "acme", all), key(older, "acme", all));

  auto recent = base();
  recent["messages"][2]["content"] = "Something else";
  EXPECT_NE(key(base(), "acme", k2), key(recent, "acme", k2));
}

TEST(ContextKey, PriorToolCallsAreInTheKey) {
  auto a = base();
  a["messages"][2]["tool_calls"] = json::array({{{"id", "c1"}, {"type", "function"}}});
  EXPECT_NE(key(base()), key(a));
}

TEST(ContextKey, FieldBoundariesAreUnambiguous) {
  auto a = base();
  a["messages"][1]["content"] = "ab";
  a["messages"][2]["content"] = "c";
  auto b = base();
  b["messages"][1]["content"] = "a";
  b["messages"][2]["content"] = "bc";
  EXPECT_NE(key(a), key(b));
}

TEST(ContextKey, EmbedTextIsNfcAndTrimmed) {
  auto a = base();
  a["messages"].back()["content"] = "  café?\n";
  auto b = base();
  b["messages"].back()["content"] = "café?";
  ASSERT_TRUE(classify(a).request && classify(b).request);
  EXPECT_EQ(classify(a).request->embed_text, "café?");
  EXPECT_EQ(classify(a).request->prompt_hash, classify(b).request->prompt_hash);
}

TEST(ContextKey, TextPartsAreCacheable) {
  auto b = base();
  b["messages"].back()["content"] = json::array({{{"type", "text"}, {"text", "What is your refund policy?"}}});
  ASSERT_TRUE(classify(b).request);
  EXPECT_EQ(classify(b).request->prompt_hash, classify(base()).request->prompt_hash);
}

TEST(ContextKey, NotCacheableReasons) {
  auto expect = [](const json& b, NotCacheable reason, CacheConfig cfg = {}, bool bypass = false) {
    auto c = classify_request(b, "acme", cfg, bypass);
    EXPECT_FALSE(c.request.has_value()) << to_string(reason);
    EXPECT_EQ(c.reason, reason) << to_string(c.reason);
  };
  CacheConfig off;
  off.enabled = false;
  expect(base(), NotCacheable::cache_disabled, off);
  expect(base(), NotCacheable::bypass_header, {}, true);

  auto tool_turn = base();
  tool_turn["messages"].push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "42"}});
  expect(tool_turn, NotCacheable::last_not_user);

  auto image = base();
  image["messages"].back()["content"] =
      json::array({{{"type", "text"}, {"text", "what is this?"}}, {{"type", "image_url"}, {"image_url", {{"url", "x"}}}}});
  expect(image, NotCacheable::non_text_content);

  auto n2 = base();
  n2["n"] = 2;
  expect(n2, NotCacheable::multiple_choices);
  auto n1 = base();
  n1["n"] = 1;
  EXPECT_TRUE(classify(n1).request.has_value());

  auto lp = base();
  lp["logprobs"] = true;
  expect(lp, NotCacheable::logprobs);

  CacheConfig cool;
  cool.max_temperature = 0.5;
  auto hot = base();
  hot["temperature"] = 0.9;
  expect(hot, NotCacheable::temperature, cool);
}

TEST(ContextKey, NonObjectMessagesDoNotThrow) {
  auto b = base();
  b["messages"].insert(b["messages"].begin(), json("stray string"));
  EXPECT_NO_THROW((void)classify(b));
}

TEST(CanonicalJson, SortedCompactAndNumberNormalized) {
  EXPECT_EQ(canonical_json(json::parse(R"({"b": [1, 2.50, true, null], "a": {"y": "s", "x": 1.0}})")),
            R"({"a":{"x":1,"y":"s"},"b":[1,2.5,true,null]})");
}

}  // namespace
}  // namespace semcache
