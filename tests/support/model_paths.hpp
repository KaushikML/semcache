#pragma once

// Locations of the pinned model (scripts/fetch-model.sh) and committed golden files.

#include <string>

#include "config/config.hpp"

namespace semcache::test {

inline std::string data_dir() { return SEMCACHE_TEST_DATA_DIR; }
inline std::string vocab_path() { return std::string(SEMCACHE_MODEL_DIR) + "/vocab.txt"; }
inline std::string model_path() { return std::string(SEMCACHE_MODEL_DIR) + "/all-MiniLM-L6-v2.onnx"; }

inline EmbedderConfig embedder_config() {
  EmbedderConfig c;
  c.model_path = model_path();
  c.vocab_path = vocab_path();
  return c;
}

}  // namespace semcache::test
