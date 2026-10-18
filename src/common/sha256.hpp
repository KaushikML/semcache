#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace semcache {

using Sha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha256Digest sha256(std::string_view data);
[[nodiscard]] std::string to_hex(const Sha256Digest& digest);

}  // namespace semcache
