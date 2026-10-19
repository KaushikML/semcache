#include "cache/completion_codec.hpp"

#include <gtest/gtest.h>

#include <random>

#include "mock_upstream.hpp"

namespace semcache {
namespace {

using nlohmann::json;

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += x;
  return s;
}

// Feeds `body` to an assembler in random-sized pieces, so events split across reads.
std::optional<json> assemble(const std::string& body, std::uint32_t seed = 1, std::size_t max = 1 << 20) {
  StreamAssembler a(max);
  std::mt19937 rng(seed);
  std::size_t pos = 0;
  while (pos < body.size()) {
    auto n = std::min<std::size_t>(body.size() - pos, 1 + rng() % 40);
    a.feed(std::string_view(body).substr(pos, n));
    pos += n;
  }
  return a.completion();
}

json mock_request() {
  return json::parse(R"({"model":"m","messages":[{"role":"user","content":"What is the refund policy?"}]})");
}

TEST(StreamAssembler, ReassemblesMockStreamToTheNonStreamingShape) {
  const auto c = mock::make_completion(mock_request(), 30);
  const auto expected = json::parse(mock::completion_json(c));
  for (std::uint32_t seed = 1; seed <= 20; ++seed) {
    auto got = assemble(join(mock::stream_events(c, /*include_usage=*/true)), seed);
    ASSERT_TRUE(got) << "seed " << seed;
    EXPECT_EQ(*got, expected) << "seed " << seed;
  }
}

TEST(StreamAssembler, WithoutUsageChunkUsageIsAbsent) {
  const auto c = mock::make_completion(mock_request(), 5);
  auto got = assemble(join(mock::stream_events(c, false)));
  ASSERT_TRUE(got);
  EXPECT_FALSE(got->contains("usage"));
  EXPECT_EQ((*got)["choices"][0]["message"]["content"], c.content());
}

TEST(StreamAssembler, MergesToolCallDeltasByIndex) {
  std::string body;
  auto ev = [&](json delta, json finish = nullptr) {
    body += "data: " +
            json{{"id", "x"}, {"created", 1}, {"model", "m"},
                 {"choices", json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}}
                .dump() +
            "\r\n\r\n";
  };
  ev({{"role", "assistant"}, {"content", nullptr}});
  ev({{"tool_calls", json::array({{{"index", 0}, {"id", "call_a"}, {"type", "function"},
                                   {"function", {{"name", "get_weather"}, {"arguments", ""}}}}})}});
  ev({{"tool_calls", json::array({{{"index", 0}, {"function", {{"arguments", "{\"city\":"}}}}})}});
  ev({{"tool_calls", json::array({{{"index", 1}, {"id", "call_b"}, {"function", {{"name", "f2"}, {"arguments", "{}"}}}}})}});
  ev({{"tool_calls", json::array({{{"index", 0}, {"function", {{"arguments", "\"Paris\"}"}}}}})}});
  ev(json::object(), "tool_calls");
  body += "data: [DONE]\r\n\r\n";
  auto got = assemble(body);
  ASSERT_TRUE(got);
  const auto& msg = (*got)["choices"][0]["message"];
  EXPECT_TRUE(msg["content"].is_null());
  ASSERT_EQ(msg["tool_calls"].size(), 2u);
  EXPECT_EQ(msg["tool_calls"][0]["id"], "call_a");
  EXPECT_EQ(msg["tool_calls"][0]["function"]["name"], "get_weather");
  EXPECT_EQ(msg["tool_calls"][0]["function"]["arguments"], "{\"city\":\"Paris\"}");
  EXPECT_EQ(msg["tool_calls"][1]["function"]["name"], "f2");
  EXPECT_EQ((*got)["choices"][0]["finish_reason"], "tool_calls");
}

TEST(StreamAssembler, IncompleteOrErroredStreamsAreNotStorable) {
  const auto c = mock::make_completion(mock_request(), 5);
  auto events = mock::stream_events(c, false);
  auto truncated = events;
  truncated.pop_back();  // no [DONE]
  EXPECT_FALSE(assemble(join(truncated)));

  auto with_error = events;
  with_error.insert(with_error.end() - 1, "data: {\"error\":{\"message\":\"upstream stream interrupted\"}}\n\n");
  EXPECT_FALSE(assemble(join(with_error)));

  EXPECT_FALSE(assemble("data: not json\n\ndata: [DONE]\n\n"));
}

TEST(StreamAssembler, OverflowStopsBuffering) {
  const auto c = mock::make_completion(mock_request(), 200);
  StreamAssembler a(100);
  a.feed(join(mock::stream_events(c, false)));
  EXPECT_TRUE(a.overflowed());
  EXPECT_FALSE(a.completion());
}

TEST(MakeStorable, AppliesFinishReasonAndSizeRules) {
  CacheConfig cfg;
  std::string why;
  const auto c = json::parse(mock::completion_json(mock::make_completion(mock_request(), 5)));
  auto ok = make_storable(c, cfg, why);
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->total_tokens, c["usage"]["total_tokens"].get<std::int64_t>());

  auto truncated = c;
  truncated["choices"][0]["finish_reason"] = "length";
  EXPECT_FALSE(make_storable(truncated, cfg, why));
  EXPECT_EQ(why, "finish_reason");

  CacheConfig small;
  small.max_response_bytes = 50;
  EXPECT_FALSE(make_storable(c, small, why));
  EXPECT_EQ(why, "too_large");

  auto two = c;
  two["choices"].push_back(c["choices"][0]);
  EXPECT_FALSE(make_storable(two, cfg, why));
}

TEST(HitRendering, BodyGetsFreshIdAndCreatedButKeepsUsage) {
  const auto stored = mock::completion_json(mock::make_completion(mock_request(), 5));
  auto j = json::parse(hit_body(stored, "chatcmpl-new", 123));
  EXPECT_EQ(j["id"], "chatcmpl-new");
  EXPECT_EQ(j["created"], 123);
  EXPECT_EQ(j["usage"], json::parse(stored)["usage"]);
}

// Property: re-streaming a stored completion and reassembling it gives the same completion.
TEST(HitRendering, RestreamRoundTripsThroughTheAssembler) {
  std::vector<json> cases;
  cases.push_back(json::parse(mock::completion_json(mock::make_completion(mock_request(), 40))));
  auto unicode = cases[0];
  unicode["choices"][0]["message"]["content"] = "naïve café 你好世界你好世界你好世界你好世界 😀😀😀 end";
  cases.push_back(unicode);
  auto tools = cases[0];
  tools["choices"][0]["message"] = {{"role", "assistant"}, {"content", nullptr},
                                    {"tool_calls", json::array({{{"id", "c1"}, {"type", "function"},
                                                                 {"function", {{"name", "f"}, {"arguments", "{\"a\":1}"}}}}})}};
  tools["choices"][0]["finish_reason"] = "tool_calls";
  cases.push_back(tools);

  for (const auto& stored : cases) {
    for (std::size_t chunk : {1u, 4u, 16u, 1000u}) {
      auto events = hit_stream_events(stored.dump(), "id-x", 7, chunk, /*include_usage=*/true);
      EXPECT_EQ(events.back(), "data: [DONE]\n\n");
      auto back = assemble(join(events));
      ASSERT_TRUE(back) << chunk;
      auto expected = stored;
      expected["id"] = "id-x";
      expected["created"] = 7;
      EXPECT_EQ(*back, expected) << "chunk_chars=" << chunk;
    }
  }
}

TEST(HitRendering, ChunksNeverSplitUtf8AndUsageOnlyWhenAsked) {
  auto stored = json::parse(mock::completion_json(mock::make_completion(mock_request(), 5)));
  stored["choices"][0]["message"]["content"] = std::string(200, 'a') + "你好你好你好你好你好你好你好你好";
  for (const auto& e : hit_stream_events(stored.dump(), "i", 1, 3, false)) {
    ASSERT_TRUE(e.starts_with("data: "));
    if (e != "data: [DONE]\n\n") {
      json parsed;
      EXPECT_NO_THROW(parsed = json::parse(e.substr(6))) << "each event is valid JSON (so valid UTF-8)";
      EXPECT_EQ(parsed.count("usage"), 0u);
    }
  }
}

}  // namespace
}  // namespace semcache
