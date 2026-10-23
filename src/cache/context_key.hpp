#pragma once

// Request classification and context keys (design §6.3, ADR-002). A cached answer may only be
// reused for a request whose context key is equal: same tenant, same parameters, same system
// messages and same last-K prior messages. Only the final user message is compared semantically.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/sha256.hpp"
#include "config/config.hpp"

namespace semcache {

using ContextKey = Sha256Digest;
using PromptHash = Sha256Digest;

enum class NotCacheable {
  cache_disabled,
  bypass_header,     // client sent X-Semcache-Bypass
  last_not_user,     // final message is not role=user (e.g. a tool result turn)
  non_text_content,  // multimodal parts in the final user message
  multiple_choices,  // n > 1
  logprobs,
  temperature,  // above cache.max_temperature
  invalid_tags,  // X-Semcache-Tags present but malformed: never store an entry that cannot be purged
};

[[nodiscard]] std::string_view to_string(NotCacheable r);

struct CacheableRequest {
  ContextKey context;
  PromptHash prompt_hash;  // SHA-256 of embed_text
  std::string embed_text;  // final user message: NFC, trimmed
};

struct Classification {
  std::optional<CacheableRequest> request;  // set when cacheable
  NotCacheable reason{};                    // valid when not
};

// `body` must be a JSON object with a non-empty `messages` array (validated by the caller).
[[nodiscard]] Classification classify_request(const nlohmann::json& body, std::string_view tenant_id,
                                              const CacheConfig& cfg, bool bypass_header);

// Parses X-Semcache-Tags ("a,b,c"): trimmed, non-empty, [A-Za-z0-9._:/-], at most
// cache.max_tag_bytes each and cache.max_tags_per_entry in total; duplicates removed, sorted.
// Empty header => no tags. nullopt => malformed.
[[nodiscard]] std::optional<std::vector<std::string>> parse_tags(std::string_view header, const CacheConfig& cfg);

// Deterministic JSON: object keys sorted, no whitespace, integers as integers, floats in shortest
// round-trip form (so 1 and 1.0 encode the same). Exposed for tests.
[[nodiscard]] std::string canonical_json(const nlohmann::json& j);

}  // namespace semcache
