// Embedding latency/throughput (Phase 2). Run via bench/run_embed_bench.sh on a Release build;
// results are recorded under bench/results/.

#include <benchmark/benchmark.h>

#include <memory>
#include <string>

#include "embed/onnx_embedder.hpp"

namespace semcache {
namespace {

EmbedderConfig bench_config() {
  EmbedderConfig c;
  c.model_path = std::string(SEMCACHE_MODEL_DIR) + "/all-MiniLM-L6-v2.onnx";
  c.vocab_path = std::string(SEMCACHE_MODEL_DIR) + "/vocab.txt";
  return c;
}

OnnxEmbedder& embedder() {
  static auto e = std::make_unique<OnnxEmbedder>(bench_config());
  return *e;
}

// An FAQ-style prompt padded with natural words to exactly `tokens` tokens (including [CLS]/[SEP]).
std::string text_with_tokens(std::size_t tokens) {
  static constexpr const char* kWords[] = {"how", "do", "i", "get", "a", "refund", "for", "my", "order", "if",
                                           "the", "item", "arrived", "damaged", "and", "late", "please", "explain",
                                           "your", "policy", "in", "detail"};
  std::string s;
  for (std::size_t i = 0; embedder().tokenizer().encode(s).size() < tokens; ++i) {
    if (!s.empty()) s.push_back(' ');
    s += kWords[i % std::size(kWords)];
  }
  return s;
}

void BM_Tokenize(benchmark::State& state) {
  const auto text = text_with_tokens(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(embedder().tokenizer().encode(text));
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * text.size()));
}
BENCHMARK(BM_Tokenize)->Arg(16)->Arg(64)->Arg(256);

void BM_EmbedSingle(benchmark::State& state) {
  const auto text = text_with_tokens(static_cast<std::size_t>(state.range(0)));
  std::string_view in[] = {text};
  for (auto _ : state) {
    auto r = embedder().embed(in);
    if (r.error != EmbedError::none) state.SkipWithError(r.detail.c_str());
    benchmark::DoNotOptimize(r);
  }
  state.counters["embeds/s"] = benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_EmbedSingle)->Arg(16)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Unit(benchmark::kMillisecond)->UseRealTime();

void BM_EmbedBatch(benchmark::State& state) {
  const auto text = text_with_tokens(32);
  std::vector<std::string_view> in(static_cast<std::size_t>(state.range(0)), text);
  for (auto _ : state) {
    auto r = embedder().embed(in);
    benchmark::DoNotOptimize(r);
  }
  state.counters["embeds/s"] =
      benchmark::Counter(static_cast<double>(state.iterations() * state.range(0)), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_EmbedBatch)->Arg(1)->Arg(4)->Arg(16)->Unit(benchmark::kMillisecond)->UseRealTime();

// Several threads calling embed() on one shared session: the embed-pool shape (ADR-005).
void BM_EmbedConcurrent(benchmark::State& state) {
  const auto text = text_with_tokens(32);
  std::string_view in[] = {text};
  for (auto _ : state) {
    auto r = embedder().embed(in);
    benchmark::DoNotOptimize(r);
  }
  state.counters["embeds/s"] = benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_EmbedConcurrent)->ThreadRange(1, 8)->Unit(benchmark::kMillisecond)->UseRealTime();

}  // namespace
}  // namespace semcache

BENCHMARK_MAIN();
