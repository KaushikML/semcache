// Phase 1: proxy forwards to upstream unchanged; every response is X-Semcache: BYPASS.

#include <gtest/gtest.h>

#include "support/test_env.hpp"

namespace semcache::test {
namespace {

TEST(Passthrough, Healthz) {
  TestEnv env;
  auto r = request(env.port(), {.method = http::verb::get, .target = "/healthz", .authorization = ""});
  EXPECT_EQ(r.status, 200u);
  EXPECT_EQ(r.body, R"({"status":"ok"})");
}

TEST(Passthrough, ReadyzListsNoDegradedComponents) {
  TestEnv env;
  auto r = request(env.port(), {.method = http::verb::get, .target = "/readyz", .authorization = ""});
  EXPECT_EQ(r.status, 200u);
  auto j = nlohmann::json::parse(r.body);
  EXPECT_EQ(j["status"], "ready");
  EXPECT_TRUE(j["degraded"].empty());
}

TEST(Passthrough, MissingEmbedderIsDegradedButStillServes) {
  TestEnv env(fast_mock(), {}, WithEmbedder::none);
  auto r = request(env.port(), {.method = http::verb::get, .target = "/readyz", .authorization = ""});
  EXPECT_EQ(r.status, 200u) << "degraded still takes traffic: requests pass through (fail open)";
  auto j = nlohmann::json::parse(r.body);
  EXPECT_EQ(j["status"], "degraded");
  EXPECT_EQ(j["degraded"], nlohmann::json::array({"embedder"}));

  auto chat_r = chat(env.port(), "hi");
  EXPECT_EQ(chat_r.status, 200u);
  EXPECT_EQ(chat_r.header("x-semcache"), "BYPASS");

  auto m = request(env.port(), {.method = http::verb::get, .target = "/metrics", .authorization = ""});
  EXPECT_NE(m.body.find("semcache_embedder_ready 0"), std::string::npos);
}

TEST(Passthrough, NonStreamingBodyIsRelayedVerbatim) {
  TestEnv env;
  const auto body = chat_body("What is the refund policy?");
  auto r = request(env.port(), {.body = body});
  ASSERT_EQ(r.status, 200u) << r.body;
  const auto expected = mock::completion_json(mock::make_completion(nlohmann::json::parse(body), 12));
  EXPECT_EQ(r.body, expected);
  EXPECT_EQ(r.header("content-type"), "application/json");
  EXPECT_EQ(r.header("x-semcache"), "BYPASS");
  EXPECT_FALSE(r.header("x-semcache-latency-ms").empty());
  EXPECT_TRUE(r.header("x-semcache-similarity").empty()) << "no similarity without a lookup";
  EXPECT_EQ(r.header("x-request-id"), mock::make_completion(nlohmann::json::parse(body), 12).id)
      << "allow-listed upstream headers are relayed";
  EXPECT_TRUE(r.header("x-mock-cost-usd").empty()) << "non-allow-listed upstream headers are dropped";
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

TEST(Passthrough, ClientKeyIsReplacedByUpstreamKey) {
  TestEnv env(fast_mock(), [](Config& c) { c.upstream.api_key = Secret("sk-upstream-secret"); });
  auto r = chat(env.port(), "hi");
  ASSERT_EQ(r.status, 200u);
  EXPECT_EQ(env.mock().last_authorization(), "Bearer sk-upstream-secret");
}

TEST(Passthrough, NoUpstreamKeyMeansNoAuthorizationForwarded) {
  TestEnv env;
  ASSERT_EQ(chat(env.port(), "hi").status, 200u);
  EXPECT_EQ(env.mock().last_authorization(), "") << "the client's proxy key must never reach upstream";
}

TEST(Passthrough, RejectsMissingOrUnknownKey) {
  TestEnv env;
  auto missing = request(env.port(), {.body = chat_body("hi"), .authorization = ""});
  EXPECT_EQ(missing.status, 401u);
  auto unknown = request(env.port(), {.body = chat_body("hi"), .authorization = "Bearer sk-nope"});
  EXPECT_EQ(unknown.status, 401u);
  EXPECT_EQ(nlohmann::json::parse(unknown.body)["error"]["code"], "invalid_api_key");
  EXPECT_EQ(env.mock().chat_requests(), 0u);
}

TEST(Passthrough, SecondTenantIsAccepted) {
  TestEnv env;
  auto r = request(env.port(), {.body = chat_body("hi"), .authorization = "Bearer " + std::string(kOtherKey)});
  EXPECT_EQ(r.status, 200u);
}

TEST(Passthrough, InvalidJsonIs400AndNotForwarded) {
  TestEnv env;
  for (std::string body : {"not json", "[]", R"({"model":"m"})", R"({"messages":"x"})"}) {
    auto r = request(env.port(), {.body = body});
    EXPECT_EQ(r.status, 400u) << body;
    EXPECT_EQ(nlohmann::json::parse(r.body)["error"]["type"], "invalid_request_error");
  }
  EXPECT_EQ(env.mock().chat_requests(), 0u);
}

TEST(Passthrough, OversizedBodyIs413) {
  TestEnv env(fast_mock(), [](Config& c) { c.server.max_body_bytes = 1024; });
  auto r = request(env.port(), {.body = chat_body(std::string(2000, 'x'))});
  EXPECT_EQ(r.status, 413u);
  EXPECT_EQ(env.mock().chat_requests(), 0u);
}

TEST(Passthrough, UnknownRouteAndWrongMethod) {
  TestEnv env;
  EXPECT_EQ(request(env.port(), {.method = http::verb::get, .target = "/nope"}).status, 404u);
  auto r = request(env.port(), {.method = http::verb::get, .target = "/v1/chat/completions"});
  EXPECT_EQ(r.status, 405u);
  EXPECT_EQ(r.header("allow"), "POST");
}

TEST(Passthrough, RateLimitPerTenant) {
  TestEnv env(fast_mock(), [](Config& c) {
    c.limits.rps = 0.01;
    c.limits.burst = 2;
  });
  EXPECT_EQ(chat(env.port(), "a").status, 200u);
  EXPECT_EQ(chat(env.port(), "b").status, 200u);
  auto limited = chat(env.port(), "c");
  EXPECT_EQ(limited.status, 429u);
  EXPECT_FALSE(limited.header("retry-after").empty());
  auto other = request(env.port(), {.body = chat_body("d"), .authorization = "Bearer " + std::string(kOtherKey)});
  EXPECT_EQ(other.status, 200u) << "limits are per tenant";
}

TEST(Passthrough, UpstreamPathPrefixIsApplied) {
  // The mock serves /v1/chat/completions only, so a prefixed path proves the prefix is sent.
  mock::MockConfig m = fast_mock();
  TestEnv env(m, [](Config& c) { c.upstream.base_url += "/gateway"; });
  auto r = chat(env.port(), "hi");
  EXPECT_EQ(r.status, 404u) << "upstream 4xx is passed through verbatim";
  EXPECT_EQ(env.mock().chat_requests(), 0u);
}

TEST(Passthrough, MetricsEndpointCountsRequests) {
  TestEnv env;
  ASSERT_EQ(chat(env.port(), "hi").status, 200u);
  ASSERT_EQ(request(env.port(), {.body = chat_body("x"), .authorization = ""}).status, 401u);
  auto m = request(env.port(), {.method = http::verb::get, .target = "/metrics", .authorization = ""});
  ASSERT_EQ(m.status, 200u);
  EXPECT_NE(m.body.find("semcache_requests_total{result=\"BYPASS\"} 1\n"), std::string::npos) << m.body;
  EXPECT_NE(m.body.find("semcache_auth_failures_total 1\n"), std::string::npos);
  EXPECT_NE(m.body.find("semcache_upstream_responses_total{class=\"2xx\"} 1\n"), std::string::npos);
  EXPECT_NE(m.body.find("semcache_upstream_seconds_count 1\n"), std::string::npos);
  EXPECT_NE(m.body.find("semcache_inflight 0\n"), std::string::npos);
}

}  // namespace
}  // namespace semcache::test
