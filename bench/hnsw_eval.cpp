// Brute force vs HNSW on one partition (Phase 5): recall@10, search latency percentiles, build
// time and memory as the partition grows. Picks index.hnsw_min_partition_size from data.
//
// Data: 384-d unit vectors drawn around cluster centers (topics), which is closer to sentence
// embeddings than uniform random vectors. Queries are fresh draws from the same distribution.
// Usage: semcache_hnsw_eval [--sizes 1000,10000,50000] [--queries 500] [--out results.json]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "embed/embedder.hpp"
#include "index/brute_force_index.hpp"
#include "index/hnsw_index.hpp"

namespace {

using namespace semcache;
using Clock = std::chrono::steady_clock;
using Vec = std::vector<float>;

constexpr std::size_t kDim = kEmbeddingDim;
constexpr std::size_t kK = 10;
constexpr std::size_t kPointsPerCluster = 50;  // ~topic size
constexpr float kSpread = 0.05f;               // per-dim noise: cosine to own center ~0.7

void normalize(Vec& v) {
  float n = 0;
  for (float x : v) n += x * x;
  for (auto& x : v) x /= std::sqrt(n);
}

std::vector<Vec> centers(std::size_t n, std::mt19937& rng) {
  std::normal_distribution<float> g;
  std::vector<Vec> out(n, Vec(kDim));
  for (auto& c : out) {
    for (auto& x : c) x = g(rng);
    normalize(c);
  }
  return out;
}

std::vector<Vec> draw(const std::vector<Vec>& cs, std::size_t n, std::mt19937& rng) {
  std::normal_distribution<float> g;
  std::uniform_int_distribution<std::size_t> pick(0, cs.size() - 1);
  std::vector<Vec> out(n, Vec(kDim));
  for (auto& v : out) {
    const auto& c = cs[pick(rng)];
    for (std::size_t d = 0; d < kDim; ++d) v[d] = c[d] + kSpread * g(rng);
    normalize(v);
  }
  return out;
}

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Latency {
  double p50_us = 0;
  double p99_us = 0;
};

template <class F>
Latency time_queries(const std::vector<Vec>& queries, F&& search) {
  std::vector<double> us;
  us.reserve(queries.size());
  for (const auto& q : queries) {
    const auto t = Clock::now();
    search(q);
    us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t).count());
  }
  std::sort(us.begin(), us.end());
  auto pct = [&](double p) { return us[std::min(us.size() - 1, static_cast<std::size_t>(p * static_cast<double>(us.size())))]; };
  return {pct(0.50), pct(0.99)};
}

std::vector<std::size_t> parse_sizes(const std::string& s) {
  std::vector<std::size_t> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, ',');) out.push_back(std::stoul(item));
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::size_t> sizes = {1000, 5000, 10000, 20000, 50000};
  std::size_t n_queries = 500;
  std::string out_path;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--sizes") sizes = parse_sizes(argv[i + 1]);
    else if (a == "--queries") n_queries = std::stoul(argv[i + 1]);
    else if (a == "--out") out_path = argv[i + 1];
  }
  const std::vector<std::size_t> efs = {16, 32, 64, 128, 256};
  const HnswIndex::Params params{};  // the production defaults: M=16, efC=200

  nlohmann::json results = nlohmann::json::array();
  std::printf("%8s %10s %10s | %9s %9s %9s | %4s %8s %9s %9s\n", "n", "bf_p50us", "bf_p99us", "build_s", "mem_MB",
              "bf_MB", "ef", "recall", "p50us", "p99us");
  for (std::size_t n : sizes) {
    std::mt19937 rng(static_cast<std::uint32_t>(n));
    const auto cs = centers(std::max<std::size_t>(1, n / kPointsPerCluster), rng);
    const auto data = draw(cs, n, rng);
    const auto queries = draw(cs, n_queries, rng);

    BruteForceIndex bf(kDim);
    for (VectorId i = 0; i < n; ++i) bf.add(i, data[i]);
    std::vector<std::set<VectorId>> truth;
    for (const auto& q : queries) {
      std::set<VectorId> t;
      for (const auto& h : bf.search(q, kK)) t.insert(h.id);
      truth.push_back(std::move(t));
    }
    const auto bf_lat = time_queries(queries, [&](const Vec& q) { (void)bf.search(q, kK); });

    HnswIndex hnsw(kDim, params);
    const auto t0 = Clock::now();
    for (VectorId i = 0; i < n; ++i) hnsw.add(i, data[i]);
    const double build_s = ms_since(t0) / 1000.0;

    nlohmann::json row = {{"n", n},
                          {"bf_p50_us", bf_lat.p50_us},
                          {"bf_p99_us", bf_lat.p99_us},
                          {"bf_bytes", bf.memory_bytes()},
                          {"hnsw_build_s", build_s},
                          {"hnsw_bytes", hnsw.memory_bytes()},
                          {"hnsw", nlohmann::json::array()}};
    for (std::size_t ef : efs) {
      hnsw.set_ef_search(ef);
      std::size_t found = 0;
      for (std::size_t qi = 0; qi < queries.size(); ++qi) {
        for (const auto& h : hnsw.search(queries[qi], kK)) found += truth[qi].count(h.id);
      }
      const double recall = static_cast<double>(found) / static_cast<double>(queries.size() * kK);
      const auto lat = time_queries(queries, [&](const Vec& q) { (void)hnsw.search(q, kK); });
      row["hnsw"].push_back({{"ef", ef}, {"recall_at_10", recall}, {"p50_us", lat.p50_us}, {"p99_us", lat.p99_us}});
      std::printf("%8zu %10.1f %10.1f | %9.2f %9.1f %9.1f | %4zu %8.4f %9.1f %9.1f\n", n, bf_lat.p50_us, bf_lat.p99_us,
                  build_s, static_cast<double>(hnsw.memory_bytes()) / 1e6, static_cast<double>(bf.memory_bytes()) / 1e6,
                  ef, recall, lat.p50_us, lat.p99_us);
    }
    std::fflush(stdout);
    results.push_back(row);
  }
  if (!out_path.empty()) {
    std::ofstream(out_path) << nlohmann::json{{"params", {{"m", params.m}, {"ef_construction", params.ef_construction}}},
                                              {"dim", kDim},
                                              {"k", kK},
                                              {"queries", n_queries},
                                              {"points_per_cluster", kPointsPerCluster},
                                              {"spread", kSpread},
                                              {"results", results}}
                                   .dump(2);
  }
  return 0;
}
