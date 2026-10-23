#pragma once

// Little-endian binary encoding of cache entries for the WAL and snapshots. Decoding is
// bounds-checked everywhere: files on disk may be torn or corrupted, and a bad record must be
// rejected, never read past.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cache/cache_store.hpp"

namespace semcache {

class ByteWriter {
 public:
  void u8(std::uint8_t v) { out_.push_back(static_cast<char>(v)); }
  void u16(std::uint16_t v) { put(v, 2); }
  void u32(std::uint32_t v) { put(v, 4); }
  void u64(std::uint64_t v) { put(v, 8); }
  void i64(std::int64_t v) { put(static_cast<std::uint64_t>(v), 8); }
  void bytes(std::string_view b) { out_.append(b); }
  void str(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    bytes(s);
  }
  [[nodiscard]] std::string& data() { return out_; }

 private:
  void put(std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) out_.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
  }
  std::string out_;
};

class ByteReader {
 public:
  explicit ByteReader(std::string_view in) : in_(in) {}

  bool u8(std::uint8_t& v) { return get(v, 1); }
  bool u16(std::uint16_t& v) { return get(v, 2); }
  bool u32(std::uint32_t& v) { return get(v, 4); }
  bool u64(std::uint64_t& v) { return get(v, 8); }
  bool i64(std::int64_t& v) {
    std::uint64_t u = 0;
    if (!get(u, 8)) return false;
    v = static_cast<std::int64_t>(u);
    return true;
  }
  bool bytes(std::size_t n, std::string_view& out) {
    if (in_.size() - pos_ < n) return false;
    out = in_.substr(pos_, n);
    pos_ += n;
    return true;
  }
  bool str(std::string& s) {
    std::uint32_t n = 0;
    std::string_view b;
    if (!u32(n) || !bytes(n, b)) return false;
    s.assign(b);
    return true;
  }
  [[nodiscard]] bool done() const { return pos_ == in_.size(); }
  [[nodiscard]] std::size_t pos() const { return pos_; }

 private:
  template <class T>
  bool get(T& v, int n) {
    if (in_.size() - pos_ < static_cast<std::size_t>(n)) return false;
    std::uint64_t x = 0;
    for (int i = 0; i < n; ++i) x |= static_cast<std::uint64_t>(static_cast<unsigned char>(in_[pos_ + static_cast<std::size_t>(i)])) << (8 * i);
    v = static_cast<T>(x);
    pos_ += static_cast<std::size_t>(n);
    return true;
  }
  std::string_view in_;
  std::size_t pos_ = 0;
};

// Entry payload: context, prompt hash, created/expires (ms since epoch), total tokens, flags,
// tenant, prompt, response, [embedding], tags.
void encode_entry(ByteWriter& w, const CacheEntry& e);
// Returns a NewEntry with `created`/`expires` set (for CacheStore::insert's recovery path), or
// nullopt if the payload is malformed. `keep_embedding = false` drops the vector (model changed).
[[nodiscard]] std::optional<NewEntry> decode_entry(std::string_view payload, bool keep_embedding);

}  // namespace semcache
