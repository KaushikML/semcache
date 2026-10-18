#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace semcache {

// Token-bucket rate limiter keyed by an arbitrary string (tenant id, client IP).
// Sharded so that unrelated keys rarely contend on the same mutex.
// Buckets are never evicted: fine for a bounded key space (tenant ids). Per-IP limiting (Phase 8)
// must add idle-bucket expiry first.
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;

  RateLimiter(double rate_per_sec, double burst, Now now = [] { return Clock::now(); });

  struct Decision {
    bool allowed;
    double retry_after_s;  // seconds until one token is available; 0 when allowed
  };
  [[nodiscard]] Decision acquire(std::string_view key);

 private:
  struct Bucket {
    double tokens;
    Clock::time_point last;
  };
  struct Shard {
    std::mutex mu;
    std::unordered_map<std::string, Bucket> buckets;
  };
  static constexpr std::size_t kShards = 16;

  double rate_;
  double burst_;
  Now now_;
  std::array<Shard, kShards> shards_;
};

}  // namespace semcache
