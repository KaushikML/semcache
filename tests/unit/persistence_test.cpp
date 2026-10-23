// Persistence (Phase 6): formats, recovery and fault injection on real files.

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include "cache/cache_writer.hpp"
#include "log/log.hpp"
#include "persist/codec.hpp"
#include "persist/crc32c.hpp"
#include "persist/log_format.hpp"
#include "persist/persistence.hpp"

namespace semcache {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ----------------------------------------------------------------- formats

TEST(Crc32c, KnownVectors) {
  EXPECT_EQ(crc32c(""), 0u);
  EXPECT_EQ(crc32c("123456789"), 0xE3069283u);  // the standard CRC-32C check value
  EXPECT_NE(crc32c("123456788"), crc32c("123456789"));
}

Embedding vec(std::uint32_t seed) {
  std::mt19937 rng(seed);
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

// CacheEntry holds atomics (not movable), so tests fill one in place.
void fill_sample(CacheEntry& e) {
  e.context.fill(7);
  e.prompt_hash.fill(9);
  e.tenant = "acme";
  e.prompt = "Wie spät ist es? 你好";
  e.embedding = vec(1);
  e.response = R"({"choices":[{"message":{"content":"hi"}}]})";
  e.total_tokens = 42;
  e.tags = {"faq", "v2"};
  e.created = CacheClock::time_point(std::chrono::milliseconds(1'700'000'000'123));
  e.expires = e.created + std::chrono::hours(24);
}

TEST(Codec, EntryRoundTrips) {
  CacheEntry e;
  fill_sample(e);
  ByteWriter w;
  encode_entry(w, e);
  auto d = decode_entry(w.data(), /*keep_embedding=*/true);
  ASSERT_TRUE(d);
  EXPECT_EQ(d->context, e.context);
  EXPECT_EQ(d->prompt_hash, e.prompt_hash);
  EXPECT_EQ(d->tenant, e.tenant);
  EXPECT_EQ(d->prompt, e.prompt);
  EXPECT_EQ(d->response, e.response);
  EXPECT_EQ(d->total_tokens, 42);
  EXPECT_EQ(d->tags, e.tags);
  EXPECT_EQ(*d->created, e.created);
  EXPECT_EQ(*d->expires, e.expires);
  ASSERT_TRUE(d->embedding);
  EXPECT_EQ(*d->embedding, *e.embedding) << "floats are stored bit-exact";
  EXPECT_FALSE(decode_entry(w.data(), /*keep_embedding=*/false)->embedding) << "model changed: drop vectors";
}

TEST(Codec, RejectsEveryTruncationAndTrailingBytes) {
  CacheEntry e;
  fill_sample(e);
  ByteWriter w;
  encode_entry(w, e);
  const std::string full = w.data();
  for (std::size_t n = 0; n < full.size(); ++n) EXPECT_FALSE(decode_entry(full.substr(0, n), true)) << n;
  EXPECT_FALSE(decode_entry(full + "x", true));
}

TEST(Codec, RandomBytesNeverCrash) {
  std::mt19937 rng(3);
  for (int i = 0; i < 2000; ++i) {
    std::string junk(rng() % 600, '\0');
    for (auto& c : junk) c = static_cast<char>(rng());
    (void)decode_entry(junk, true);  // must return, not crash or over-read (ASan)
  }
}

// ----------------------------------------------------------------- fixtures

class PersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    log::init("warn");
    dir_ = fs::temp_directory_path() /
           ("semcache-persist-" + std::to_string(::getpid()) + "-" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    cfg_.enabled = true;
    cfg_.dir = dir_.string();
    cfg_.fsync_interval = 1ms;
  }
  void TearDown() override {
    ::chmod(dir_.c_str(), 0755);
    fs::remove_all(dir_);
  }

  // A running "process": store + persistence + writer. Destroying it is a clean shutdown.
  struct Node {
    MetricsRegistry registry;
    CacheStore store;
    Persistence persistence;
    RecoveryReport recovery;
    std::unique_ptr<CacheWriter> writer;

    Node(const PersistenceConfig& pc, const Sha256Digest& fp, CacheConfig cc = {})
        : store(cc, {}, registry), persistence(pc, fp, registry) {
      recovery = persistence.recover(store);
      store.set_observer(&persistence);
      writer = std::make_unique<CacheWriter>(store, 1024, registry, &persistence);
    }
    ~Node() { writer->stop(); }
    void insert(NewEntry e) {
      writer->submit_insert(std::move(e));
      writer->flush();
    }
    std::size_t purge(PurgeFilter f) {
      std::atomic<std::size_t> n{0};
      writer->submit_purge(std::move(f), [&](std::size_t k) { n = k; });
      writer->flush();
      return n;
    }
    bool has(std::uint8_t ctx, std::uint8_t prompt) const {
      ContextKey c;
      c.fill(ctx);
      PromptHash p;
      p.fill(prompt);
      return store.exact_lookup(c, p, CacheClock::now()) != nullptr;
    }
    std::string metrics() const { return registry.render_prometheus(); }
  };

  static NewEntry entry(std::uint8_t ctx, std::uint8_t prompt, std::vector<std::string> tags = {}) {
    NewEntry e;
    e.context.fill(ctx);
    e.prompt_hash.fill(prompt);
    e.tenant = ctx == 1 ? "acme" : "globex";
    e.prompt = "prompt " + std::to_string(prompt);
    e.embedding = vec(prompt);
    e.response = "response " + std::to_string(prompt);
    e.total_tokens = prompt;
    e.tags = std::move(tags);
    return e;
  }

  std::vector<fs::path> files(std::string_view prefix) const {
    std::vector<fs::path> out;
    for (const auto& de : fs::directory_iterator(dir_)) {
      if (de.path().filename().string().starts_with(prefix)) out.push_back(de.path());
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  fs::path dir_;
  PersistenceConfig cfg_;
  Sha256Digest fp_{};
};

// ----------------------------------------------------------------- recovery

TEST_F(PersistenceTest, RestartRestoresEntriesAndReplaysDeletes) {
  {
    Node n(cfg_, fp_);
    EXPECT_TRUE(n.persistence.enabled());
    for (std::uint8_t i = 1; i <= 5; ++i) n.insert(entry(1, i, {"faq"}));
    n.insert(entry(2, 1));
    EXPECT_EQ(n.purge(PurgeFilter{.tag = "faq"}), 5u);
    n.insert(entry(1, 9));
  }
  Node n(cfg_, fp_);
  EXPECT_EQ(n.recovery.loaded, 2u);
  EXPECT_TRUE(n.has(2, 1));
  EXPECT_TRUE(n.has(1, 9));
  for (std::uint8_t i = 1; i <= 5; ++i) EXPECT_FALSE(n.has(1, i)) << "purged entries stay purged after a restart";
  ContextKey c;
  c.fill(1);
  PromptHash p;
  p.fill(9);
  auto e = n.store.exact_lookup(c, p, CacheClock::now());
  ASSERT_TRUE(e);
  EXPECT_EQ(e->response, "response 9");
  EXPECT_TRUE(e->embedding) << "same model fingerprint: vectors restored";
  EXPECT_TRUE(n.store.nearest(c, vec(9), CacheClock::now()).entry) << "and searchable";
}

TEST_F(PersistenceTest, SnapshotIsPointInTimeAndCompactsTheWal) {
  cfg_.max_wal_bytes = 1;  // every tick wants a snapshot
  {
    Node n(cfg_, fp_);
    for (std::uint8_t i = 1; i <= 20; ++i) n.insert(entry(1, i));
    n.persistence.wait_for_snapshot();
    n.insert(entry(1, 21));  // after the snapshot: only in the WAL
    n.persistence.wait_for_snapshot();
    EXPECT_TRUE(fs::exists(dir_ / "snapshot.bin"));
    EXPECT_NE(n.metrics().find("semcache_snapshots_total 0\n"), 0u);
  }
  EXPECT_LE(files("wal-").size(), 2u) << "segments covered by the snapshot are deleted";
  Node n(cfg_, fp_);
  EXPECT_GT(n.recovery.snapshot_entries, 0u);
  EXPECT_EQ(n.recovery.loaded, 21u);
  for (std::uint8_t i = 1; i <= 21; ++i) EXPECT_TRUE(n.has(1, i)) << int(i);
}

TEST_F(PersistenceTest, TornWalTailAtEveryOffsetKeepsThePrefix) {
  {
    Node n(cfg_, fp_);
    n.insert(entry(1, 1));
    n.insert(entry(1, 2));
  }
  const auto seg = files("wal-").back();
  const auto full = fs::file_size(seg);
  std::string bytes(full, '\0');
  std::ifstream(seg, std::ios::binary).read(bytes.data(), static_cast<std::streamsize>(full));

  // Both records have the same size, so record 1 ends halfway through the bytes after the header.
  constexpr std::size_t kHeader = 56;
  const std::size_t end_of_first = kHeader + (full - kHeader) / 2;
  std::size_t checked = 0;
  for (std::size_t cut = full - 1; cut > end_of_first; cut -= 7) {  // every 7th byte of record 2
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    std::ofstream(seg, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(cut));
    Node n(cfg_, fp_);
    EXPECT_TRUE(n.recovery.wal_truncated) << cut;
    EXPECT_TRUE(n.has(1, 1)) << "complete records before the tear survive, cut=" << cut;
    EXPECT_FALSE(n.has(1, 2)) << "the torn record is dropped, cut=" << cut;
    ++checked;
  }
  EXPECT_GT(checked, 10u);
}

TEST_F(PersistenceTest, BitFlipStopsReplayAtTheDamagedRecord) {
  {
    Node n(cfg_, fp_);
    for (std::uint8_t i = 1; i <= 3; ++i) n.insert(entry(1, i));
  }
  const auto seg = files("wal-").back();
  std::fstream f(seg, std::ios::binary | std::ios::in | std::ios::out);
  f.seekp(static_cast<std::streamoff>(fs::file_size(seg) - 10));  // inside the last record
  f.put('\xff');
  f.close();
  Node n(cfg_, fp_);
  EXPECT_TRUE(n.has(1, 1));
  EXPECT_TRUE(n.has(1, 2));
  EXPECT_FALSE(n.has(1, 3)) << "CRC mismatch: record rejected";
  EXPECT_NE(n.metrics().find(R"(semcache_persistence_recovery_total{result="wal_truncated"} 1)"), std::string::npos);
}

TEST_F(PersistenceTest, CorruptSnapshotIsSetAsideAndStartupContinues) {
  cfg_.max_wal_bytes = 1;
  {
    Node n(cfg_, fp_);
    for (std::uint8_t i = 1; i <= 5; ++i) n.insert(entry(1, i));
    n.persistence.wait_for_snapshot();
  }
  ASSERT_TRUE(fs::exists(dir_ / "snapshot.bin"));
  {
    std::fstream f(dir_ / "snapshot.bin", std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(100);
    f.put('\x42');
  }
  Node n(cfg_, fp_);
  EXPECT_TRUE(n.recovery.snapshot_corrupt);
  EXPECT_TRUE(n.persistence.enabled()) << "a bad file never stops the cache";
  EXPECT_FALSE(files("snapshot.bin.corrupt-").empty()) << "kept for inspection";
}

TEST_F(PersistenceTest, DamageBeforeTheLastSegmentStopsReplay) {
  // No snapshots: each run leaves its own segment behind.
  {
    Node n(cfg_, fp_);
    n.insert(entry(1, 1));
  }
  {
    Node n(cfg_, fp_);
    n.insert(entry(1, 2));
  }
  auto segs = files("wal-");
  ASSERT_GE(segs.size(), 2u);
  {
    std::fstream f(segs.front(), std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0);  // the first segment's magic
    f.put('X');
  }
  Node n(cfg_, fp_);
  EXPECT_TRUE(n.recovery.wal_stopped_early);
  EXPECT_FALSE(n.has(1, 2)) << "later segments are not replayed past the damage";
  EXPECT_FALSE(files("wal-0").empty());
  EXPECT_NE(n.metrics().find(R"(result="wal_corrupt")"), std::string::npos);
  std::size_t set_aside = 0;
  for (const auto& de : fs::directory_iterator(dir_)) set_aside += de.path().string().find(".corrupt-") != std::string::npos;
  EXPECT_GE(set_aside, 2u) << "the damaged segment and the ones after it are kept for inspection";
}

TEST_F(PersistenceTest, DifferentModelDropsVectorsButKeepsExactEntries) {
  {
    Node n(cfg_, fp_);
    n.insert(entry(1, 1));
  }
  Sha256Digest other{};
  other.fill(0xAB);
  Node n(cfg_, other);
  EXPECT_TRUE(n.recovery.fingerprint_mismatch);
  EXPECT_TRUE(n.has(1, 1)) << "exact match still works";
  ContextKey c;
  c.fill(1);
  EXPECT_FALSE(n.store.nearest(c, vec(1), CacheClock::now()).entry) << "vectors from another model are not searched";
}

TEST_F(PersistenceTest, ExpiredEntriesAreNotLoaded) {
  {
    Node n(cfg_, fp_);
    auto e = entry(1, 1);
    e.created = CacheClock::now() - 2h;
    e.expires = CacheClock::now() + 100ms;
    n.insert(std::move(e));
    n.insert(entry(1, 2));
  }
  std::this_thread::sleep_for(150ms);
  Node n(cfg_, fp_);
  EXPECT_FALSE(n.has(1, 1));
  EXPECT_TRUE(n.has(1, 2));
  EXPECT_GE(n.recovery.skipped, 1u);
}

TEST_F(PersistenceTest, DiskErrorFallsBackToMemoryThenRecovers) {
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  cfg_.max_wal_bytes = 1;
  cfg_.retry_interval = 10ms;
  Node n(cfg_, fp_);
  n.insert(entry(1, 1));
  n.persistence.wait_for_snapshot();
  ASSERT_EQ(::chmod(dir_.c_str(), 0555), 0);  // read-only: the next segment/snapshot cannot be created
  n.insert(entry(1, 2));
  n.persistence.wait_for_snapshot();
  n.insert(entry(1, 3));  // a tick after the failed snapshot disables persistence
  EXPECT_FALSE(n.persistence.enabled());
  EXPECT_TRUE(n.has(1, 3)) << "the cache keeps serving from memory";
  EXPECT_NE(n.metrics().find("semcache_persistence_enabled 0\n"), std::string::npos);

  ASSERT_EQ(::chmod(dir_.c_str(), 0755), 0);
  std::this_thread::sleep_for(20ms);
  n.insert(entry(1, 4));  // the next tick retries
  EXPECT_TRUE(n.persistence.enabled());
  n.persistence.wait_for_snapshot();
  EXPECT_TRUE(fs::exists(dir_ / "snapshot.bin")) << "re-enabling writes a full snapshot";
}

TEST_F(PersistenceTest, PurgeAlsoBlocksInsertsFromRequestsThatStartedBefore) {
  Node n(cfg_, fp_);
  const auto before = CacheClock::now() - 1s;
  EXPECT_EQ(n.purge(PurgeFilter{.tag = "faq"}), 0u);
  auto late = entry(1, 1, {"faq"});
  late.request_started = before;  // was in flight when the purge ran
  n.insert(std::move(late));
  EXPECT_FALSE(n.has(1, 1)) << "a response computed before the purge must not come back";
  auto fresh = entry(1, 2, {"faq"});
  fresh.request_started = CacheClock::now();
  n.insert(std::move(fresh));
  EXPECT_TRUE(n.has(1, 2));
  auto untagged = entry(1, 3);
  untagged.request_started = before;
  n.insert(std::move(untagged));
  EXPECT_TRUE(n.has(1, 3)) << "the guard only applies to entries the purge would have matched";
}

TEST_F(PersistenceTest, PurgeFiltersCombineAndEmptyFilterPurgesNothing) {
  Node n(cfg_, fp_);
  n.insert(entry(1, 1, {"a"}));
  n.insert(entry(1, 2, {"b"}));
  n.insert(entry(2, 3, {"a"}));
  EXPECT_EQ(n.purge(PurgeFilter{}), 0u);
  EXPECT_EQ(n.purge(PurgeFilter{.tenant = "acme", .tag = "a"}), 1u);
  EXPECT_FALSE(n.has(1, 1));
  EXPECT_TRUE(n.has(1, 2));
  EXPECT_TRUE(n.has(2, 3));
  ContextKey c;
  c.fill(2);
  EXPECT_EQ(n.purge(PurgeFilter{.context = c}), 1u);
  EXPECT_FALSE(n.has(2, 3));
}

}  // namespace
}  // namespace semcache
