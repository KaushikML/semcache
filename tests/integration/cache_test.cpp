// Phase 3: the cache in the request path. Uses the deterministic FakeEmbedder so similarities are
// exact; tests/integration/semantic_model_test.cpp covers the real model.

#include <gtest/gtest.h>

#include <future>

#include "support/test_env.hpp"

namespace semcache::test {
namespace {

using nlohmann::json;

std::string content_of(const ClientResponse& r) {
  return json::parse(r.body)["choices"][0]["message"]["content"].get<std::string>();
}

bool has_metric(const std::string& metrics, const std::string& line) {
  return metrics.find(line + "\n") != std::string::npos;
}

TEST(Cache, MissThenExactHitWithoutCallingUpstream) {
  TestEnv env;
  auto miss = env.chat("What is your refund policy?");
  ASSERT_EQ(miss.status, 200u) << miss.body;
  EXPECT_EQ(miss.header("x-semcache"), "MISS");
  EXPECT_TRUE(miss.header("x-semcache-similarity").empty()) << "empty partition: nothing to compare";

  auto hit = env.chat("What is your refund policy?");
  ASSERT_EQ(hit.status, 200u) << hit.body;
  EXPECT_EQ(hit.header("x-semcache"), "HIT");
  EXPECT_EQ(hit.header("x-semcache-similarity"), "1.0000");
  EXPECT_EQ(content_of(hit), content_of(miss));
  EXPECT_NE(json::parse(hit.body)["id"], json::parse(miss.body)["id"]) << "fresh id per response";
  EXPECT_EQ(json::parse(hit.body)["usage"], json::parse(miss.body)["usage"]) << "original usage preserved";
  EXPECT_EQ(env.mock().chat_requests(), 1u);
  EXPECT_EQ(env.fake_embedder().calls(), 1) << "exact fast path skips embedding";
}

TEST(Cache, ParaphraseAboveThresholdIsASemanticHit) {
  TestEnv env;  // threshold 0.90
  env.fake_embedder().alias("How do I get my money back?", "What is your refund policy?", 0.95f);
  auto first = env.chat("What is your refund policy?");
  auto para = env.chat("How do I get my money back?");
  ASSERT_EQ(para.status, 200u);
  EXPECT_EQ(para.header("x-semcache"), "HIT");
  EXPECT_EQ(para.header("x-semcache-similarity"), "0.9500");
  EXPECT_EQ(content_of(para), content_of(first)) << "the cached answer, not a fresh mock answer";
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

TEST(Cache, BelowThresholdIsAMissThatReportsTheBestScore) {
  TestEnv env;
  env.fake_embedder().alias("Can I return a gift?", "What is your refund policy?", 0.85f);
  env.chat("What is your refund policy?");
  auto r = env.chat("Can I return a gift?");
  EXPECT_EQ(r.header("x-semcache"), "MISS");
  EXPECT_EQ(r.header("x-semcache-similarity"), "0.8500");
  EXPECT_EQ(env.mock().chat_requests(), 2u);
  EXPECT_EQ(env.chat("Can I return a gift?").header("x-semcache"), "HIT") << "the miss was stored";
}

TEST(Cache, ThresholdIsConfigurable) {
  TestEnv env(fast_mock(), [](Config& c) { c.cache.similarity_threshold = 0.80f; });
  env.fake_embedder().alias("Can I return a gift?", "What is your refund policy?", 0.85f);
  env.chat("What is your refund policy?");
  EXPECT_EQ(env.chat("Can I return a gift?").header("x-semcache"), "HIT");
}

TEST(Cache, StreamedMissIsStoredAndServedToBothModes) {
  TestEnv env;
  auto miss = env.chat("Explain TCP", /*stream=*/true, {{"stream_options", {{"include_usage", true}}}});
  ASSERT_EQ(miss.status, 200u);
  EXPECT_EQ(miss.header("x-semcache"), "MISS");
  const auto expected = assembled_content(sse_events(miss.body));

  auto plain = env.chat("Explain TCP");
  EXPECT_EQ(plain.header("x-semcache"), "HIT");
  EXPECT_EQ(content_of(plain), expected);

  auto streamed = env.chat("Explain TCP", true, {{"stream_options", {{"include_usage", true}}}});
  EXPECT_EQ(streamed.header("x-semcache"), "HIT");
  EXPECT_EQ(streamed.header("content-type"), "text/event-stream");
  auto events = sse_events(streamed.body);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back(), "[DONE]");
  EXPECT_EQ(assembled_content(events), expected);
  EXPECT_GT(events.size(), 4u) << "re-streamed in several chunks";
  EXPECT_TRUE(events[events.size() - 2].contains("usage")) << "usage chunk when include_usage is set";
  EXPECT_NE(streamed.body.find(": semcache-total-ms="), std::string::npos);
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

TEST(Cache, TenantsNeverShareEntries) {
  TestEnv env;
  env.chat("What is your refund policy?");
  auto other = env.request({.body = chat_body("What is your refund policy?"),
                                    .authorization = "Bearer " + std::string(kOtherKey)});
  EXPECT_EQ(other.header("x-semcache"), "MISS");
  EXPECT_EQ(env.mock().chat_requests(), 2u);
}

TEST(Cache, DifferentContextMisses) {
  TestEnv env;
  env.chat("What is your refund policy?");
  EXPECT_EQ(env.chat("What is your refund policy?", false, {{"temperature", 0.2}}).header("x-semcache"), "MISS");
  json with_system = json::parse(chat_body("What is your refund policy?"));
  with_system["messages"].insert(with_system["messages"].begin(), {{"role", "system"}, {"content", "Be terse."}});
  EXPECT_EQ(env.request({.body = with_system.dump()}).header("x-semcache"), "MISS");
}

TEST(Cache, BypassHeaderSkipsLookupAndStore) {
  TestEnv env;
  ClientRequest r{.body = chat_body("hello"), .headers = {{"X-Semcache-Bypass", "1"}}};
  EXPECT_EQ(env.request(r).header("x-semcache"), "BYPASS");
  EXPECT_EQ(env.chat("hello").header("x-semcache"), "MISS") << "the bypassed response was not stored";
  EXPECT_TRUE(has_metric(env.metrics(), R"(semcache_bypass_total{reason="bypass_header"} 1)"));
}

TEST(Cache, UncacheableRequestsBypass) {
  TestEnv env;
  EXPECT_EQ(env.chat("hello", false, {{"n", 2}}).header("x-semcache"), "BYPASS");
  EXPECT_EQ(env.chat("hello", false, {{"n", 2}}).header("x-semcache"), "BYPASS");
  EXPECT_EQ(env.mock().chat_requests(), 2u);
  EXPECT_TRUE(has_metric(env.metrics(), R"(semcache_bypass_total{reason="multiple_choices"} 2)"));
}

TEST(Cache, CacheDisabledBypassesEverything) {
  TestEnv env(fast_mock(), [](Config& c) { c.cache.enabled = false; });
  env.chat("hello");
  EXPECT_EQ(env.chat("hello").header("x-semcache"), "BYPASS");
}

TEST(Cache, UpstreamErrorsAreNeverStored) {
  TestEnv env;
  const json fault = {{"mock_fault", "status=503"}};
  EXPECT_EQ(env.chat("hello", false, fault).status, 503u);
  auto again = env.chat("hello", false, fault);
  EXPECT_EQ(again.status, 503u);
  EXPECT_EQ(again.header("x-semcache"), "MISS");
  EXPECT_TRUE(has_metric(env.metrics(), R"(semcache_store_skipped_total{reason="status"} 2)"));
}

TEST(Cache, InterruptedStreamsAreNeverStored) {
  TestEnv env;
  const json fault = {{"mock_fault", "reset-after=3"}};
  env.chat("hello", true, fault);
  EXPECT_EQ(env.chat("hello", true, fault).header("x-semcache"), "MISS");
  EXPECT_EQ(env.store().stats().entries, 0u);
}

TEST(Cache, EmbedderFailureFailsOpen) {
  TestEnv env;
  env.fake_embedder().fail_with(EmbedError::inference);
  auto r = env.chat("hello");
  EXPECT_EQ(r.status, 200u);
  EXPECT_EQ(r.header("x-semcache"), "BYPASS");
  EXPECT_EQ(env.store().stats().entries, 0u);
  EXPECT_TRUE(has_metric(env.metrics(), "semcache_embed_errors_total 1"));
}

TEST(Cache, TooLongPromptIsCachedForExactMatchOnly) {
  TestEnv env;
  env.fake_embedder().fail_with(EmbedError::too_long);
  EXPECT_EQ(env.chat("a very long prompt").header("x-semcache"), "MISS");
  EXPECT_EQ(env.chat("a very long prompt").header("x-semcache"), "HIT");
}

TEST(Cache, WithoutEmbedderExactMatchStillWorks) {
  TestEnv env(fast_mock(), {}, WithEmbedder::none);
  EXPECT_EQ(env.chat("hello").header("x-semcache"), "MISS");
  EXPECT_EQ(env.chat("hello").header("x-semcache"), "HIT");
  EXPECT_EQ(env.chat("hello there").header("x-semcache"), "MISS");
}

TEST(Cache, FullEmbedQueueFailsOpen) {
  TestEnv env(fast_mock(), [](Config& c) {
    c.embedder.threads = 1;
    c.embedder.queue_capacity = 1;
  });
  env.fake_embedder().set_gate(true);  // embeds block: one runs, one queues, the rest are rejected
  std::vector<ClientRequest> reqs;
  for (int i = 0; i < 4; ++i) reqs.push_back({.body = chat_body("prompt " + std::to_string(i))});
  auto pending = std::async(std::launch::async, [&] { return request_many(env.port(), reqs); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (env.metrics().find("semcache_embed_queue_rejected_total 0\n") != std::string::npos &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  env.fake_embedder().set_gate(false);
  int bypass = 0;
  for (const auto& r : pending.get()) {
    EXPECT_EQ(r.status, 200u) << "every request is served";
    bypass += r.header("x-semcache") == "BYPASS";
  }
  EXPECT_GE(bypass, 1);
  EXPECT_EQ(env.mock().chat_requests(), 4u);
}

TEST(Cache, StatsAndMetricsReflectHits) {
  TestEnv env;
  env.chat("What is your refund policy?");
  env.chat("What is your refund policy?");
  auto s = env.request({.method = http::verb::get, .target = "/stats", .authorization = ""});
  ASSERT_EQ(s.status, 200u);
  auto j = json::parse(s.body);
  EXPECT_EQ(j["requests"]["HIT"], 1);
  EXPECT_EQ(j["requests"]["MISS"], 1);
  EXPECT_DOUBLE_EQ(j["hit_rate"].get<double>(), 0.5);
  EXPECT_EQ(j["cache"]["entries"], 1);
  EXPECT_GT(j["tokens_saved"].get<int>(), 0);
  EXPECT_EQ(s.body.find("refund"), std::string::npos) << "/stats never contains prompt text";

  const auto m = env.metrics();
  EXPECT_TRUE(has_metric(m, "semcache_index_entries 1"));
  EXPECT_TRUE(has_metric(m, "semcache_partitions 1"));
  EXPECT_TRUE(has_metric(m, R"(semcache_requests_total{result="HIT"} 1)"));
}

}  // namespace
}  // namespace semcache::test
