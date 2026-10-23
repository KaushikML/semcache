// Phase 4: request coalescing under thundering herds, end to end (real sockets, mock upstream).

#include <gtest/gtest.h>

#include <set>

#include "support/test_env.hpp"

namespace semcache::test {
namespace {

using nlohmann::json;

mock::MockConfig slow_mock(int latency_ms = 300) {
  auto m = fast_mock();
  m.latency = std::chrono::milliseconds(latency_ms);
  return m;
}

std::vector<ClientRequest> copies(const ClientRequest& r, int n) { return std::vector<ClientRequest>(n, r); }

std::map<std::string, int> count_results(const std::vector<ClientResponse>& rs) {
  std::map<std::string, int> out;
  for (const auto& r : rs) out[r.header("x-semcache")]++;
  return out;
}

TEST(Herd, IdenticalRequestsMakeOneUpstreamCall) {
  TestEnv env(slow_mock());
  auto rs = request_many(env.port(), copies({.body = chat_body("What is your refund policy?")}, 50));
  auto counts = count_results(rs);
  EXPECT_EQ(counts["MISS"], 1);
  EXPECT_EQ(counts["COALESCED"], 49);
  std::set<std::string> bodies;
  for (const auto& r : rs) {
    EXPECT_EQ(r.status, 200u);
    bodies.insert(r.body);
  }
  EXPECT_EQ(bodies.size(), 1u) << "followers see exactly the leader's response";
  EXPECT_EQ(env.mock().chat_requests(), 1u);

  env.flush();
  EXPECT_EQ(env.chat("What is your refund policy?").header("x-semcache"), "HIT");
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

TEST(Herd, StreamingFollowersReceiveTokensLive) {
  auto m = fast_mock();
  m.latency = std::chrono::milliseconds(50);
  m.token_delay = std::chrono::milliseconds(20);
  m.response_words = 20;  // ~22 chunks * 20 ms = ~450 ms of streaming
  TestEnv env(m);
  auto rs = request_many(env.port(), copies({.body = chat_body("Explain TCP", /*stream=*/true)}, 20));
  const auto expected = assembled_content(sse_events(rs.front().body));
  ASSERT_FALSE(expected.empty());
  for (const auto& r : rs) {
    EXPECT_EQ(assembled_content(sse_events(r.body)), expected);
    EXPECT_EQ(sse_events(r.body).back(), "[DONE]");
    EXPECT_LT(r.header_ms, r.total_ms - 200) << "headers arrive long before the stream ends: live, not buffered";
  }
  EXPECT_EQ(count_results(rs)["COALESCED"], 19);
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

TEST(Herd, StreamedAndNonStreamedRequestsDoNotShareAFlight) {
  TestEnv env(slow_mock());
  std::vector<ClientRequest> reqs;
  for (int i = 0; i < 5; ++i) reqs.push_back({.body = chat_body("hello", true)});
  for (int i = 0; i < 5; ++i) reqs.push_back({.body = chat_body("hello", false)});
  auto rs = request_many(env.port(), reqs);
  for (const auto& r : rs) EXPECT_EQ(r.status, 200u);
  EXPECT_EQ(env.mock().chat_requests(), 2u);
  EXPECT_EQ(count_results(rs)["COALESCED"], 8);
}

TEST(Herd, LeaderErrorIsSharedWithoutRetryStorm) {
  TestEnv env(slow_mock());
  auto rs = request_many(env.port(), copies({.body = chat_body("hello", false, {{"mock_fault", "status=503"}})}, 20));
  for (const auto& r : rs) EXPECT_EQ(r.status, 503u);
  EXPECT_EQ(env.mock().chat_requests(), 1u) << "one failed upstream call, not twenty";
  EXPECT_EQ(env.store().stats().entries, 0u);
}

TEST(Herd, LeaderStreamResetEndsEveryFollowerWithAnErrorEvent) {
  auto m = slow_mock(100);
  m.token_delay = std::chrono::milliseconds(10);
  TestEnv env(m);
  auto rs = request_many(env.port(), copies({.body = chat_body("hello", true, {{"mock_fault", "reset-after=3"}})}, 10));
  for (const auto& r : rs) {
    EXPECT_NE(r.body.find("interrupted"), std::string::npos) << r.body;
    EXPECT_EQ(r.body.find("[DONE]"), std::string::npos);
  }
  EXPECT_EQ(env.mock().chat_requests(), 1u);
  EXPECT_EQ(env.store().stats().entries, 0u);
}

TEST(Herd, FollowerCapServesExtrasIndependently) {
  TestEnv env(slow_mock(), [](Config& c) { c.coalescer.max_waiters_per_flight = 2; });
  auto rs = request_many(env.port(), copies({.body = chat_body("hello")}, 6));
  for (const auto& r : rs) EXPECT_EQ(r.status, 200u);
  EXPECT_EQ(count_results(rs)["COALESCED"], 2);
  EXPECT_EQ(env.mock().chat_requests(), 4u) << "1 leader + 3 independent";
  EXPECT_NE(env.metrics().find("semcache_coalesce_independent_total 3\n"), std::string::npos);
}

TEST(Herd, CoalescingCanBeDisabled) {
  TestEnv env(slow_mock(), [](Config& c) { c.coalescer.enabled = false; });
  request_many(env.port(), copies({.body = chat_body("hello")}, 5));
  EXPECT_EQ(env.mock().chat_requests(), 5u);
}

TEST(Herd, TenantsAndUncacheableRequestsNeverCoalesce) {
  TestEnv env(slow_mock());
  std::vector<ClientRequest> reqs;
  for (int i = 0; i < 3; ++i) reqs.push_back({.body = chat_body("hello")});
  for (int i = 0; i < 3; ++i) reqs.push_back({.body = chat_body("hello"), .authorization = "Bearer " + std::string(kOtherKey)});
  for (int i = 0; i < 3; ++i) reqs.push_back({.body = chat_body("hello", false, {{"n", 2}})});
  auto rs = request_many(env.port(), reqs);
  EXPECT_EQ(env.mock().chat_requests(), 1u + 1u + 3u) << "one flight per tenant; BYPASS requests each go upstream";
  EXPECT_EQ(count_results(rs)["BYPASS"], 3);
}

TEST(Herd, LeaderSemanticHitIsSharedWithFollowers) {
  TestEnv env(slow_mock(100));
  env.fake_embedder().alias("How do I get my money back?", "What is your refund policy?", 0.985f);
  auto first = env.chat("What is your refund policy?");
  auto rs = request_many(env.port(), copies({.body = chat_body("How do I get my money back?")}, 10));
  const auto expected = json::parse(first.body)["choices"][0]["message"]["content"];
  for (const auto& r : rs) EXPECT_EQ(json::parse(r.body)["choices"][0]["message"]["content"], expected);
  auto counts = count_results(rs);
  EXPECT_EQ(counts["HIT"] + counts["COALESCED"], 10);
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

// A client that sends a streaming request, reads only the response head, then hangs up.
void request_then_disconnect(std::uint16_t port, const std::string& body) {
  net::io_context ioc;
  beast::tcp_stream s(ioc);
  s.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
  http::request<http::string_body> req{http::verb::post, "/v1/chat/completions", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::authorization, "Bearer " + std::string(kTestKey));
  req.set(http::field::content_type, "application/json");
  req.body() = body;
  req.prepare_payload();
  http::write(s, req);
  beast::flat_buffer buf;
  http::response_parser<http::empty_body> p;
  http::read_header(s, buf, p);
  beast::error_code ignored;
  s.socket().shutdown(tcp::socket::shutdown_both, ignored);
  s.close();
}

TEST(Herd, LeaderClientLeavingDoesNotCancelTheSharedCall) {
  auto m = fast_mock();
  m.latency = std::chrono::milliseconds(50);
  m.token_delay = std::chrono::milliseconds(20);
  m.response_words = 15;
  TestEnv env(m);
  const auto body = chat_body("Explain UDP", true);
  std::thread leader([&] { request_then_disconnect(env.port(), body); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the leader's request registers the flight
  auto rs = request_many(env.port(), copies({.body = body}, 5));
  leader.join();
  for (const auto& r : rs) {
    EXPECT_EQ(sse_events(r.body).back(), "[DONE]") << "followers get the full stream";
    EXPECT_EQ(r.header("x-semcache"), "COALESCED");
  }
  env.flush();
  EXPECT_EQ(env.chat("Explain UDP", true).header("x-semcache"), "HIT") << "and the response was still stored";
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

// Same, but the leader has nothing to store (embedder failure => BYPASS), so only the followers
// keep the upstream read alive once the leader's client is gone.
TEST(Herd, FollowersAloneKeepTheSharedCallAlive) {
  auto m = fast_mock();
  m.latency = std::chrono::milliseconds(50);
  m.token_delay = std::chrono::milliseconds(20);
  m.response_words = 15;
  TestEnv env(m);
  env.fake_embedder().fail_with(EmbedError::inference);
  const auto body = chat_body("Explain UDP", true);
  std::thread leader([&] { request_then_disconnect(env.port(), body); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto rs = request_many(env.port(), copies({.body = body}, 5));
  leader.join();
  for (const auto& r : rs) {
    EXPECT_EQ(r.header("x-semcache"), "COALESCED");
    EXPECT_EQ(sse_events(r.body).back(), "[DONE]") << "followers get the full stream";
  }
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

// Stress for TSan: concurrent duplicates and distinct prompts, streaming and not, with a cache
// small enough to evict constantly and a sweeper running.
TEST(Herd, ConcurrentMixedTrafficIsConsistent) {
  auto m = fast_mock();
  m.latency = std::chrono::milliseconds(5);
  TestEnv env(m, [](Config& c) {
    c.cache.max_bytes = 20 * 1024;  // a handful of entries: constant CLOCK eviction
    c.embedder.threads = 2;
  });
  std::vector<ClientRequest> reqs;
  for (int i = 0; i < 120; ++i) {
    reqs.push_back({.body = chat_body("prompt " + std::to_string(i % 15), i % 3 == 0)});
  }
  std::vector<std::thread> clients;
  std::atomic<int> bad{0};
  for (int t = 0; t < 3; ++t) {
    clients.emplace_back([&] {
      for (const auto& r : request_many(env.port(), reqs)) {
        if (r.status != 200) bad++;
      }
    });
  }
  std::atomic<bool> stop{false};
  std::thread sweeper([&] {  // TTL sweeps on the writer + gauge reads, interleaved with inserts
    while (!stop) {
      env.writer().submit_expire();
      EXPECT_LE(env.store().stats().bytes, 20u * 1024u);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  for (auto& c : clients) c.join();
  stop = true;
  sweeper.join();
  env.flush();
  EXPECT_EQ(bad.load(), 0);
  EXPECT_LE(env.store().stats().bytes, 20u * 1024u);
  EXPECT_LT(env.mock().chat_requests(), 360u) << "duplicates were coalesced or cached";
}

}  // namespace
}  // namespace semcache::test
