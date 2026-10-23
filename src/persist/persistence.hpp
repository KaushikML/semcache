#pragma once

// Snapshot + WAL persistence (ADR-008).
//
// - Every applied mutation reaches on_insert/on_remove (MutationObserver) on the writer thread and
//   is appended to an in-memory WAL buffer, tagged with a monotonically increasing LSN.
// - tick() (writer thread, between mutations, no store lock held) writes + fdatasyncs the buffer
//   every persistence.fsync_interval_ms, so at most that window is lost on a crash.
// - A snapshot is exactly point-in-time: the writer copies the (immutable, shared) entry pointers
//   at LSN L and starts a new WAL segment at L+1; a background thread serializes the copy to
//   snapshot.bin.tmp, fsyncs, renames it over snapshot.bin and deletes segments <= L.
// - recover() loads the snapshot, then replays segments with records > L. A corrupt snapshot is
//   moved aside; a torn WAL tail is truncated at the first bad record; damage in the middle stops
//   replay there (later deletes would be missing, so purged entries could come back).
// - Any disk error disables persistence (memory-only), removes the files best-effort (so a crash
//   cannot resurrect entries purged while disabled), and retries every retry_interval_ms.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cache/cache_store.hpp"
#include "cache/cache_writer.hpp"
#include "config/config.hpp"
#include "metrics/metrics.hpp"
#include "persist/files.hpp"

namespace semcache {

struct RecoveryReport {
  std::size_t snapshot_entries = 0;
  std::size_t wal_records = 0;
  std::size_t loaded = 0;   // entries present after recovery
  std::size_t skipped = 0;  // expired, over budget, or purged
  bool snapshot_corrupt = false;
  bool wal_truncated = false;
  bool wal_stopped_early = false;   // damage before the last segment
  bool fingerprint_mismatch = false;  // embeddings dropped: written by another model
  double seconds = 0;
};

class Persistence final : public MutationObserver, public WriterHook {
 public:

  // `fingerprint` identifies the embedding model (SHA-256 of the model file; zeros if none).
  Persistence(const PersistenceConfig& cfg, const Sha256Digest& fingerprint, MetricsRegistry& metrics);
  ~Persistence() override;
  Persistence(const Persistence&) = delete;
  Persistence& operator=(const Persistence&) = delete;

  // Loads disk state into `store` and opens a fresh WAL segment. Call once, before the store's
  // observer is set and before the writer thread runs.
  RecoveryReport recover(CacheStore& store);

  // MutationObserver (writer thread, inside the store's W lock: memory only).
  void on_insert(const CacheEntry& e) override;
  void on_remove(const CacheEntry& e) override;

  // WriterHook (writer thread, between mutations): fsync, snapshot, retry when due.
  void tick(CacheStore& store, Clock::time_point now) override;
  [[nodiscard]] Clock::time_point next_deadline() const override;
  // Final sync and wait for a running snapshot (shutdown). Idempotent.
  void close() override;

  [[nodiscard]] bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t lsn() const { return lsn_; }
  // Blocks until no snapshot is being written (tests).
  void wait_for_snapshot();

 private:
  struct SnapshotJob {
    std::vector<EntryPtr> entries;
    std::uint64_t lsn = 0;
  };

  void append(std::string record);
  bool sync_wal(std::string& error);
  bool open_segment(std::uint64_t first_lsn, std::string& error);
  void start_snapshot(CacheStore& store, Clock::time_point now);
  void snapshot_loop();
  bool write_snapshot(const SnapshotJob& job, std::string& error);
  void disable(const std::string& error);
  [[nodiscard]] std::string path(const std::string& name) const;

  const PersistenceConfig cfg_;
  const Sha256Digest fingerprint_;

  // Writer-thread state.
  std::uint64_t lsn_ = 0;
  std::string buffer_;
  AppendFile segment_;
  std::size_t segment_bytes_ = 0;
  Clock::time_point next_sync_;
  Clock::time_point next_snapshot_;
  Clock::time_point next_retry_;
  std::atomic<bool> enabled_{false};

  // Snapshot thread.
  std::mutex snap_mu_;
  std::condition_variable snap_cv_;
  std::optional<SnapshotJob> snap_job_;
  bool snap_busy_ = false;
  bool snap_stop_ = false;
  std::atomic<bool> snap_failed_{false};
  std::string snap_error_;  // guarded by snap_mu_
  std::thread snap_thread_;

  Gauge& enabled_gauge_;
  Gauge& wal_bytes_gauge_;
  Counter& errors_;
  Counter& snapshots_;
  Histogram& snapshot_seconds_;
  MetricsRegistry& metrics_;
};

// SHA-256 of a file's contents (the model fingerprint). Zeros if it cannot be read.
[[nodiscard]] Sha256Digest file_fingerprint(const std::string& path);

}  // namespace semcache
