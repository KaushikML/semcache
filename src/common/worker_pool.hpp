#pragma once

// Bounded pool of CPU threads for blocking work (the embed pool, design §7.1). Coroutines on I/O
// threads `co_await pool.run(fn)`: fn runs on a pool thread and the coroutine resumes on its own
// executor. A full queue rejects immediately instead of queueing unbounded work; callers treat
// that as overload and fail open.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net/asio.hpp"

namespace semcache {

enum class PoolStatus {
  ok,
  rejected,  // queue full or pool stopping; fn did not run
  failed,    // fn threw
};

template <class R>
struct PoolResult {
  PoolStatus status = PoolStatus::rejected;
  std::optional<R> value;  // set when status == ok
};

class WorkerPool {
 public:
  WorkerPool(std::string name, int threads, std::size_t capacity);
  ~WorkerPool();
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Stops the threads; queued jobs complete as `rejected`. Idempotent.
  void stop();

  template <class R>
  Task<PoolResult<R>> run(std::function<R()> fn);

  [[nodiscard]] std::size_t queued() const;
  [[nodiscard]] std::size_t capacity() const { return capacity_; }

 private:
  using Job = std::function<void(bool run)>;  // run == false: reject without running

  bool try_push(Job& job);
  void worker_loop();

  const std::size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

template <class R>
Task<PoolResult<R>> WorkerPool::run(std::function<R()> fn) {
  auto initiate = [this](auto handler, std::function<R()> f) {
    using Handler = decltype(handler);
    // Keep the caller's io_context alive while the job is pending, and resume on its executor.
    auto ex = net::prefer(net::get_associated_executor(handler), net::execution::outstanding_work.tracked);
    auto h = std::make_shared<std::optional<Handler>>(std::move(handler));
    auto complete = [h, ex](PoolResult<R> r) {
      net::post(ex, [h, r = std::move(r)]() mutable {
        auto resume = std::move(**h);
        h->reset();
        std::move(resume)(std::move(r));
      });
    };
    Job job = [f = std::move(f), complete](bool run) mutable {
      if (!run) return complete(PoolResult<R>{PoolStatus::rejected, std::nullopt});
      PoolResult<R> r;
      try {
        r.value.emplace(f());
        r.status = PoolStatus::ok;
      } catch (...) {
        r.status = PoolStatus::failed;
      }
      complete(std::move(r));
    };
    if (!try_push(job)) complete(PoolResult<R>{PoolStatus::rejected, std::nullopt});
  };
  return net::async_initiate<decltype(net::use_awaitable), void(PoolResult<R>)>(std::move(initiate),
                                                                                net::use_awaitable, std::move(fn));
}

}  // namespace semcache
