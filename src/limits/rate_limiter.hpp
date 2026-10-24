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
// Bounded: with max_keys > 0, a shard that grows past its share first drops buckets idle for at
// least burst/rate seconds (they have refilled completely, so dropping them is lossless), then,
// if every key is active, the least recently used one.
class RateLimiter {
 public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;

  RateLimiter(double rate_per_sec, double burst, Now now = [] { return Clock::now(); }, std::size_t max_keys = 0);

  struct Decision {
    bool allowed;
    double retry_after_s;  // seconds until one token is available; 0 when allowed
  };
  [[nodiscard]] Decision acquire(std::string_view key);
  [[nodiscard]] std::size_t tracked_keys() const;

 private:
  struct Bucket {
    double tokens;
    Clock::time_point last;
  };
  struct KeyHash {  // transparent: look up by string_view without allocating
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
  };
  struct Shard {
    mutable std::mutex mu;
    std::unordered_map<std::string, Bucket, KeyHash, std::equal_to<>> buckets;
  };
  static constexpr std::size_t kShards = 16;

  double rate_;
  double burst_;
  Now now_;
  std::size_t max_per_shard_;  // 0 = unbounded
  std::array<Shard, kShards> shards_;
};

}  // namespace semcache
