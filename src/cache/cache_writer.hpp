#pragma once

// The single writer thread (design §7.1): all CacheStore mutations (inserts, TTL expiry) are
// applied in submission order on one thread, so request threads never wait on exclusive index
// locks and the mutation order is total (Phase 6 appends it to the WAL in the same order).
// Inserts become visible asynchronously, typically well under a millisecond after submission.

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

class CacheWriter {
 public:
  // Runs on the writer thread once the insert was attempted, whatever the outcome. Must not block.
  using AfterInsert = std::function<void(InsertOutcome)>;

  CacheWriter(CacheStore& store, std::size_t capacity, MetricsRegistry& metrics);
  ~CacheWriter();
  CacheWriter(const CacheWriter&) = delete;
  CacheWriter& operator=(const CacheWriter&) = delete;

  // Returns false (and counts semcache_store_dropped_total) when the queue is full or stopping;
  // `after` is not called in that case.
  bool submit_insert(NewEntry e, AfterInsert after = {});
  // Queues a TTL sweep unless one is already queued.
  void submit_expire();
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
  using Job = std::variant<Insert, Expire>;

  void run();

  CacheStore& store_;
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
