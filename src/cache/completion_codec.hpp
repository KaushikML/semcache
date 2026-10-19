#pragma once

// Converting between what upstream sends, what the cache stores and what a HIT returns.
//   store:  one chat.completion JSON (streamed misses are reassembled from SSE chunks)
//   serve:  that JSON with a fresh id/created, or re-streamed as chat.completion.chunk events

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"

namespace semcache {

// Reassembles an OpenAI chat.completion from a `stream=true` response body (n = 1).
class StreamAssembler {
 public:
  explicit StreamAssembler(std::size_t max_bytes) : max_bytes_(max_bytes) {}

  // Raw response bytes in any chunking.
  void feed(std::string_view bytes);

  [[nodiscard]] bool overflowed() const { return overflowed_; }
  // The assembled completion, if the stream ended with [DONE], had no error event and parsed.
  [[nodiscard]] std::optional<nlohmann::json> completion() const;

 private:
  void on_event(std::string_view data);

  std::size_t max_bytes_;
  std::size_t seen_ = 0;
  std::string pending_;  // bytes after the last complete event
  bool overflowed_ = false;
  bool done_ = false;
  bool bad_ = false;
  nlohmann::json head_ = nlohmann::json::object();  // id, created, model, system_fingerprint
  std::string role_ = "assistant";
  std::optional<std::string> content_;
  std::optional<std::string> refusal_;
  nlohmann::json tool_calls_ = nlohmann::json::array();
  nlohmann::json finish_reason_;
  nlohmann::json usage_;
};

struct StorableCompletion {
  std::string json;              // compact chat.completion
  std::int64_t total_tokens = -1;
};

// Design §6.4: one choice, finish_reason in cache.store_finish_reasons, size within
// cache.max_response_bytes. Returns the reason it is not storable via `why` otherwise.
[[nodiscard]] std::optional<StorableCompletion> make_storable(const nlohmann::json& completion,
                                                              const CacheConfig& cfg, std::string& why);

// A HIT response body: the stored completion with a new id and created time.
[[nodiscard]] std::string hit_body(const std::string& stored, std::string_view id, std::int64_t created);

// A HIT as SSE events ("data: ...\n\n" each, ending with [DONE]). Content is split near word
// boundaries into pieces of about `chunk_chars` bytes, never inside a UTF-8 sequence.
[[nodiscard]] std::vector<std::string> hit_stream_events(const std::string& stored, std::string_view id,
                                                         std::int64_t created, std::size_t chunk_chars,
                                                         bool include_usage);

}  // namespace semcache
