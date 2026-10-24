// Phase 8: the dashboard page and its assets, /stats and /stats/stream, and demo-mode safety
// (per-address limit, prompt cap, reset). The JavaScript itself is exercised by tools/dashboard_e2e.py.

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <filesystem>
#include <fstream>

#include "support/test_env.hpp"

namespace semcache::test {
namespace {

using nlohmann::json;

constexpr std::string_view kDemoKey = "sk-sc-demo-test";

ClientResponse get(TestEnv& env, const std::string& target) {
  return env.request({.method = http::verb::get, .target = target, .authorization = ""});
}

ClientResponse demo_chat(TestEnv& env, const std::string& prompt) {
  return env.request({.body = chat_body(prompt), .authorization = "Bearer " + std::string(kDemoKey)});
}

// Demo mode against the test's mock. finalize_config (run by TestEnv) checks the upstream against
// the allow-list and adds the "demo" tenant for the public key.
std::function<void(Config&)> demo(std::function<void(Config&)> more = {}) {
  return [more](Config& c) {
    c.mode = Mode::demo;
    c.demo.public_key = std::string(kDemoKey);
    c.demo.allowed_upstreams = {c.upstream.base_url.substr(std::string_view("http://").size())};
    if (more) more(c);
  };
}

// Blocking reader for an event stream: reads until `events` frames arrived or a read times out.
struct SseClient {
  net::io_context ioc;
  tcp::socket sock{ioc};
  std::string head;  // status line + headers
  std::vector<json> frames;

  explicit SseClient(std::uint16_t port) {
    sock.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    timeval tv{5, 0};
    ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const std::string req = "GET /stats/stream HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    net::write(sock, net::buffer(req));
  }
  void read_events(std::size_t events) {
    std::string buf;
    char tmp[4096];
    while (frames.size() < events) {
      boost::system::error_code ec;
      const auto n = sock.read_some(net::buffer(tmp), ec);
      if (ec) return;
      buf.append(tmp, n);
      if (head.empty()) {
        const auto end = buf.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        head = buf.substr(0, end);
        buf.erase(0, end + 4);
      }
      // Chunked transfer encoding wraps the events; frames are found by their "data: " prefix.
      for (std::size_t at; (at = buf.find("data: ")) != std::string::npos;) {
        const auto end = buf.find("\n\n", at);
        if (end == std::string::npos) break;
        frames.push_back(json::parse(buf.substr(at + 6, end - at - 6)));
        buf.erase(0, end + 2);
      }
    }
  }
};

TEST(Dashboard, PageIsServedWithAStrictPolicy) {
  TestEnv env;
  for (const char* target : {"/dashboard", "/dashboard/", "/dashboard/index.html"}) {
    auto r = get(env, target);
    ASSERT_EQ(r.status, 200u) << target;
    EXPECT_EQ(r.header("content-type"), "text/html; charset=utf-8");
    EXPECT_NE(r.header("content-security-policy").find("script-src 'self'"), std::string::npos);
    EXPECT_NE(r.header("content-security-policy").find("frame-ancestors 'none'"), std::string::npos);
    EXPECT_EQ(r.header("x-content-type-options"), "nosniff");
    EXPECT_NE(r.body.find("/dashboard/app.js"), std::string::npos);
  }
  EXPECT_EQ(get(env, "/dashboard/app.js").header("content-type"), "text/javascript; charset=utf-8");
  EXPECT_EQ(get(env, "/dashboard/app.css").header("content-type"), "text/css; charset=utf-8");
}

TEST(Dashboard, OnlyKnownBareFileNamesAreServed) {
  TestEnv env;
  for (const char* target : {"/dashboard/missing.js", "/dashboard/../CMakeLists.txt", "/dashboard/.git",
                             "/dashboard/sub/app.js", "/dashboard/%2e%2e%2fsecret"}) {
    EXPECT_EQ(get(env, target).status, 404u) << target;
  }
  auto post = env.request({.method = http::verb::post, .target = "/dashboard", .authorization = ""});
  EXPECT_EQ(post.status, 404u);
}

TEST(Dashboard, DirectoryOverrideServesFilesFromDisk) {
  const auto dir = std::filesystem::temp_directory_path() / "semcache_dashboard_test";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "index.html") << "<p>dev copy</p>";
  TestEnv env(fast_mock(), [&](Config& c) { c.server.dashboard_dir = dir.string(); });
  EXPECT_EQ(get(env, "/dashboard").body, "<p>dev copy</p>");
  EXPECT_EQ(get(env, "/dashboard/app.js").status, 404u);
  std::filesystem::remove_all(dir);
}

TEST(Dashboard, StatsListRecentRequestsWithoutPromptText) {
  TestEnv env(fast_mock(), [](Config& c) { c.stats.recent_requests = 3; });
  env.chat("What is your refund policy?");
  env.chat("What is your refund policy?");
  env.chat("Where is my parcel?");
  env.chat("Where is my parcel?");
  auto s = get(env, "/stats");
  ASSERT_EQ(s.status, 200u);
  auto j = json::parse(s.body);
  EXPECT_EQ(j["mode"], "live");
  EXPECT_FALSE(j.contains("demo")) << "no public key outside demo mode";
  EXPECT_GE(j["uptime_s"].get<double>(), 0.0);
  ASSERT_EQ(j["recent"].size(), 3u) << "bounded by stats.recent_requests";
  EXPECT_EQ(j["recent"][0]["result"], "HIT") << "newest first";
  EXPECT_EQ(j["recent"][1]["result"], "MISS");
  EXPECT_EQ(j["recent"][0]["status"], 200);
  EXPECT_EQ(j["recent"][0]["similarity"], 1.0);
  EXPECT_GE(j["recent"][0]["at_ms"].get<std::int64_t>(), j["recent"][2]["at_ms"].get<std::int64_t>());
  EXPECT_EQ(s.body.find("refund"), std::string::npos);
  EXPECT_EQ(s.body.find("parcel"), std::string::npos);
  EXPECT_EQ(s.body.find("\"test\""), std::string::npos) << "no tenant ids either";
}

TEST(Dashboard, StatsStreamSendsPeriodicSnapshots) {
  TestEnv env(fast_mock(), [](Config& c) { c.stats.stream_interval = std::chrono::milliseconds(50); });
  SseClient sse(env.port());
  sse.read_events(1);
  env.chat("hello");
  sse.read_events(6);
  ASSERT_GE(sse.frames.size(), 6u) << sse.head;
  EXPECT_NE(sse.head.find("text/event-stream"), std::string::npos);
  EXPECT_NE(sse.head.find("no-cache"), std::string::npos);
  EXPECT_EQ(sse.frames.front()["requests_total"], 0);
  EXPECT_EQ(sse.frames.back()["requests_total"], 1) << "later frames see the request";
}

TEST(Dashboard, StatsStreamClientsAreBounded) {
  TestEnv env(fast_mock(), [](Config& c) {
    c.stats.max_stream_clients = 1;
    c.stats.stream_interval = std::chrono::milliseconds(50);
  });
  {
    SseClient first(env.port());
    first.read_events(1);
    ASSERT_EQ(first.frames.size(), 1u);
    auto second = get(env, "/stats/stream");
    EXPECT_EQ(second.status, 503u);
    EXPECT_FALSE(second.header("retry-after").empty());
  }
  // The slot is released once the server notices the first client left (its next write fails).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    SseClient again(env.port());
    again.read_events(1);
    if (again.frames.size() == 1) break;
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "slot never released";
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

TEST(Demo, PublicKeyWorksAndStatsPublishIt) {
  TestEnv env(fast_mock(), demo());
  auto r = demo_chat(env, "What is your refund policy?");
  EXPECT_EQ(r.status, 200u) << r.body;
  EXPECT_EQ(r.header("x-semcache"), "MISS");
  EXPECT_EQ(demo_chat(env, "What is your refund policy?").header("x-semcache"), "HIT");
  auto j = json::parse(get(env, "/stats").body);
  EXPECT_EQ(j["mode"], "demo");
  EXPECT_EQ(j["demo"]["public_key"], kDemoKey);
  EXPECT_EQ(j["demo"]["max_prompt_chars"], 500);
}

TEST(Demo, EachAddressIsRateLimited) {
  TestEnv env(fast_mock(), demo([](Config& c) {
    c.demo.per_ip_rps = 0.001;
    c.demo.per_ip_burst = 3;
  }));
  for (int i = 0; i < 3; ++i) EXPECT_EQ(demo_chat(env, "q" + std::to_string(i)).status, 200u);
  auto limited = demo_chat(env, "one more");
  EXPECT_EQ(limited.status, 429u);
  EXPECT_FALSE(limited.header("retry-after").empty());
  EXPECT_NE(limited.body.find("this address"), std::string::npos);
  EXPECT_EQ(env.chat("other tenants share the address limit").status, 429u);
  EXPECT_EQ(env.mock().chat_requests(), 3u);
  EXPECT_EQ(get(env, "/stats").status, 200u) << "read-only endpoints are not limited";
}

TEST(Demo, LongPromptsAreRefusedBeforeTheCacheOrUpstream) {
  TestEnv env(fast_mock(), demo([](Config& c) { c.demo.max_prompt_chars = 5; }));
  EXPECT_EQ(demo_chat(env, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9").status, 200u) << "5 code points, 10 bytes";
  auto r = demo_chat(env, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");
  EXPECT_EQ(r.status, 400u);
  EXPECT_NE(r.body.find("prompt_too_long"), std::string::npos);
  EXPECT_EQ(r.header("x-semcache"), "BYPASS");
  EXPECT_EQ(env.mock().chat_requests(), 1u);
}

TEST(Demo, ResetPurgesOnlyTheDemoTenant) {
  TestEnv env(fast_mock(), demo());
  demo_chat(env, "What is your refund policy?");
  demo_chat(env, "Where is my parcel?");
  env.chat("What is your refund policy?");  // tenant "test"
  ASSERT_EQ(env.store().stats().entries, 3u);
  auto r = env.request({.method = http::verb::post, .target = "/demo/reset", .authorization = ""});
  ASSERT_EQ(r.status, 200u) << r.body;
  EXPECT_EQ(json::parse(r.body)["purged"], 2);
  EXPECT_EQ(env.store().stats().entries, 1u);
  EXPECT_EQ(demo_chat(env, "What is your refund policy?").header("x-semcache"), "MISS");
  EXPECT_EQ(env.chat("What is your refund policy?").header("x-semcache"), "HIT");
}

TEST(Demo, DemoEndpointsDoNotExistInLiveMode) {
  TestEnv env;
  auto r = env.request({.method = http::verb::post, .target = "/demo/reset", .authorization = ""});
  EXPECT_EQ(r.status, 404u);
}

}  // namespace
}  // namespace semcache::test
