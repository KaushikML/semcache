#include "cache/cache_writer.hpp"

#include "log/log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <latch>

namespace semcache {
namespace {

NewEntry entry(std::uint8_t p, std::size_t response_bytes = 10) {
  NewEntry e;
  e.context.fill(1);
  e.prompt_hash.fill(p);
  e.tenant = "acme";
  e.prompt = "p";
  e.response = std::string(response_bytes, 'r');
  return e;
}

class CacheWriterTest : public ::testing::Test {
 protected:
  // The writer thread logs; initialize before it exists (see log.hpp).
  void SetUp() override { log::init("warn"); }
  MetricsRegistry registry_;
  CacheStore store_{CacheConfig{}, {}, registry_};
};

TEST_F(CacheWriterTest, InsertsAreAppliedAsynchronouslyInOrder) {
  CacheWriter w(store_, 16, registry_);
  std::vector<int> order;
  std::mutex mu;
  for (std::uint8_t i = 1; i <= 5; ++i) {
    ASSERT_TRUE(w.submit_insert(entry(i), [&, i](InsertOutcome o) {
      EXPECT_EQ(o, InsertOutcome::stored);
      std::lock_guard l(mu);
      order.push_back(i);
    }));
  }
  w.flush();
  EXPECT_EQ(store_.stats().entries, 5u);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4, 5}));
}

TEST_F(CacheWriterTest, CallbackRunsEvenWhenNotStored) {
  CacheConfig tiny;
  tiny.max_bytes = 100;
  CacheStore small(tiny, {}, registry_);
  CacheWriter w(small, 4, registry_);
  std::atomic<int> calls{0};
  w.submit_insert(entry(1, 10'000), [&](InsertOutcome o) {
    EXPECT_EQ(o, InsertOutcome::too_large);
    calls++;
  });
  w.flush();
  EXPECT_EQ(calls.load(), 1);
}

TEST_F(CacheWriterTest, FullQueueDropsAndCounts) {
  CacheWriter w(store_, 2, registry_);
  std::latch release(1);
  std::latch started(1);
  w.submit_insert(entry(1), [&](InsertOutcome) {  // blocks the writer thread
    started.count_down();
    release.wait();
  });
  started.wait();
  EXPECT_TRUE(w.submit_insert(entry(2)));
  EXPECT_TRUE(w.submit_insert(entry(3)));
  bool called = false;
  EXPECT_FALSE(w.submit_insert(entry(4), [&](InsertOutcome) { called = true; })) << "queue (2) is full";
  release.count_down();
  w.flush();
  EXPECT_FALSE(called) << "a dropped insert never calls back";
  EXPECT_EQ(store_.stats().entries, 3u);
  EXPECT_NE(registry_.render_prometheus().find("semcache_store_dropped_total 1\n"), std::string::npos);
}

TEST_F(CacheWriterTest, StopAppliesQueuedWorkThenRejects) {
  auto w = std::make_unique<CacheWriter>(store_, 16, registry_);
  for (std::uint8_t i = 1; i <= 8; ++i) w->submit_insert(entry(i));
  w->stop();
  EXPECT_EQ(store_.stats().entries, 8u);
  EXPECT_FALSE(w->submit_insert(entry(9)));
  w->flush();  // must not hang after stop
}

TEST_F(CacheWriterTest, ExpireRunsOnTheWriter) {
  CacheConfig cfg;
  cfg.default_ttl = std::chrono::seconds(1);
  CacheStore s(cfg, {}, registry_);
  s.insert(entry(1), CacheClock::now() - std::chrono::seconds(5));  // already expired
  CacheWriter w(s, 4, registry_);
  w.submit_expire();
  w.submit_expire();  // deduplicated while queued
  w.flush();
  EXPECT_EQ(s.stats().entries, 0u);
}

}  // namespace
}  // namespace semcache
