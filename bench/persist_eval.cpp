// Persistence cost (Phase 6): WAL replay, snapshot write and snapshot load times, file sizes.
// Answers ADR-008's open question: is rebuilding indexes from vectors on startup fast enough, or
// must HNSW graphs be persisted?
//
// Usage: semcache_persist_eval [--entries 20000] [--partitions 1] [--response-bytes 2000] [--dir /tmp/x]
// Entries are spread evenly over `partitions` contexts; one partition >= 5000 entries becomes HNSW.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

#include "cache/cache_writer.hpp"
#include "log/log.hpp"
#include "persist/persistence.hpp"

namespace {

using namespace semcache;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double secs_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

Embedding vec(std::mt19937& rng) {
  std::normal_distribution<float> d;
  Embedding v{};
  float n = 0;
  for (auto& x : v) {
    x = d(rng);
    n += x * x;
  }
  for (auto& x : v) x /= std::sqrt(n);
  return v;
}

std::uintmax_t dir_bytes(const fs::path& dir, std::string_view prefix) {
  std::uintmax_t total = 0;
  for (const auto& de : fs::directory_iterator(dir)) {
    if (de.path().filename().string().starts_with(prefix)) total += de.file_size();
  }
  return total;
}

struct Node {
  MetricsRegistry registry;
  CacheStore store;
  Persistence persistence;
  RecoveryReport recovery;
  std::unique_ptr<CacheWriter> writer;

  Node(const PersistenceConfig& pc, CacheConfig cc) : store(cc, {}, registry), persistence(pc, Sha256Digest{}, registry) {
    recovery = persistence.recover(store);
    store.set_observer(&persistence);
    writer = std::make_unique<CacheWriter>(store, 1 << 16, registry, &persistence);
  }
  ~Node() { writer->stop(); }
};

}  // namespace

int main(int argc, char** argv) {
  std::size_t entries = 20'000;
  std::size_t partitions = 1;
  std::size_t response_bytes = 2000;  // design §5.1 estimate for a response
  fs::path dir = fs::temp_directory_path() / "semcache-persist-eval";
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--entries") entries = std::stoul(argv[i + 1]);
    else if (a == "--partitions") partitions = std::stoul(argv[i + 1]);
    else if (a == "--response-bytes") response_bytes = std::stoul(argv[i + 1]);
    else if (a == "--dir") dir = argv[i + 1];
  }
  log::init("warn");
  fs::remove_all(dir);

  PersistenceConfig pc;
  pc.dir = dir.string();
  pc.snapshot_interval = std::chrono::hours(24);  // only the explicit snapshot below
  CacheConfig cc;
  cc.max_bytes = std::size_t{8} << 30;  // no eviction during the measurement

  // 1. Write N entries through the writer (WAL only).
  double insert_s = 0;
  {
    Node a(pc, cc);
    std::mt19937 rng(1);
    const std::string response(response_bytes, 'r');
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < entries; ++i) {
      NewEntry e;
      e.context.fill(static_cast<std::uint8_t>(i % partitions));
      e.context[1] = static_cast<std::uint8_t>((i % partitions) >> 8);
      for (std::size_t b = 0; b < 8; ++b) e.prompt_hash[b] = static_cast<std::uint8_t>(i >> (8 * b));
      e.tenant = "acme";
      e.prompt = "prompt number " + std::to_string(i);
      e.embedding = vec(rng);
      e.response = response;
      while (!a.writer->submit_insert(std::move(e))) a.writer->flush();  // queue full: wait
    }
    a.writer->flush();
    insert_s = secs_since(t0);
  }
  const auto wal_bytes = dir_bytes(dir, "wal-");

  // 2. Recover from the WAL alone, then write a snapshot.
  double wal_recovery_s = 0;
  double snapshot_s = 0;
  std::size_t hnsw = 0;
  {
    PersistenceConfig snap = pc;
    snap.max_wal_bytes = 1;  // the next tick snapshots
    Node b(snap, cc);
    wal_recovery_s = b.recovery.seconds;
    hnsw = b.store.stats().hnsw_partitions;
    const auto t0 = Clock::now();
    b.writer->submit_expire();  // any job => a tick => snapshot
    b.writer->flush();
    b.persistence.wait_for_snapshot();
    snapshot_s = secs_since(t0);
  }
  const auto snapshot_bytes = dir_bytes(dir, "snapshot.bin");

  // 3. Recover from the snapshot.
  double snapshot_recovery_s = 0;
  std::size_t loaded = 0;
  {
    Node c(pc, cc);
    snapshot_recovery_s = c.recovery.seconds;
    loaded = c.recovery.loaded;
  }

  std::printf("entries=%zu partitions=%zu hnsw_partitions=%zu response_bytes=%zu\n", entries, partitions, hnsw,
              response_bytes);
  std::printf("insert via writer (WAL on):  %8.2f s  (%.0f inserts/s)\n", insert_s, static_cast<double>(entries) / insert_s);
  std::printf("WAL size:                    %8.1f MB\n", static_cast<double>(wal_bytes) / 1e6);
  std::printf("recover from WAL:            %8.2f s\n", wal_recovery_s);
  std::printf("write snapshot:              %8.2f s\n", snapshot_s);
  std::printf("snapshot size:               %8.1f MB\n", static_cast<double>(snapshot_bytes) / 1e6);
  std::printf("recover from snapshot:       %8.2f s  (%zu entries loaded)\n", snapshot_recovery_s, loaded);
  fs::remove_all(dir);
  return loaded == entries ? 0 : 1;
}
