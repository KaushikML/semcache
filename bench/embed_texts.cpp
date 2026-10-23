// Embeds a JSONL file of {"text": ...} with the product's OnnxEmbedder (Phase 7), so evaluation
// numbers describe what semcache itself computes, not a Python reimplementation.
//
// Output: <out>.f32 = N x 384 float32 (little-endian, row per input line, zeros if not embedded)
//         <out>.json = {"count", "dim", "too_long": [line indexes over embedder.max_tokens], "seconds"}
// Usage: semcache_embed_texts --in texts.jsonl --out prefix [--threads 4]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "embed/onnx_embedder.hpp"

int main(int argc, char** argv) {
  using namespace semcache;
  std::string in_path;
  std::string out_prefix;
  int threads = 4;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--in") in_path = argv[i + 1];
    else if (a == "--out") out_prefix = argv[i + 1];
    else if (a == "--threads") threads = std::stoi(argv[i + 1]);
  }
  if (in_path.empty() || out_prefix.empty()) {
    std::fprintf(stderr, "usage: semcache_embed_texts --in texts.jsonl --out prefix [--threads N]\n");
    return 2;
  }
  std::vector<std::string> texts;
  std::ifstream in(in_path);
  for (std::string line; std::getline(in, line);) texts.push_back(nlohmann::json::parse(line).at("text").get<std::string>());

  EmbedderConfig cfg;
  cfg.model_path = std::string(SEMCACHE_MODEL_DIR) + "/all-MiniLM-L6-v2.onnx";
  cfg.vocab_path = std::string(SEMCACHE_MODEL_DIR) + "/vocab.txt";
  OnnxEmbedder embedder(cfg);

  std::vector<Embedding> out(texts.size(), Embedding{});
  std::vector<char> too_long(texts.size(), 0);
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> done{0};
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (std::size_t i; (i = next.fetch_add(1)) < texts.size();) {
        std::string_view one[] = {texts[i]};
        auto r = embedder.embed(one);
        if (r.error == EmbedError::none) {
          out[i] = r.embeddings[0];
        } else {
          too_long[i] = 1;
        }
        if (auto d = ++done; d % 2000 == 0) std::fprintf(stderr, "  %zu / %zu\n", d, texts.size());
      }
    });
  }
  for (auto& t : pool) t.join();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  std::ofstream f(out_prefix + ".f32", std::ios::binary);
  for (const auto& v : out) f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(sizeof(v)));
  nlohmann::json meta = {{"count", texts.size()}, {"dim", kEmbeddingDim}, {"seconds", secs}, {"threads", threads}};
  meta["too_long"] = nlohmann::json::array();
  for (std::size_t i = 0; i < texts.size(); ++i) {
    if (too_long[i]) meta["too_long"].push_back(i);
  }
  std::ofstream(out_prefix + ".json") << meta.dump(2);
  std::printf("embedded %zu texts in %.1f s (%.0f/s, %d threads), %zu not embedded (too long)\n", texts.size(), secs,
              static_cast<double>(texts.size()) / secs, threads, meta["too_long"].size());
  return 0;
}
