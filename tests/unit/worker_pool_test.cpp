#include "common/worker_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <latch>
#include <stdexcept>

namespace semcache {
namespace {

template <class F>
auto run_on_ioc(F f) {
  net::io_context ioc;
  auto fut = net::co_spawn(ioc, f(), net::use_future);
  ioc.run();
  return fut.get();
}

TEST(WorkerPool, RunsOnPoolThreadAndResumesOnCaller) {
  WorkerPool pool("test", 2, 8);
  const auto caller = std::this_thread::get_id();
  auto [status, worker, resumed] = run_on_ioc([&]() -> Task<std::tuple<PoolStatus, std::thread::id, std::thread::id>> {
    auto r = co_await pool.run<std::thread::id>([] { return std::this_thread::get_id(); });
    co_return std::make_tuple(r.status, *r.value, std::this_thread::get_id());
  });
  EXPECT_EQ(status, PoolStatus::ok);
  EXPECT_NE(worker, caller);
  EXPECT_EQ(resumed, caller);
}

TEST(WorkerPool, ExceptionsBecomeFailed) {
  WorkerPool pool("test", 1, 8);
  auto status = run_on_ioc([&]() -> Task<PoolStatus> {
    auto r = co_await pool.run<int>([]() -> int { throw std::runtime_error("boom"); });
    co_return r.status;
  });
  EXPECT_EQ(status, PoolStatus::failed);
}

TEST(WorkerPool, FullQueueRejectsImmediately) {
  WorkerPool pool("test", 1, 1);
  std::latch started(1);
  std::latch release(1);
  net::io_context ioc;
  auto work = net::make_work_guard(ioc);
  std::thread runner([&] { ioc.run(); });
  auto submit = [&](std::function<int()> fn) {
    return net::co_spawn(ioc, [&pool, fn]() -> Task<PoolStatus> { co_return (co_await pool.run<int>(fn)).status; },
                         net::use_future);
  };

  auto busy = submit([&] {
    started.count_down();
    release.wait();
    return 0;
  });
  started.wait();  // the only worker is now blocked
  auto queued = submit([] { return 1; });
  while (pool.queued() < 1) std::this_thread::yield();  // fills the queue (capacity 1)
  auto rejected = submit([] { return 2; });
  EXPECT_EQ(rejected.get(), PoolStatus::rejected) << "rejected while the worker is still busy";

  release.count_down();
  EXPECT_EQ(busy.get(), PoolStatus::ok);
  EXPECT_EQ(queued.get(), PoolStatus::ok);
  work.reset();
  runner.join();
}

TEST(WorkerPool, StopRejectsQueuedJobsAndCompletesEveryCaller) {
  auto pool = std::make_unique<WorkerPool>("test", 1, 16);
  std::latch started(1);
  std::latch release(1);
  net::io_context ioc;
  std::atomic<int> ok{0};
  std::atomic<int> rejected{0};
  for (int i = 0; i < 5; ++i) {
    net::co_spawn(ioc, [&, i]() -> Task<void> {
      auto r = co_await pool->run<int>([&, i] {
        if (i == 0) {
          started.count_down();
          release.wait();
        }
        return i;
      });
      (r.status == PoolStatus::ok ? ok : rejected).fetch_add(1);
    }, net::detached);
  }
  std::thread runner([&] { ioc.run(); });
  started.wait();
  std::thread stopper([&] { pool->stop(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  release.count_down();
  stopper.join();
  runner.join();  // returns only once every coroutine resumed
  EXPECT_EQ(ok.load(), 1);
  EXPECT_EQ(rejected.load(), 4);
}

}  // namespace
}  // namespace semcache
