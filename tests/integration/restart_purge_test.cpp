// Phase 6 end to end: the cache survives restarts, and /admin/purge invalidates by tag / tenant.

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <future>

#include "support/test_env.hpp"

namespace semcache::test {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kAdminKey = "sk-admin-test";

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() / ("semcache-it-" + std::to_string(::getpid()) + "-" +
                                        ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(path);
  }
  ~TempDir() { fs::remove_all(path); }
};

std::function<void(Config&)> with_persistence(const TempDir& d) {
  return [dir = d.path.string()](Config& c) {
    c.persistence.enabled = true;
    c.persistence.dir = dir;
    c.admin_key = Secret(std::string(kAdminKey));
  };
}

ClientResponse purge(TestEnv& env, const std::string& body, std::string_view key = kAdminKey) {
  return env.request({.target = "/admin/purge", .body = body, .authorization = "Bearer " + std::string(key)});
}

ClientRequest tagged(const std::string& prompt, const std::string& tags) {
  return {.body = chat_body(prompt), .headers = {{"X-Semcache-Tags", tags}}};
}

TEST(Restart, CachedAnswersSurviveARestart) {
  TempDir dir;
  std::string first;
  {
    TestEnv env(fast_mock(), with_persistence(dir));
    auto r = env.chat("What is your refund policy?");
    ASSERT_EQ(r.header("x-semcache"), "MISS");
    first = r.body;
  }
  TestEnv env(fast_mock(), with_persistence(dir));
  EXPECT_EQ(env.recovery().loaded, 1u);
  auto r = env.chat("What is your refund policy?");
  EXPECT_EQ(r.header("x-semcache"), "HIT");
  EXPECT_EQ(nlohmann::json::parse(r.body)["choices"], nlohmann::json::parse(first)["choices"]);
  EXPECT_EQ(env.mock().chat_requests(), 0u) << "served from the recovered cache";
}

TEST(Purge, ByTagRemovesOnlyTaggedEntries) {
  TempDir dir;
  TestEnv env(fast_mock(), with_persistence(dir));
  env.request(tagged("refund policy?", "faq,billing"));
  env.chat("shipping times?");
  auto p = purge(env, R"({"tag":"faq"})");
  ASSERT_EQ(p.status, 200u) << p.body;
  EXPECT_EQ(nlohmann::json::parse(p.body)["purged"], 1);
  EXPECT_EQ(env.chat("refund policy?").header("x-semcache"), "MISS") << "purged";
  EXPECT_EQ(env.chat("shipping times?").header("x-semcache"), "HIT") << "untouched";
}

TEST(Purge, ByTenantIsScopedToThatTenant) {
  TempDir dir;
  TestEnv env(fast_mock(), with_persistence(dir));
  env.chat("hello");
  env.request({.body = chat_body("hello"), .authorization = "Bearer " + std::string(kOtherKey)});
  EXPECT_EQ(nlohmann::json::parse(purge(env, R"({"tenant":"test"})").body)["purged"], 1);
  EXPECT_EQ(env.chat("hello").header("x-semcache"), "MISS");
  EXPECT_EQ(env.request({.body = chat_body("hello"), .authorization = "Bearer " + std::string(kOtherKey)})
                .header("x-semcache"),
            "HIT");
}

TEST(Purge, SurvivesARestart) {
  TempDir dir;
  {
    TestEnv env(fast_mock(), with_persistence(dir));
    env.request(tagged("refund policy?", "faq"));
    ASSERT_EQ(purge(env, R"({"tag":"faq"})").status, 200u);
  }
  TestEnv env(fast_mock(), with_persistence(dir));
  EXPECT_EQ(env.chat("refund policy?").header("x-semcache"), "MISS") << "a purge is durable, not undone by replay";
}

TEST(Purge, ResponseInFlightDuringAPurgeIsNotStored) {
  TempDir dir;
  auto m = fast_mock();
  m.latency = std::chrono::milliseconds(400);
  TestEnv env(m, with_persistence(dir));
  auto slow = std::async(std::launch::async, [&] { return request(env.port(), tagged("refund policy?", "faq")); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the request is waiting on upstream
  ASSERT_EQ(purge(env, R"({"tag":"faq"})").status, 200u);
  EXPECT_EQ(slow.get().header("x-semcache"), "MISS");
  env.flush();
  EXPECT_EQ(env.request(tagged("refund policy?", "faq")).header("x-semcache"), "MISS")
      << "the answer computed before the purge must not reappear";
}

TEST(Purge, RequiresTheAdminKeyAndAValidFilter) {
  TempDir dir;
  TestEnv env(fast_mock(), with_persistence(dir));
  EXPECT_EQ(purge(env, R"({"tag":"faq"})", "wrong").status, 401u);
  EXPECT_EQ(purge(env, R"({"tag":"faq"})", kTestKey).status, 401u) << "a tenant key is not an admin key";
  EXPECT_EQ(purge(env, R"({})").status, 400u) << "purging everything must be explicit";
  EXPECT_EQ(purge(env, R"({"prompt":"x"})").status, 400u);
  EXPECT_EQ(purge(env, R"({"context":"zz"})").status, 400u);
  EXPECT_EQ(purge(env, "not json").status, 400u);
}

TEST(Purge, AdminEndpointIsDisabledWithoutAKey) {
  TestEnv env;  // no SEMCACHE_ADMIN_KEY
  EXPECT_EQ(purge(env, R"({"tag":"faq"})").status, 404u);
}

TEST(Tags, InvalidTagsBypassInsteadOfStoringAnUnpurgeableEntry) {
  TestEnv env;
  EXPECT_EQ(env.request(tagged("hello", "bad tag!")).header("x-semcache"), "BYPASS");
  EXPECT_EQ(env.chat("hello").header("x-semcache"), "MISS") << "nothing was stored";
  EXPECT_NE(env.metrics().find(R"(semcache_bypass_total{reason="invalid_tags"} 1)"), std::string::npos);
}

}  // namespace
}  // namespace semcache::test
