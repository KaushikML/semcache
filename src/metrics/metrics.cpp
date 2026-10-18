#include "metrics/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace semcache {

Histogram::Histogram(std::vector<double> upper_bounds)
    : bounds_(std::move(upper_bounds)),
      buckets_(std::make_unique<std::atomic<std::uint64_t>[]>(bounds_.size() + 1)) {
  if (!std::is_sorted(bounds_.begin(), bounds_.end())) {
    throw std::invalid_argument("histogram bounds must be ascending");
  }
}

void Histogram::observe(double v) noexcept {
  auto it = std::lower_bound(bounds_.begin(), bounds_.end(), v);
  auto idx = static_cast<std::size_t>(it - bounds_.begin());
  buckets_[idx].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_.fetch_add(v, std::memory_order_relaxed);
}

Histogram::Snapshot Histogram::snapshot() const {
  Snapshot s;
  s.upper_bounds = bounds_;
  s.counts.resize(bounds_.size() + 1);
  for (std::size_t i = 0; i < s.counts.size(); ++i) {
    s.counts[i] = buckets_[i].load(std::memory_order_relaxed);
    s.count += s.counts[i];  // derived from buckets so the snapshot is self-consistent
  }
  s.sum = sum_.load(std::memory_order_relaxed);
  return s;
}

double Histogram::Snapshot::quantile(double q) const {
  if (count == 0) return 0;
  const double rank = q * static_cast<double>(count);
  double seen = 0;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    const auto c = static_cast<double>(counts[i]);
    if (seen + c >= rank && c > 0) {
      const double lo = i == 0 ? 0.0 : upper_bounds[i - 1];
      if (i == upper_bounds.size()) return lo;  // +Inf bucket: report its lower edge
      const double hi = upper_bounds[i];
      return lo + (hi - lo) * ((rank - seen) / c);
    }
    seen += c;
  }
  return upper_bounds.empty() ? 0 : upper_bounds.back();
}

std::vector<double> latency_buckets_seconds() {
  return {0.0001, 0.00025, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.025, 0.05,
          0.1,    0.25,    0.5,    1.0,   2.5,   5.0,   10.0, 30.0,  60.0};
}

MetricsRegistry::Family& MetricsRegistry::family(const std::string& name, const std::string& help,
                                                 Kind kind) {
  for (auto& f : families_) {
    if (f.name == name) {
      if (f.kind != kind) throw std::logic_error("metric " + name + " registered with two types");
      return f;
    }
  }
  return families_.emplace_back(Family{name, help, kind, {}});
}

MetricsRegistry::Series& MetricsRegistry::series(Family& f, const std::string& labels) {
  for (auto& s : f.series) {
    if (s.labels == labels) return s;
  }
  auto& s = f.series.emplace_back();
  s.labels = labels;
  return s;
}

Counter& MetricsRegistry::counter(const std::string& name, const std::string& help,
                                  const std::string& labels) {
  std::lock_guard lock(mu_);
  auto& s = series(family(name, help, Kind::counter), labels);
  if (!s.counter) s.counter = std::make_unique<Counter>();
  return *s.counter;
}

Gauge& MetricsRegistry::gauge(const std::string& name, const std::string& help,
                              const std::string& labels) {
  std::lock_guard lock(mu_);
  auto& s = series(family(name, help, Kind::gauge), labels);
  if (!s.gauge) s.gauge = std::make_unique<Gauge>();
  return *s.gauge;
}

Histogram& MetricsRegistry::histogram(const std::string& name, const std::string& help,
                                      std::vector<double> bounds, const std::string& labels) {
  std::lock_guard lock(mu_);
  auto& s = series(family(name, help, Kind::histogram), labels);
  if (!s.histogram) s.histogram = std::make_unique<Histogram>(std::move(bounds));
  return *s.histogram;
}

namespace {

std::string with_labels(const std::string& name, const std::string& labels,
                        const std::string& extra = "") {
  std::string all = labels;
  if (!extra.empty()) all = all.empty() ? extra : all + "," + extra;
  return all.empty() ? name : name + "{" + all + "}";
}

std::string fmt_double(double v) {
  if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
  std::ostringstream os;
  os.precision(17);
  os << v;
  return os.str();
}

}  // namespace

std::string MetricsRegistry::render_prometheus() const {
  std::lock_guard lock(mu_);
  std::ostringstream out;
  for (const auto& f : families_) {
    const char* type = f.kind == Kind::counter ? "counter" : f.kind == Kind::gauge ? "gauge" : "histogram";
    out << "# HELP " << f.name << ' ' << f.help << '\n' << "# TYPE " << f.name << ' ' << type << '\n';
    for (const auto& s : f.series) {
      if (s.counter) out << with_labels(f.name, s.labels) << ' ' << s.counter->value() << '\n';
      if (s.gauge) out << with_labels(f.name, s.labels) << ' ' << s.gauge->value() << '\n';
      if (s.histogram) {
        auto snap = s.histogram->snapshot();
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < snap.counts.size(); ++i) {
          cumulative += snap.counts[i];
          std::string le = i < snap.upper_bounds.size() ? fmt_double(snap.upper_bounds[i]) : "+Inf";
          out << with_labels(f.name + "_bucket", s.labels, "le=\"" + le + "\"") << ' ' << cumulative << '\n';
        }
        out << with_labels(f.name + "_sum", s.labels) << ' ' << fmt_double(snap.sum) << '\n';
        out << with_labels(f.name + "_count", s.labels) << ' ' << snap.count << '\n';
      }
    }
  }
  return out.str();
}

}  // namespace semcache
