#include "limits/rate_limiter.hpp"

#include <algorithm>

namespace semcache {

RateLimiter::RateLimiter(double rate_per_sec, double burst, Now now)
    : rate_(rate_per_sec), burst_(burst), now_(std::move(now)) {}

RateLimiter::Decision RateLimiter::acquire(std::string_view key) {
  auto& shard = shards_[std::hash<std::string_view>{}(key) % kShards];
  const auto t = now_();
  std::lock_guard lock(shard.mu);
  auto [it, inserted] = shard.buckets.try_emplace(std::string(key), Bucket{burst_, t});
  auto& b = it->second;
  if (!inserted) {
    const double elapsed = std::chrono::duration<double>(t - b.last).count();
    b.tokens = std::min(burst_, b.tokens + elapsed * rate_);
    b.last = t;
  }
  if (b.tokens >= 1.0) {
    b.tokens -= 1.0;
    return {true, 0.0};
  }
  return {false, (1.0 - b.tokens) / rate_};
}

}  // namespace semcache
