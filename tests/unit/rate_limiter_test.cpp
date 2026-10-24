#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "limits/rate_limiter.hpp"

namespace semcache {
namespace {

TEST(RateLimiter, BurstThenRefill) {
  auto t = RateLimiter::Clock::time_point{};
  RateLimiter rl(2.0, 3.0, [&] { return t; });
  EXPECT_TRUE(rl.acquire("a").allowed);
  EXPECT_TRUE(rl.acquire("a").allowed);
  EXPECT_TRUE(rl.acquire("a").allowed);
  auto denied = rl.acquire("a");
  EXPECT_FALSE(denied.allowed);
  EXPECT_NEAR(denied.retry_after_s, 0.5, 1e-9);  // 1 token at 2/s

  t += std::chrono::milliseconds(500);
  EXPECT_TRUE(rl.acquire("a").allowed);
  EXPECT_FALSE(rl.acquire("a").allowed);

  t += std::chrono::seconds(100);  // refill is capped at burst
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(rl.acquire("a").allowed);
  EXPECT_FALSE(rl.acquire("a").allowed);
}

TEST(RateLimiter, KeysAreIndependent) {
  auto t = RateLimiter::Clock::time_point{};
  RateLimiter rl(1.0, 1.0, [&] { return t; });
  EXPECT_TRUE(rl.acquire("a").allowed);
  EXPECT_FALSE(rl.acquire("a").allowed);
  EXPECT_TRUE(rl.acquire("b").allowed);
}

// Keys that land in the same shard as `key` (the limiter shards by std::hash % 16).
std::vector<std::string> same_shard(const std::string& key, std::size_t n) {
  const auto shard = std::hash<std::string_view>{}(key) % 16;
  std::vector<std::string> out;
  for (int i = 0; out.size() < n; ++i) {
    auto k = "k" + std::to_string(i);
    if (k != key && std::hash<std::string_view>{}(k) % 16 == shard) out.push_back(k);
  }
  return out;
}

TEST(RateLimiter, TrackedKeysAreBounded) {
  auto t = RateLimiter::Clock::time_point{};
  RateLimiter rl(1.0, 1.0, [&] { return t; }, 32);  // 2 per shard
  for (int i = 0; i < 1000; ++i) (void)rl.acquire("ip" + std::to_string(i));
  EXPECT_LE(rl.tracked_keys(), 32u);
}

TEST(RateLimiter, FullShardDropsIdleBucketsFirst) {
  auto t = RateLimiter::Clock::time_point{};
  RateLimiter rl(1.0, 2.0, [&] { return t; }, 32);  // 2 per shard; idle after burst/rate = 2 s
  const auto others = same_shard("active", 2);
  (void)rl.acquire(others[0]);  // becomes idle
  t += std::chrono::seconds(3);
  EXPECT_TRUE(rl.acquire("active").allowed);
  EXPECT_TRUE(rl.acquire("active").allowed);
  EXPECT_FALSE(rl.acquire("active").allowed);
  (void)rl.acquire(others[1]);  // shard full: the idle bucket goes, the exhausted one stays
  EXPECT_FALSE(rl.acquire("active").allowed) << "an active client's bucket must not be reset by eviction";
}

TEST(RateLimiter, WhenAllAreActiveTheLeastRecentlyUsedIsEvicted) {
  auto t = RateLimiter::Clock::time_point{};
  RateLimiter rl(1.0, 1.0, [&] { return t; }, 16);  // 1 per shard
  const auto other = same_shard("a", 1)[0];
  EXPECT_TRUE(rl.acquire("a").allowed);
  EXPECT_FALSE(rl.acquire("a").allowed);
  EXPECT_TRUE(rl.acquire(other).allowed);  // evicts "a"
  EXPECT_TRUE(rl.acquire("a").allowed) << "known limit: eviction under pressure forgets a bucket";
}

}  // namespace
}  // namespace semcache
