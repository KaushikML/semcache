#include "common/worker_pool.hpp"

#include <pthread.h>

namespace semcache {

WorkerPool::WorkerPool(std::string name, int threads, std::size_t capacity) : capacity_(capacity) {
  threads_.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    threads_.emplace_back([this] { worker_loop(); });
    // Linux limits thread names to 15 chars; shows up in top/gdb/perf.
    auto label = (name + "-" + std::to_string(i)).substr(0, 15);
    pthread_setname_np(threads_.back().native_handle(), label.c_str());
  }
}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::stop() {
  {
    std::lock_guard lock(mu_);
    if (stopping_ && threads_.empty()) return;
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) t.join();
  threads_.clear();
  std::deque<Job> leftover;
  {
    std::lock_guard lock(mu_);
    leftover.swap(queue_);
  }
  for (auto& job : leftover) job(false);
}

bool WorkerPool::try_push(Job& job) {
  {
    std::lock_guard lock(mu_);
    if (stopping_ || queue_.size() >= capacity_) return false;
    queue_.push_back(std::move(job));
  }
  cv_.notify_one();
  return true;
}

std::size_t WorkerPool::queued() const {
  std::lock_guard lock(mu_);
  return queue_.size();
}

void WorkerPool::worker_loop() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;  // stop() rejects whatever is left
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    job(true);  // no lock held while working
  }
}

}  // namespace semcache
