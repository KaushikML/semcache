#include "persist/persistence.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#include "log/log.hpp"
#include "persist/codec.hpp"
#include "persist/log_format.hpp"

namespace semcache {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSnapshotName = "snapshot.bin";
constexpr std::string_view kSnapshotTmpName = "snapshot.bin.tmp";
// Write the WAL buffer early once it holds this much, regardless of the fsync interval.
constexpr std::size_t kSyncBytes = 1 << 20;
// Snapshot writes are batched into buffers of this size.
constexpr std::size_t kWriteChunkBytes = 1 << 20;

std::string segment_name(std::uint64_t first_lsn) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "wal-%016llx.log", static_cast<unsigned long long>(first_lsn));
  return buf;
}

std::optional<std::uint64_t> segment_lsn(const std::string& name) {
  if (name.size() != 24 || !name.starts_with("wal-") || !name.ends_with(".log")) return std::nullopt;
  try {
    return std::stoull(name.substr(4, 16), nullptr, 16);
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<std::pair<std::uint64_t, fs::path>> list_segments(const std::string& dir) {
  std::vector<std::pair<std::uint64_t, fs::path>> out;
  std::error_code ec;
  for (const auto& de : fs::directory_iterator(dir, ec)) {
    if (auto lsn = segment_lsn(de.path().filename().string())) out.emplace_back(*lsn, de.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string unix_suffix() {
  return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}

void set_aside(const fs::path& p) {
  std::error_code ec;
  fs::rename(p, fs::path(p.string() + ".corrupt-" + unix_suffix()), ec);
}

std::string remove_payload(const CacheEntry& e) {
  return std::string(reinterpret_cast<const char*>(e.context.data()), e.context.size()) +
         std::string(reinterpret_cast<const char*>(e.prompt_hash.data()), e.prompt_hash.size());
}

}  // namespace

Sha256Digest file_fingerprint(const std::string& path) {
  Sha256Digest out{};
  std::ifstream in(path, std::ios::binary);
  if (!in) return out;
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return out;
  std::vector<char> buf(1 << 20);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (in.gcount() > 0) EVP_DigestUpdate(ctx.get(), buf.data(), static_cast<std::size_t>(in.gcount()));
  }
  unsigned int len = 0;
  EVP_DigestFinal_ex(ctx.get(), out.data(), &len);
  return out;
}

Persistence::Persistence(const PersistenceConfig& cfg, const Sha256Digest& fingerprint, MetricsRegistry& m)
    : cfg_(cfg),
      fingerprint_(fingerprint),
      enabled_gauge_(m.gauge("semcache_persistence_enabled", "1 if mutations are being written to disk")),
      wal_bytes_gauge_(m.gauge("semcache_wal_bytes", "Bytes in the current WAL segment")),
      errors_(m.counter("semcache_persistence_errors_total", "Disk errors (persistence disabled until a retry works)")),
      snapshots_(m.counter("semcache_snapshots_total", "Snapshots written")),
      snapshot_seconds_(m.histogram("semcache_snapshot_seconds", "Time to write one snapshot", latency_buckets_seconds())),
      metrics_(m) {
  // Started here, not in the initializer list: the thread uses the members above.
  snap_thread_ = std::thread([this] { snapshot_loop(); });
}

Persistence::~Persistence() { close(); }

std::string Persistence::path(const std::string& name) const { return (fs::path(cfg_.dir) / name).string(); }

RecoveryReport Persistence::recover(CacheStore& store) {
  RecoveryReport rep;
  const auto t0 = std::chrono::steady_clock::now();
  const auto now = CacheClock::now();
  auto count = [&](std::string_view result) {
    metrics_.counter("semcache_persistence_recovery_total", "Startup recovery outcomes",
                     "result=\"" + std::string(result) + "\"")
        .inc();
  };
  auto apply = [&](NewEntry e) {
    if (store.insert(std::move(e), now) != InsertOutcome::stored) ++rep.skipped;
  };

  std::error_code ec;
  fs::create_directories(cfg_.dir, ec);

  // 1. Snapshot: all or nothing (a partially read snapshot is treated as corrupt).
  std::uint64_t snap_lsn = 0;
  if (fs::exists(path(std::string(kSnapshotName)))) {
    std::vector<NewEntry> entries;
    bool decode_ok = true;
    auto res = read_snapshot(path(std::string(kSnapshotName)), [&](const ReadResult& h, std::string_view payload) {
      auto e = decode_entry(payload, h.fingerprint == fingerprint_);
      if (!e) return decode_ok = false;
      entries.push_back(std::move(*e));
      return true;
    });
    if (!res.header_ok || !res.clean || !decode_ok) {
      rep.snapshot_corrupt = true;
      count("corrupt_snapshot");
      set_aside(path(std::string(kSnapshotName)));
      log::warn("snapshot is corrupt; set aside, starting without it", {{"records_read", res.records}});
    } else {
      snap_lsn = res.lsn;
      rep.snapshot_entries = entries.size();
      if (res.fingerprint != fingerprint_) rep.fingerprint_mismatch = true;
      for (auto& e : entries) apply(std::move(e));
    }
  }

  // 2. WAL segments in LSN order; records <= snapshot LSN are already in the snapshot.
  std::uint64_t max_lsn = snap_lsn;
  const auto segments = list_segments(cfg_.dir);
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const bool last = i + 1 == segments.size();
    const auto& seg = segments[i].second;
    bool content_ok = true;
    auto res = read_wal_segment(seg.string(), [&](const ReadResult& h, WalRecordType type, std::uint64_t lsn,
                                                   std::string_view payload) {
      if (lsn <= snap_lsn) return true;
      if (type == WalRecordType::insert) {
        auto e = decode_entry(payload, h.fingerprint == fingerprint_);
        if (!e) return content_ok = false;
        apply(std::move(*e));
      } else {
        if (payload.size() != 64) return content_ok = false;
        ContextKey ctx;
        PromptHash ph;
        std::memcpy(ctx.data(), payload.data(), 32);
        std::memcpy(ph.data(), payload.data() + 32, 32);
        store.erase(ctx, ph);
      }
      ++rep.wal_records;
      max_lsn = std::max(max_lsn, lsn);
      return true;
    });
    if (res.header_ok && res.fingerprint != fingerprint_) rep.fingerprint_mismatch = true;
    if (res.header_ok && res.clean && content_ok) continue;
    if (last && res.header_ok) {  // torn tail of the segment being written at the crash
      fs::resize_file(seg, res.good_bytes, ec);
      rep.wal_truncated = true;
      count("wal_truncated");
      continue;
    }
    if (last) {  // crash while creating the segment: no valid header, nothing to keep
      fs::remove(seg, ec);
      continue;
    }
    // Damage before the last segment: stop. Replaying later segments would skip the deletes
    // lost here, which could bring purged entries back.
    rep.wal_stopped_early = true;
    count("wal_corrupt");
    for (std::size_t j = i; j < segments.size(); ++j) set_aside(segments[j].second);
    log::warn("WAL segment is corrupt; replay stopped there", {{"segment", seg.filename().string()}});
    break;
  }
  if (rep.fingerprint_mismatch) {
    count("fingerprint_mismatch");
    log::warn("persisted embeddings come from another model; loaded entries as exact-match only");
  }

  lsn_ = max_lsn;
  rep.loaded = store.stats().entries;
  rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  count("ok");

  std::string error;
  const auto t = Clock::now();
  next_sync_ = t + cfg_.fsync_interval;
  next_snapshot_ = t + cfg_.snapshot_interval;
  if (open_segment(lsn_ + 1, error)) {
    enabled_ = true;
    enabled_gauge_.set(1);
  } else {
    disable(error);
  }
  return rep;
}

bool Persistence::open_segment(std::uint64_t first_lsn, std::string& error) {
  AppendFile f;
  if (!f.create(path(segment_name(first_lsn)), error)) return false;
  const auto header = wal_segment_header(first_lsn, fingerprint_);
  if (!f.write(header, error) || !f.sync(error) || !sync_dir(cfg_.dir, error)) return false;
  segment_ = std::move(f);
  segment_bytes_ = header.size();
  wal_bytes_gauge_.set(static_cast<std::int64_t>(segment_bytes_));
  return true;
}

void Persistence::on_insert(const CacheEntry& e) {
  if (!enabled()) return;
  ByteWriter w;
  encode_entry(w, e);
  append(wal_record(WalRecordType::insert, ++lsn_, w.data()));
}

void Persistence::on_remove(const CacheEntry& e) {
  if (!enabled()) return;
  append(wal_record(WalRecordType::remove, ++lsn_, remove_payload(e)));
}

void Persistence::append(std::string record) { buffer_ += record; }

bool Persistence::sync_wal(std::string& error) {
  if (buffer_.empty()) return true;
  if (!segment_.write(buffer_, error) || !segment_.sync(error)) return false;
  segment_bytes_ += buffer_.size();
  wal_bytes_gauge_.set(static_cast<std::int64_t>(segment_bytes_));
  buffer_.clear();
  return true;
}

void Persistence::tick(CacheStore& store, Clock::time_point now) {
  if (snap_failed_.exchange(false)) {
    std::string error;
    {
      std::lock_guard lock(snap_mu_);
      error = snap_error_;
    }
    disable(error);
  }
  if (!enabled()) {
    if (now < next_retry_) return;
    std::string error;
    std::error_code ec;
    fs::create_directories(cfg_.dir, ec);
    if (!open_segment(lsn_ + 1, error)) {
      next_retry_ = now + cfg_.retry_interval;
      return;
    }
    enabled_ = true;
    enabled_gauge_.set(1);
    log::info("persistence re-enabled; writing a full snapshot");
    start_snapshot(store, now);  // the WAL missed everything while disabled
    return;
  }
  if (!buffer_.empty() && (now >= next_sync_ || buffer_.size() >= kSyncBytes)) {
    std::string error;
    if (!sync_wal(error)) return disable(error);
    next_sync_ = now + cfg_.fsync_interval;
  }
  if (now >= next_snapshot_ || segment_bytes_ >= cfg_.max_wal_bytes) start_snapshot(store, now);
}

Persistence::Clock::time_point Persistence::next_deadline() const {
  if (!enabled()) return next_retry_;
  return buffer_.empty() ? next_snapshot_ : std::min(next_sync_, next_snapshot_);
}

void Persistence::start_snapshot(CacheStore& store, Clock::time_point now) {
  {
    std::lock_guard lock(snap_mu_);
    if (snap_busy_ || snap_job_) {  // the previous one is still writing: try again shortly
      next_snapshot_ = now + cfg_.fsync_interval;
      return;
    }
  }
  std::string error;
  // Everything up to lsn_ must be durable in the old segment before the new one starts.
  if (!sync_wal(error)) return disable(error);
  SnapshotJob job{store.all_entries(), lsn_};
  if (!open_segment(lsn_ + 1, error)) return disable(error);
  {
    std::lock_guard lock(snap_mu_);
    snap_job_ = std::move(job);
  }
  snap_cv_.notify_all();
  next_snapshot_ = now + cfg_.snapshot_interval;
}

void Persistence::snapshot_loop() {
  for (;;) {
    SnapshotJob job;
    {
      std::unique_lock lock(snap_mu_);
      snap_cv_.wait(lock, [this] { return snap_stop_ || snap_job_.has_value(); });
      if (!snap_job_) return;  // stopping with nothing pending
      job = std::move(*snap_job_);
      snap_job_.reset();
      snap_busy_ = true;
    }
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = write_snapshot(job, error);
    if (ok) {
      snapshots_.inc();
      snapshot_seconds_.observe(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
      for (const auto& [first, p] : list_segments(cfg_.dir)) {  // everything <= lsn is in the snapshot now
        std::error_code ec;
        if (first <= job.lsn) fs::remove(p, ec);
      }
    }
    {
      std::lock_guard lock(snap_mu_);
      if (!ok) snap_error_ = error;
      snap_busy_ = false;
    }
    if (!ok) snap_failed_ = true;
    snap_cv_.notify_all();
  }
}

bool Persistence::write_snapshot(const SnapshotJob& job, std::string& error) {
  AppendFile f;
  const auto tmp = path(std::string(kSnapshotTmpName));
  if (!f.create(tmp, error)) return false;
  std::string chunk = snapshot_header(job.lsn, job.entries.size(), fingerprint_);
  for (const auto& e : job.entries) {
    ByteWriter w;
    encode_entry(w, *e);
    chunk += snapshot_record(w.data());
    if (chunk.size() >= kWriteChunkBytes) {
      if (!f.write(chunk, error)) return false;
      chunk.clear();
    }
  }
  if (!f.write(chunk, error) || !f.sync(error)) return false;
  f.close();
  // Disabled meanwhile (disk error): do not publish a snapshot that may predate a purge.
  if (!enabled()) {
    error = "persistence disabled during snapshot";
    std::error_code ec;
    fs::remove(tmp, ec);
    return true;
  }
  std::error_code ec;
  fs::rename(tmp, path(std::string(kSnapshotName)), ec);
  if (ec) {
    error = "rename snapshot: " + ec.message();
    return false;
  }
  return sync_dir(cfg_.dir, error);
}

void Persistence::disable(const std::string& error) {
  const bool was_enabled = enabled_.exchange(false);
  enabled_gauge_.set(0);
  errors_.inc();
  segment_.close();
  buffer_.clear();
  next_retry_ = Clock::now() + cfg_.retry_interval;
  if (was_enabled) {
    log::error("persistence disabled; serving from memory only", {{"error", error}});
    // Best effort: without these files a crash cannot resurrect entries purged while disabled.
    std::error_code ec;
    fs::remove(path(std::string(kSnapshotName)), ec);
    for (const auto& [first, p] : list_segments(cfg_.dir)) fs::remove(p, ec);
  }
}

void Persistence::wait_for_snapshot() {
  std::unique_lock lock(snap_mu_);
  snap_cv_.wait(lock, [this] { return !snap_busy_ && !snap_job_; });
}

void Persistence::close() {
  if (enabled()) {
    std::string error;
    if (!sync_wal(error)) disable(error);
  }
  {
    std::lock_guard lock(snap_mu_);
    snap_stop_ = true;
  }
  snap_cv_.notify_all();
  if (snap_thread_.joinable()) snap_thread_.join();
  segment_.close();
}

}  // namespace semcache
