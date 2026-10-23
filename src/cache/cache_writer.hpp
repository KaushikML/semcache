#pragma once

// The single writer thread (design §7.1): all CacheStore mutations (inserts, TTL expiry) are
// applied in submission order on one thread, so request threads never wait on exclusive index
// locks and the mutation order is total (Phase 6 appends it to the WAL in the same order).
// Inserts become visible asynchronously, typically well under a millisecond after submission.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <variant>

#include "cache/cache_store.hpp"
#include "metrics/metrics.hpp"

namespace semcache {

// Periodic work that must run on the writer thread between mutations (persistence: WAL fsync,
// snapshots). Implemented by Persistence; keeps this module independent of it.
class WriterHook {
 public:
  using Clock = std::chrono::steady_clock;
  virtual ~WriterHook() = default;
  virtual void tick(CacheStore& store, Clock::time_point now) = 0;
  [[nodiscard]] virtual Clock::time_point next_deadline() const = 0;
  virtual void close() = 0;
};

class CacheWriter {
 public:
  // Runs on the writer thread once the insert was attempted, whatever the outcome. Must not block.
  using AfterInsert = std::function<void(InsertOutcome)>;

  using PurgeDone = std::function<void(std::size_t purged)>;

  // `hook`, if any, is ticked after every mutation and whenever its deadline passes.
  CacheWriter(CacheStore& store, std::size_t capacity, MetricsRegistry& metrics, WriterHook* hook = nullptr);
  ~CacheWriter();
  CacheWriter(const CacheWriter&) = delete;
  CacheWriter& operator=(const CacheWriter&) = delete;

  // Returns false (and counts semcache_store_dropped_total) when the queue is full or stopping;
  // `after` is not called in that case.
  bool submit_insert(NewEntry e, AfterInsert after = {});
  // Queues a TTL sweep unless one is already queued.
  void submit_expire();
  // Queues a purge; `done` runs on the writer thread with the number removed. False if full.
  bool submit_purge(PurgeFilter filter, PurgeDone done);
  // Blocks until everything submitted before the call has been applied. For tests and shutdown.
  void flush();
  // Applies what is queued, then stops the thread. Idempotent.
  void stop();

 private:
  struct Insert {
    NewEntry entry;
    AfterInsert after;
  };
  struct Expire {};
  struct Purge {
    PurgeFilter filter;
    PurgeDone done;
  };
  using Job = std::variant<Insert, Expire, Purge>;

  void apply(Job& job);

  void run();

  CacheStore& store_;
  WriterHook* hook_;
  const std::size_t capacity_;
  std::mutex mu_;
  std::condition_variable cv_;       // work available / stopping
  std::condition_variable done_cv_;  // progress, for flush()
  std::deque<Job> queue_;
  std::uint64_t submitted_ = 0;
  std::uint64_t applied_ = 0;
  bool expire_queued_ = false;
  bool stopping_ = false;
  bool exited_ = false;  // the thread has finished; flush() must not wait for it any more
  Counter& dropped_;
  Gauge& depth_;
  std::thread thread_;  // last: starts after everything above is initialized
};

}  // namespace semcache
