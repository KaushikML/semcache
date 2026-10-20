#include "coalesce/coalescer.hpp"

#include <gtest/gtest.h>

#include "support/recording_writer.hpp"

namespace semcache {
namespace {

using test::RecordingWriter;

FlightKey key(std::uint8_t p, bool stream = false) {
  FlightKey k;
  k.context.fill(1);
  k.prompt.fill(p);
  k.stream = stream;
  return k;
}

const HeaderList kLeaderHeaders = {{"X-Semcache", "MISS"}, {"X-Semcache-Latency-Ms", "5"}, {"x-request-id", "abc"}};
HeaderList own_headers() { return {{"X-Semcache", "COALESCED"}}; }

// Runs a coroutine to completion on a fresh io_context.
template <class T>
T run(net::io_context& ioc, Task<T> t) {
  auto f = net::co_spawn(ioc, std::move(t), net::use_future);
  ioc.run();
  ioc.restart();
  return f.get();
}

class CoalescerTest : public ::testing::Test {
 protected:
  MetricsRegistry registry_;
  Coalescer coalescer_{CoalescerConfig{}, 1 << 20, registry_};
};

TEST_F(CoalescerTest, FirstJoinLeadsOthersFollowUntilRetired) {
  auto a = coalescer_.join(key(1));
  auto b = coalescer_.join(key(1));
  auto c = coalescer_.join(key(2));
  EXPECT_TRUE(a.leader);
  EXPECT_FALSE(b.leader);
  EXPECT_EQ(a.flight, b.flight);
  EXPECT_TRUE(c.leader);
  EXPECT_TRUE(coalescer_.join(key(1, /*stream=*/true)).leader) << "stream mode is part of the key";
  coalescer_.retire(key(1), a.flight);
  EXPECT_TRUE(coalescer_.join(key(1)).leader);
}

TEST_F(CoalescerTest, RetireOnlyRemovesTheSameFlight) {
  auto a = coalescer_.join(key(1));
  coalescer_.retire(key(1), a.flight);
  auto b = coalescer_.join(key(1));
  coalescer_.retire(key(1), a.flight);  // stale retire must not drop the new flight
  EXPECT_FALSE(coalescer_.join(key(1)).leader);
  EXPECT_EQ(coalescer_.join(key(1)).flight, b.flight);
}

TEST_F(CoalescerTest, FollowerReplaysCompleteResponseWithOwnHeaders) {
  auto flight = coalescer_.join(key(1)).flight;
  RecordingWriter leader_client;
  LeaderWriter lw(leader_client, flight);
  net::io_context ioc;
  run(ioc, [&]() -> Task<bool> { co_return co_await lw.send(200, "application/json", R"({"a":1})", kLeaderHeaders); }());

  RecordingWriter follower;
  auto r = run(ioc, follow(flight, 10, follower, own_headers));
  EXPECT_TRUE(r.served);
  EXPECT_EQ(r.status, 200u);
  EXPECT_EQ(follower.body, R"({"a":1})");
  EXPECT_EQ(follower.header("X-Semcache"), "COALESCED");
  EXPECT_EQ(follower.header("x-request-id"), "abc") << "non-semcache headers are shared";
  EXPECT_EQ(follower.header("X-Semcache-Latency-Ms"), "") << "the leader's own headers are not";
  EXPECT_EQ(leader_client.body, R"({"a":1})");
}

TEST_F(CoalescerTest, StreamingFollowerReceivesChunksLiveThenEnd) {
  auto flight = coalescer_.join(key(1, true)).flight;
  RecordingWriter leader_client;
  LeaderWriter lw(leader_client, flight);
  net::io_context ioc;
  auto work = net::make_work_guard(ioc);
  RecordingWriter follower;
  auto fut = net::co_spawn(ioc, follow(flight, 10, follower, own_headers), net::use_future);
  std::thread io([&] { ioc.run(); });

  auto leader = [&]() -> Task<void> {
    co_await lw.begin_stream(200, "text/event-stream", kLeaderHeaders);
    co_await lw.write_chunk("data: 1\n\n");
    co_await lw.write_chunk("data: 2\n\n");
    co_await lw.end_stream();
  };
  net::io_context leader_ioc;
  net::co_spawn(leader_ioc, leader(), net::detached);
  leader_ioc.run();

  auto r = fut.get();
  work.reset();
  io.join();
  EXPECT_TRUE(r.served && r.streamed);
  EXPECT_TRUE(follower.complete);
  EXPECT_EQ(follower.joined(), "data: 1\n\ndata: 2\n\n");
  EXPECT_EQ(follower.header("X-Semcache"), "COALESCED");
}

TEST_F(CoalescerTest, LeaderGoneBeforeHeadMeansFollowerServesItself) {
  auto flight = coalescer_.join(key(1)).flight;
  flight->abort();
  RecordingWriter follower;
  net::io_context ioc;
  auto r = run(ioc, follow(flight, 10, follower, own_headers));
  EXPECT_FALSE(r.served) << "fail open: caller forwards the request itself";
  EXPECT_EQ(follower.status, 0u);
}

TEST_F(CoalescerTest, LeaderGoneMidStreamEndsFollowerWithErrorEvent) {
  auto flight = coalescer_.join(key(1, true)).flight;
  flight->record_begin(200, "text/event-stream", kLeaderHeaders);
  flight->record_chunk("data: 1\n\n");
  flight->abort();
  RecordingWriter follower;
  net::io_context ioc;
  auto r = run(ioc, follow(flight, 10, follower, own_headers));
  EXPECT_TRUE(r.served);
  EXPECT_FALSE(r.keep_alive);
  EXPECT_EQ(follower.chunks.front(), "data: 1\n\n");
  EXPECT_NE(follower.chunks.back().find("interrupted"), std::string::npos);
}

TEST_F(CoalescerTest, FollowerCapIsEnforced) {
  auto flight = coalescer_.join(key(1)).flight;
  net::io_context ioc;
  auto w1 = std::make_shared<Flight::Wakeup>(ioc.get_executor(), 1);
  auto w2 = std::make_shared<Flight::Wakeup>(ioc.get_executor(), 1);
  auto id = flight->add_follower(w1, 1);
  ASSERT_TRUE(id);
  EXPECT_FALSE(flight->add_follower(w2, 1));
  EXPECT_EQ(flight->followers(), 1u);
  flight->remove_follower(*id);
  EXPECT_EQ(flight->followers(), 0u);
}

TEST_F(CoalescerTest, LeaderWriterKeepsGoingForFollowersAfterClientLeaves) {
  auto flight = coalescer_.join(key(1, true)).flight;
  RecordingWriter gone;
  gone.ok = false;
  LeaderWriter lw(gone, flight);
  net::io_context ioc;
  EXPECT_FALSE(run(ioc, lw.begin_stream(200, "text/event-stream", kLeaderHeaders))) << "no followers: stop";
  auto w = std::make_shared<Flight::Wakeup>(ioc.get_executor(), 1);
  auto id = flight->add_follower(w, 10);
  EXPECT_TRUE(run(ioc, lw.write_chunk("data: x\n\n"))) << "a follower still needs the rest";
  EXPECT_EQ(flight->read(0).chunks.size(), 1u) << "recorded even though the leader's client is gone";
  flight->remove_follower(*id);
}

TEST_F(CoalescerTest, StreamLogIsBounded) {
  Coalescer small(CoalescerConfig{}, 10, registry_);
  auto flight = small.join(key(1, true)).flight;
  flight->record_begin(200, "text/event-stream", {});
  flight->record_chunk("0123456789ABC");
  EXPECT_EQ(flight->read(0).state, Flight::State::aborted);
}

}  // namespace
}  // namespace semcache
