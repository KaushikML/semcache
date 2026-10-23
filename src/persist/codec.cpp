#include "persist/codec.hpp"

#include <bit>
#include <cstring>

namespace semcache {
namespace {

constexpr std::uint8_t kHasEmbedding = 0x1;
// Sanity limits for decoding untrusted lengths (far above what the cache ever stores).
constexpr std::uint16_t kMaxTags = 1024;

std::int64_t to_ms(CacheClock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}
CacheClock::time_point from_ms(std::int64_t ms) {
  return CacheClock::time_point(std::chrono::duration_cast<CacheClock::duration>(std::chrono::milliseconds(ms)));
}

}  // namespace

void encode_entry(ByteWriter& w, const CacheEntry& e) {
  w.bytes(std::string_view(reinterpret_cast<const char*>(e.context.data()), e.context.size()));
  w.bytes(std::string_view(reinterpret_cast<const char*>(e.prompt_hash.data()), e.prompt_hash.size()));
  w.i64(to_ms(e.created));
  w.i64(to_ms(e.expires));
  w.i64(e.total_tokens);
  w.u8(e.embedding ? kHasEmbedding : 0);
  w.str(e.tenant);
  w.str(e.prompt);
  w.str(e.response);
  if (e.embedding) {
    for (float f : *e.embedding) w.u32(std::bit_cast<std::uint32_t>(f));
  }
  w.u16(static_cast<std::uint16_t>(e.tags.size()));
  for (const auto& t : e.tags) w.str(t);
}

std::optional<NewEntry> decode_entry(std::string_view payload, bool keep_embedding) {
  ByteReader r(payload);
  NewEntry e;
  std::string_view ctx;
  std::string_view ph;
  std::int64_t created = 0;
  std::int64_t expires = 0;
  std::uint8_t flags = 0;
  if (!r.bytes(e.context.size(), ctx) || !r.bytes(e.prompt_hash.size(), ph) || !r.i64(created) || !r.i64(expires) ||
      !r.i64(e.total_tokens) || !r.u8(flags) || !r.str(e.tenant) || !r.str(e.prompt) || !r.str(e.response)) {
    return std::nullopt;
  }
  std::memcpy(e.context.data(), ctx.data(), ctx.size());
  std::memcpy(e.prompt_hash.data(), ph.data(), ph.size());
  if (flags & kHasEmbedding) {
    Embedding v{};
    for (auto& f : v) {
      std::uint32_t bits = 0;
      if (!r.u32(bits)) return std::nullopt;
      f = std::bit_cast<float>(bits);
    }
    if (keep_embedding) e.embedding = v;
  }
  std::uint16_t n_tags = 0;
  if (!r.u16(n_tags) || n_tags > kMaxTags) return std::nullopt;
  e.tags.resize(n_tags);
  for (auto& t : e.tags) {
    if (!r.str(t)) return std::nullopt;
  }
  if (!r.done()) return std::nullopt;  // trailing bytes: not a record we wrote
  e.created = from_ms(created);
  e.expires = from_ms(expires);
  return e;
}

}  // namespace semcache
