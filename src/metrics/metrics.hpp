#pragma once

// Lock-free metric primitives plus a registry that renders Prometheus text format.
// Registration happens at startup; the hot path only touches atomics.

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace semcache {

class Counter {
 public:
  void inc(std::uint64_t n = 1) noexcept { value_.fetch_add(n, std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> value_{0};
};

class Gauge {
 public:
  void add(std::int64_t n) noexcept { value_.fetch_add(n, std::memory_order_relaxed); }
  void set(std::int64_t n) noexcept { value_.store(n, std::memory_order_relaxed); }
  [[nodiscard]] std::int64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::int64_t> value_{0};
};

// Fixed-bucket histogram. Bounds are upper bounds (inclusive), ascending; +Inf is implicit.
class Histogram {
 public:
  explicit Histogram(std::vector<double> upper_bounds);

  void observe(double v) noexcept;

  struct Snapshot {
    std::vector<double> upper_bounds;
    std::vector<std::uint64_t> counts;  // per bucket, non-cumulative; last entry is +Inf
    std::uint64_t count = 0;
    double sum = 0;
    // Linear interpolation inside the bucket that contains quantile q (0..1).
    [[nodiscard]] double quantile(double q) const;
  };
  [[nodiscard]] Snapshot snapshot() const;

 private:
  std::vector<double> bounds_;
  std::unique_ptr<std::atomic<std::uint64_t>[]> buckets_;  // bounds_.size() + 1
  std::atomic<std::uint64_t> count_{0};
  std::atomic<double> sum_{0};
};

// Latency buckets in seconds, 100us .. 60s. Covers both the sub-ms lookup budget and multi-second
// upstream calls (design §3).
std::vector<double> latency_buckets_seconds();

class MetricsRegistry {
 public:
  // `labels` is the rendered label set without braces, e.g. R"(result="HIT")".
  Counter& counter(const std::string& name, const std::string& help, const std::string& labels = "");
  Gauge& gauge(const std::string& name, const std::string& help, const std::string& labels = "");
  Histogram& histogram(const std::string& name, const std::string& help, std::vector<double> bounds,
                       const std::string& labels = "");

  [[nodiscard]] std::string render_prometheus() const;

 private:
  enum class Kind { counter, gauge, histogram };
  struct Series {
    std::string labels;
    std::unique_ptr<Counter> counter;
    std::unique_ptr<Gauge> gauge;
    std::unique_ptr<Histogram> histogram;
  };
  struct Family {
    std::string name;
    std::string help;
    Kind kind;
    std::deque<Series> series;
  };
  Family& family(const std::string& name, const std::string& help, Kind kind);
  Series& series(Family& f, const std::string& labels);

  mutable std::mutex mu_;
  std::deque<Family> families_;
};

}  // namespace semcache
