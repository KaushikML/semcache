#include "common/sha256.hpp"

#include <openssl/evp.h>

#include <stdexcept>

namespace semcache {

Sha256Digest sha256(std::string_view data) {
  Sha256Digest out{};
  unsigned int len = 0;
  if (EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_sha256(), nullptr) != 1 ||
      len != out.size()) {
    throw std::runtime_error("EVP_Digest(sha256) failed");
  }
  return out;
}

std::string to_hex(const Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (std::uint8_t b : digest) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

}  // namespace semcache
