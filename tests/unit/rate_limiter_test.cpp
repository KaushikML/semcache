#include <gtest/gtest.h>

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

}  // namespace
}  // namespace semcache
