#include "limits/rate_limiter.hpp"

#include <algorithm>

namespace semcache {

RateLimiter::RateLimiter(double rate_per_sec, double burst, Now now, std::size_t max_keys)
    : rate_(rate_per_sec),
      burst_(burst),
      now_(std::move(now)),
      max_per_shard_(max_keys == 0 ? 0 : std::max<std::size_t>(1, max_keys / kShards)) {}

RateLimiter::Decision RateLimiter::acquire(std::string_view key) {
  auto& shard = shards_[std::hash<std::string_view>{}(key) % kShards];
  const auto t = now_();
  std::lock_guard lock(shard.mu);
  auto it = shard.buckets.find(key);
  if (it == shard.buckets.end()) {
    if (max_per_shard_ != 0 && shard.buckets.size() >= max_per_shard_) {
      // Lossless first: a bucket idle this long is full again, exactly like a new one.
      const auto refill = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(burst_ / rate_));
      std::erase_if(shard.buckets, [&](const auto& kv) { return t - kv.second.last >= refill; });
      if (shard.buckets.size() >= max_per_shard_) {
        auto oldest = std::min_element(shard.buckets.begin(), shard.buckets.end(),
                                       [](const auto& a, const auto& b) { return a.second.last < b.second.last; });
        shard.buckets.erase(oldest);
      }
    }
    it = shard.buckets.emplace(std::string(key), Bucket{burst_, t}).first;
  } else {
    auto& b = it->second;
    const double elapsed = std::chrono::duration<double>(t - b.last).count();
    b.tokens = std::min(burst_, b.tokens + elapsed * rate_);
    b.last = t;
  }
  auto& b = it->second;
  if (b.tokens >= 1.0) {
    b.tokens -= 1.0;
    return {true, 0.0};
  }
  return {false, (1.0 - b.tokens) / rate_};
}

std::size_t RateLimiter::tracked_keys() const {
  std::size_t n = 0;
  for (const auto& s : shards_) {
    std::lock_guard lock(s.mu);
    n += s.buckets.size();
  }
  return n;
}

}  // namespace semcache
