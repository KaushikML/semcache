#include <gtest/gtest.h>

#include "metrics/metrics.hpp"

namespace semcache {
namespace {

TEST(Metrics, CounterAndGauge) {
  MetricsRegistry r;
  auto& c = r.counter("c_total", "help");
  c.inc();
  c.inc(4);
  EXPECT_EQ(c.value(), 5u);
  EXPECT_EQ(&r.counter("c_total", "help"), &c) << "same name+labels returns the same series";
  auto& g = r.gauge("g", "help");
  g.add(3);
  g.add(-1);
  EXPECT_EQ(g.value(), 2);
}

TEST(Metrics, TypeConflictThrows) {
  MetricsRegistry r;
  (void)r.counter("x", "h");
  EXPECT_THROW((void)r.gauge("x", "h"), std::logic_error);
}

TEST(Metrics, HistogramBucketsAndQuantiles) {
  Histogram h({1, 2, 4});
  for (double v : {0.5, 1.0, 1.5, 3.0, 10.0}) h.observe(v);
  auto s = h.snapshot();
  ASSERT_EQ(s.counts.size(), 4u);
  EXPECT_EQ(s.counts[0], 2u);  // 0.5, 1.0 (upper bound inclusive)
  EXPECT_EQ(s.counts[1], 1u);
  EXPECT_EQ(s.counts[2], 1u);
  EXPECT_EQ(s.counts[3], 1u);  // +Inf
  EXPECT_EQ(s.count, 5u);
  EXPECT_DOUBLE_EQ(s.sum, 16.0);
  EXPECT_DOUBLE_EQ(s.quantile(0.4), 1.0);  // 2nd of 5 is the top of bucket [0,1]
  EXPECT_GT(s.quantile(0.6), 1.0);
  EXPECT_LE(s.quantile(0.6), 2.0);
}

TEST(Metrics, PrometheusRendering) {
  MetricsRegistry r;
  r.counter("req_total", "Requests", R"(result="HIT")").inc(3);
  r.counter("req_total", "Requests", R"(result="MISS")").inc();
  r.histogram("lat_seconds", "Latency", {0.1, 1}).observe(0.5);
  const auto text = r.render_prometheus();
  EXPECT_NE(text.find("# HELP req_total Requests\n# TYPE req_total counter\n"), std::string::npos);
  EXPECT_NE(text.find("req_total{result=\"HIT\"} 3\n"), std::string::npos);
  EXPECT_NE(text.find("req_total{result=\"MISS\"} 1\n"), std::string::npos);
  EXPECT_NE(text.find("# TYPE lat_seconds histogram"), std::string::npos);
  EXPECT_NE(text.find("lat_seconds_bucket{le=\"0.10000000000000001\"} 0\n"), std::string::npos);
  EXPECT_NE(text.find("lat_seconds_bucket{le=\"1\"} 1\n"), std::string::npos);
  EXPECT_NE(text.find("lat_seconds_bucket{le=\"+Inf\"} 1\n"), std::string::npos);
  EXPECT_NE(text.find("lat_seconds_count 1\n"), std::string::npos);
}

}  // namespace
}  // namespace semcache
