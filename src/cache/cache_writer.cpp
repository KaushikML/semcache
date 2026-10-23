#include "cache/cache_writer.hpp"

#include <pthread.h>

#include "log/log.hpp"

namespace semcache {

CacheWriter::CacheWriter(CacheStore& store, std::size_t capacity, MetricsRegistry& m, WriterHook* hook)
    : store_(store),
      hook_(hook),
      capacity_(capacity),
      dropped_(m.counter("semcache_store_dropped_total", "Inserts dropped because the writer queue was full")),
      depth_(m.gauge("semcache_writer_queue_depth", "Mutations waiting for the writer thread")),
      thread_([this] { run(); }) {
  pthread_setname_np(thread_.native_handle(), "cache-writer");
}

CacheWriter::~CacheWriter() { stop(); }

bool CacheWriter::submit_insert(NewEntry e, AfterInsert after) {
  {
    std::lock_guard lock(mu_);
    if (stopping_ || queue_.size() >= capacity_) {
      dropped_.inc();
      return false;
    }
    queue_.emplace_back(Insert{std::move(e), std::move(after)});
    ++submitted_;
    depth_.set(static_cast<std::int64_t>(queue_.size()));
  }
  cv_.notify_one();
  return true;
}

void CacheWriter::submit_expire() {
  {
    std::lock_guard lock(mu_);
    if (stopping_ || expire_queued_) return;
    expire_queued_ = true;
    queue_.emplace_back(Expire{});
    ++submitted_;
  }
  cv_.notify_one();
}

bool CacheWriter::submit_purge(PurgeFilter filter, PurgeDone done) {
  {
    std::lock_guard lock(mu_);
    if (stopping_ || queue_.size() >= capacity_) return false;
    queue_.emplace_back(Purge{std::move(filter), std::move(done)});
    ++submitted_;
  }
  cv_.notify_one();
  return true;
}

void CacheWriter::flush() {
  std::unique_lock lock(mu_);
  const auto target = submitted_;
  done_cv_.wait(lock, [&] { return applied_ >= target || exited_; });
}

void CacheWriter::stop() {
  {
    std::lock_guard lock(mu_);
    if (stopping_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void CacheWriter::run() {
  for (;;) {
    std::optional<Job> job;
    bool exiting = false;
    {
      std::unique_lock lock(mu_);
      auto ready = [this] { return stopping_ || !queue_.empty(); };
      if (hook_ != nullptr) {
        cv_.wait_until(lock, hook_->next_deadline(), ready);
      } else {
        cv_.wait(lock, ready);
      }
      if (!queue_.empty()) {
        job = std::move(queue_.front());
        queue_.pop_front();
        if (std::holds_alternative<Expire>(*job)) expire_queued_ = false;
        depth_.set(static_cast<std::int64_t>(queue_.size()));
      } else {
        exiting = stopping_;  // stopping, and everything queued was applied
      }
    }
    if (exiting) {
      if (hook_ != nullptr) hook_->close();  // final WAL sync
      std::lock_guard lock(mu_);
      exited_ = true;
      done_cv_.notify_all();
      return;
    }
    // No lock held: the store takes its own (W -> L2 -> L3); the hook does its I/O here too.
    if (job) apply(*job);
    if (hook_ != nullptr) {
      try {
        hook_->tick(store_, WriterHook::Clock::now());
      } catch (const std::exception& e) {
        log::error("cache writer: persistence tick failed", {{"error", e.what()}});
      }
    }
    if (job) {
      {
        std::lock_guard lock(mu_);
        ++applied_;
      }
      done_cv_.notify_all();
    }
  }
}

void CacheWriter::apply(Job& job) {
  if (auto* ins = std::get_if<Insert>(&job)) {
    auto outcome = InsertOutcome::failed;
    try {
      outcome = store_.insert(std::move(ins->entry), CacheClock::now());
    } catch (const std::exception& e) {
      // A failed mutation loses one cache entry, never a request (fail open).
      log::error("cache writer: insert failed", {{"error", e.what()}});
    }
    if (ins->after) ins->after(outcome);  // always: callers release state waiting on it
  } else if (auto* purge = std::get_if<Purge>(&job)) {
    std::size_t n = 0;
    try {
      n = store_.purge(purge->filter, CacheClock::now());
    } catch (const std::exception& e) {
      log::error("cache writer: purge failed", {{"error", e.what()}});
    }
    if (purge->done) purge->done(n);
  } else {
    try {
      if (auto n = store_.expire(CacheClock::now()); n > 0) log::info("expired cache entries", {{"count", n}});
    } catch (const std::exception& e) {
      log::error("cache writer: expiry failed", {{"error", e.what()}});
    }
  }
}

}  // namespace semcache
