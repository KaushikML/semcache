#pragma once

// In-flight request coalescing (design F8, §6.2, ADR-009).
//
// Identical cacheable requests (same FlightKey) that arrive while one is being served share it:
// the first becomes the leader and serves normally through a LeaderWriter, which records
// everything its client receives into the Flight. Followers replay that log into their own
// connection, live, with their own X-Semcache headers. Followers see exactly the leader's
// response, including errors (no retry storm). If the leader disappears before any response head
// was recorded, followers fall back to serving themselves (fail open).
//
// Locks: L1 (flight map) and L1b (one flight) are never held together, and never across I/O or
// while notifying followers.

#include <boost/asio/experimental/concurrent_channel.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/context_key.hpp"
#include "config/config.hpp"
#include "http/response_writer.hpp"
#include "metrics/metrics.hpp"

namespace semcache {

// Followers replay the leader's bytes, so everything that changes the response *format* is part
// of the key on top of the context: a streamed and a non-streamed request never share a flight.
struct FlightKey {
  ContextKey context{};
  PromptHash prompt{};
  bool stream = false;
  bool include_usage = false;

  bool operator==(const FlightKey&) const = default;
};

struct FlightKeyHash {
  std::size_t operator()(const FlightKey& k) const noexcept;
};

class Flight {
 public:
  enum class State { running, done, aborted };
  struct Head {
    unsigned status = 0;
    std::string content_type;
    HeaderList headers;  // without X-Semcache-*: those are per request
    bool streaming = false;
  };
  using Wakeup = net::experimental::concurrent_channel<void(boost::system::error_code)>;

  explicit Flight(std::size_t max_log_bytes) : max_log_bytes_(max_log_bytes) {}

  // Leader side. Each call wakes the followers.
  void record_send(unsigned status, std::string_view content_type, const std::string& body, const HeaderList& headers);
  void record_begin(unsigned status, std::string_view content_type, const HeaderList& headers);
  void record_chunk(std::string_view data);
  void record_end();
  // The leader stopped without finishing (cancelled, exception). No-op once done.
  void abort();

  // Follower side. Returns nullopt when the flight already has `max_followers` followers.
  std::optional<std::uint64_t> add_follower(std::shared_ptr<Wakeup> wakeup, std::size_t max_followers);
  void remove_follower(std::uint64_t id);
  [[nodiscard]] std::size_t followers() const { return follower_count_.load(std::memory_order_acquire); }

  struct View {
    std::optional<Head> head;
    std::string body;                                     // non-streamed response
    std::vector<std::shared_ptr<const std::string>> chunks;  // streamed chunks from `from` onwards
    State state = State::running;
  };
  [[nodiscard]] View read(std::size_t from) const;

 private:
  void notify();
  bool finish_locked(State s);

  const std::size_t max_log_bytes_;
  mutable std::mutex mu_;  // L1b
  std::optional<Head> head_;
  std::string body_;
  std::vector<std::shared_ptr<const std::string>> chunks_;
  std::size_t log_bytes_ = 0;
  State state_ = State::running;
  std::map<std::uint64_t, std::shared_ptr<Wakeup>> waiters_;
  std::uint64_t next_follower_ = 0;
  std::atomic<std::size_t> follower_count_{0};
};

using FlightPtr = std::shared_ptr<Flight>;

class Coalescer {
 public:
  Coalescer(const CoalescerConfig& cfg, std::size_t max_log_bytes, MetricsRegistry& metrics);

  struct Join {
    bool leader = false;
    FlightPtr flight;
  };
  // Leader: a new flight was registered under `key`. Follower: an existing flight is returned.
  [[nodiscard]] Join join(const FlightKey& key);
  // Unregisters `flight` if it is still the one under `key`. Called once the leader's response is
  // complete and its cache insert was applied, so late duplicates replay instead of re-fetching.
  void retire(const FlightKey& key, const FlightPtr& flight);

  [[nodiscard]] std::size_t max_followers() const { return cfg_.max_waiters_per_flight; }
  [[nodiscard]] std::size_t active() const;

 private:
  const CoalescerConfig cfg_;
  const std::size_t max_log_bytes_;
  mutable std::mutex mu_;  // L1
  std::unordered_map<FlightKey, FlightPtr, FlightKeyHash> flights_;
  Gauge& active_gauge_;
};

// ResponseWriter for a leader: writes to the leader's client and records into the flight. Keeps
// reporting success after the leader's client is gone while followers still need the response.
class LeaderWriter final : public ResponseWriter {
 public:
  LeaderWriter(ResponseWriter& client, FlightPtr flight) : client_(client), flight_(std::move(flight)) {}

  Task<bool> send(unsigned status, std::string_view content_type, std::string body,
                  const HeaderList& headers) override;
  Task<bool> begin_stream(unsigned status, std::string_view content_type, const HeaderList& headers) override;
  Task<bool> write_chunk(std::string_view data) override;
  Task<bool> end_stream() override;

  [[nodiscard]] bool client_ok() const { return client_ok_; }

 private:
  [[nodiscard]] bool keep_going() const { return client_ok_ || flight_->followers() > 0; }

  ResponseWriter& client_;
  FlightPtr flight_;
  bool client_ok_ = true;
};

struct FollowResult {
  bool served = false;  // false: the caller must serve the request itself (cap reached, or leader gone)
  unsigned status = 0;
  bool streamed = false;
  bool keep_alive = true;
};

// Replays `flight` into `out`. `headers` produces this request's X-Semcache-* headers at the
// moment the response head is written. Parameters are taken by value: a coroutine outlives the
// full-expression that created it, so references to temporaries would dangle.
Task<FollowResult> follow(FlightPtr flight, std::size_t max_followers, ResponseWriter& out,
                          std::function<HeaderList()> headers);

}  // namespace semcache
