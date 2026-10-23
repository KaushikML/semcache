#pragma once

// CRC-32C (Castagnoli), the checksum on every WAL and snapshot record. Table-driven; fast enough
// for records written by one thread (measured in tests, not on the hot path).

#include <cstdint>
#include <string_view>

namespace semcache {

[[nodiscard]] std::uint32_t crc32c(std::string_view data, std::uint32_t seed = 0);

}  // namespace semcache
