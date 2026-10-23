#pragma once

// On-disk formats (ADR-008). All integers little-endian.
//
// WAL segment  wal-<first lsn, 16 hex>.log
//   header  "SCWAL001" | u32 version | u64 first_lsn | fingerprint[32] | u32 crc32c(previous)
//   record  u32 len | u32 crc32c(body) | body = u8 type | u64 lsn | payload
//           type 1 INSERT: payload = encoded entry;  type 2 DELETE: context[32] | prompt_hash[32]
// Snapshot  snapshot.bin (written as snapshot.bin.tmp, fsynced, renamed)
//   header  "SCSNAP01" | u32 version | u64 lsn | u64 count | fingerprint[32] | u32 crc32c(previous)
//   record  u32 len | u32 crc32c(payload) | payload = encoded entry      (count records)
//
// The fingerprint identifies the embedding model; vectors from another model are not loaded.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "common/sha256.hpp"

namespace semcache {

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::uint32_t kMaxRecordBytes = 64u << 20;  // sanity bound on untrusted lengths

enum class WalRecordType : std::uint8_t { insert = 1, remove = 2 };

[[nodiscard]] std::string wal_segment_header(std::uint64_t first_lsn, const Sha256Digest& fingerprint);
[[nodiscard]] std::string wal_record(WalRecordType type, std::uint64_t lsn, std::string_view payload);
[[nodiscard]] std::string snapshot_header(std::uint64_t lsn, std::uint64_t count, const Sha256Digest& fingerprint);
[[nodiscard]] std::string snapshot_record(std::string_view payload);

struct ReadResult {
  bool header_ok = false;
  Sha256Digest fingerprint{};
  std::uint64_t lsn = 0;          // segment: first lsn; snapshot: snapshot lsn
  std::uint64_t count = 0;        // snapshot: declared records
  std::uint64_t records = 0;      // records read successfully
  std::uint64_t good_bytes = 0;   // offset just past the last good record
  bool clean = false;             // reached end of file exactly at a record boundary (and count matched)
};

// Calls `on_record(header, type, lsn, payload)` for each valid WAL record, in order; stops at the
// first bad or truncated one. A `false` from the callback also stops.
using WalVisitor = std::function<bool(const ReadResult& header, WalRecordType, std::uint64_t, std::string_view)>;
[[nodiscard]] ReadResult read_wal_segment(const std::string& path, const WalVisitor& on_record);

using SnapshotVisitor = std::function<bool(const ReadResult& header, std::string_view)>;
[[nodiscard]] ReadResult read_snapshot(const std::string& path, const SnapshotVisitor& on_record);

}  // namespace semcache
