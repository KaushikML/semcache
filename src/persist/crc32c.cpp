#include "persist/crc32c.hpp"

#include <array>

namespace semcache {
namespace {

constexpr std::uint32_t kPoly = 0x82F63B78u;  // reflected Castagnoli polynomial

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ kPoly : c >> 1;
    t[i] = c;
  }
  return t;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32c(std::string_view data, std::uint32_t seed) {
  std::uint32_t c = ~seed;
  for (unsigned char b : data) c = kTable[(c ^ b) & 0xffu] ^ (c >> 8);
  return ~c;
}

}  // namespace semcache
