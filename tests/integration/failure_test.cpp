// Upstream failure modes (design §8). Faults are injected through the mock's "mock_fault" field.

#include <gtest/gtest.h>

#include "support/test_env.hpp"

namespace semcache::test {
namespace {

nlohmann::json fault(const std::string& spec) { return {{"mock_fault", spec}}; }

std::string error_code(const ClientResponse& r) {
  auto j = nlohmann::json::parse(r.body, nullptr, false);
  return j.is_object() ? j["error"].value("code", std::string{}) : std::string{};
}

TEST(Failure, Upstream5xxIsPassedThrough) {
  TestEnv env;
  auto r = chat(env.port(), "q", false, fault("status=503"));
  EXPECT_EQ(r.status, 503u);
  EXPECT_EQ(nlohmann::json::parse(r.body)["error"]["message"], "injected fault");
  EXPECT_EQ(r.header("x-semcache"), "MISS");
}

TEST(Failure, UnreachableUpstreamIs502) {
  TestEnv env(fast_mock(), [](Config& c) { c.upstream.base_url = "http://127.0.0.1:" + std::to_string(closed_port()); });
  auto r = chat(env.port(), "q");
  EXPECT_EQ(r.status, 502u);
  EXPECT_EQ(error_code(r), "upstream_unavailable");
  EXPECT_EQ(r.header("x-semcache"), "MISS");
}

TEST(Failure, FirstByteTimeoutIs504) {
  TestEnv env(fast_mock(), [](Config& c) { c.upstream.first_byte_timeout = std::chrono::milliseconds(200); });
  auto r = chat(env.port(), "q", false, fault("hang"));
  EXPECT_EQ(r.status, 504u);
  EXPECT_EQ(error_code(r), "upstream_timeout");
  EXPECT_LT(r.total_ms, 2000);
}

TEST(Failure, TotalTimeoutBoundsSlowStreams) {
  auto m = fast_mock();
  m.token_delay = std::chrono::milliseconds(100);
  TestEnv env(m, [](Config& c) { c.upstream.total_timeout = std::chrono::milliseconds(300); });
  auto r = chat(env.port(), "q", true);
  ASSERT_EQ(r.status, 200u);
  auto events = sse_events(r.body);
  ASSERT_FALSE(events.empty());
  EXPECT_NE(events.back(), "[DONE]");
  ASSERT_TRUE(events.back().contains("error"));
  EXPECT_NE(events.back()["error"]["message"].get<std::string>().find("timeout"), std::string::npos);
  EXPECT_LT(r.total_ms, 2000);
}

TEST(Failure, IdleTimeoutMidStream) {
  auto m = fast_mock();
  m.token_delay = std::chrono::milliseconds(500);
  TestEnv env(m, [](Config& c) { c.upstream.idle_timeout = std::chrono::milliseconds(100); });
  auto r = chat(env.port(), "q", true);
  ASSERT_EQ(r.status, 200u);
  auto events = sse_events(r.body);
  ASSERT_FALSE(events.empty());
  EXPECT_TRUE(events.back().contains("error"));
}

TEST(Failure, UpstreamResetMidStreamEndsStreamWithErrorEvent) {
  TestEnv env;
  auto r = chat(env.port(), "q", true, fault("reset-after=3"));
  ASSERT_EQ(r.status, 200u);
  EXPECT_TRUE(r.error.empty()) << "proxy must terminate the chunked stream cleanly: " << r.error;
  auto events = sse_events(r.body);
  ASSERT_GE(events.size(), 2u);
  EXPECT_TRUE(mock::make_completion(nlohmann::json::parse(chat_body("q", true)), 12).content().starts_with(
      assembled_content(events)));
  ASSERT_TRUE(events.back().contains("error"));
  EXPECT_NE(events.back()["error"]["message"].get<std::string>().find("reset"), std::string::npos);
}

TEST(Failure, UpstreamResetBeforeResponseIs502) {
  TestEnv env;
  auto r = chat(env.port(), "q", false, fault("reset-after=0"));
  EXPECT_EQ(r.status, 502u);
  EXPECT_EQ(error_code(r), "upstream_unavailable");
}

TEST(Failure, OversizedUpstreamResponseIs502) {
  TestEnv env(fast_mock(), [](Config& c) { c.upstream.max_response_bytes = 100; });
  auto r = chat(env.port(), "q");
  EXPECT_EQ(r.status, 502u);
  EXPECT_EQ(error_code(r), "upstream_response_too_large");
}

TEST(Failure, OverloadReturns503WithRetryAfter) {
  auto m = fast_mock();
  m.latency = std::chrono::milliseconds(300);
  TestEnv env(m, [](Config& c) { c.server.max_inflight = 2; });
  std::vector<ClientRequest> reqs;
  for (int i = 0; i < 6; ++i) reqs.push_back({.body = chat_body("q" + std::to_string(i))});
  auto rs = request_many(env.port(), reqs);
  int ok = 0, overloaded = 0;
  for (const auto& r : rs) {
    if (r.status == 200) ++ok;
    if (r.status == 503) {
      ++overloaded;
      EXPECT_EQ(r.header("retry-after"), "1");
      EXPECT_EQ(error_code(r), "overloaded");
    }
  }
  EXPECT_EQ(ok, 2);
  EXPECT_EQ(overloaded, 4);
}

TEST(Failure, ShutdownCancelsHangingUpstreamCall) {
  // The TestEnv destructor must drain even while a request waits on a hung upstream (the mock
  // hangs for an hour; timeouts are far longer than the 5 s drain budget).
  auto env = std::make_unique<TestEnv>();
  std::thread client([port = env->port()] { (void)chat(port, "q", false, fault("hang")); });
  while (env->mock().chat_requests() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const auto start = std::chrono::steady_clock::now();
  env.reset();  // EXPECTs drained inside
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  client.join();
}

}  // namespace
}  // namespace semcache::test
