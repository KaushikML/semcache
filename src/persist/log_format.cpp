#include "persist/log_format.hpp"

#include <cstring>
#include <fstream>

#include "persist/codec.hpp"
#include "persist/crc32c.hpp"

namespace semcache {
namespace {

constexpr std::string_view kWalMagic = "SCWAL001";
constexpr std::string_view kSnapMagic = "SCSNAP01";
constexpr std::size_t kWalHeaderBytes = 8 + 4 + 8 + 32 + 4;
constexpr std::size_t kSnapHeaderBytes = 8 + 4 + 8 + 8 + 32 + 4;

std::string with_crc(ByteWriter& w) {
  const auto crc = crc32c(w.data());
  w.u32(crc);
  return std::move(w.data());
}

void put_fingerprint(ByteWriter& w, const Sha256Digest& f) {
  w.bytes(std::string_view(reinterpret_cast<const char*>(f.data()), f.size()));
}

bool read_exact(std::ifstream& in, std::string& buf, std::size_t n) {
  buf.resize(n);
  in.read(buf.data(), static_cast<std::streamsize>(n));
  return static_cast<std::size_t>(in.gcount()) == n;
}

// Parses the common header layout; `extra` reads the type-specific fields.
template <class Extra>
bool parse_header(std::string_view h, std::string_view magic, ReadResult& r, Extra&& extra) {
  const auto body = h.substr(0, h.size() - 4);
  ByteReader rd(h);
  std::string_view m;
  std::uint32_t version = 0;
  std::string_view fp;
  std::uint32_t crc = 0;
  if (!rd.bytes(magic.size(), m) || m != magic || !rd.u32(version) || version != kFormatVersion) return false;
  if (!extra(rd) || !rd.bytes(32, fp) || !rd.u32(crc) || crc != crc32c(body)) return false;
  std::memcpy(r.fingerprint.data(), fp.data(), fp.size());
  return true;
}

// Reads `u32 len | u32 crc | body`, verifying the checksum. False on EOF or damage.
bool read_framed(std::ifstream& in, std::string& frame, std::string& body, bool& eof) {
  eof = false;
  if (!read_exact(in, frame, 8)) {
    eof = in.gcount() == 0;
    return false;
  }
  ByteReader rd(frame);
  std::uint32_t len = 0;
  std::uint32_t crc = 0;
  rd.u32(len);
  rd.u32(crc);
  if (len == 0 || len > kMaxRecordBytes) return false;
  if (!read_exact(in, body, len)) return false;
  return crc32c(body) == crc;
}

}  // namespace

std::string wal_segment_header(std::uint64_t first_lsn, const Sha256Digest& fingerprint) {
  ByteWriter w;
  w.bytes(kWalMagic);
  w.u32(kFormatVersion);
  w.u64(first_lsn);
  put_fingerprint(w, fingerprint);
  return with_crc(w);
}

std::string wal_record(WalRecordType type, std::uint64_t lsn, std::string_view payload) {
  ByteWriter body;
  body.u8(static_cast<std::uint8_t>(type));
  body.u64(lsn);
  body.bytes(payload);
  ByteWriter w;
  w.u32(static_cast<std::uint32_t>(body.data().size()));
  w.u32(crc32c(body.data()));
  w.bytes(body.data());
  return std::move(w.data());
}

std::string snapshot_header(std::uint64_t lsn, std::uint64_t count, const Sha256Digest& fingerprint) {
  ByteWriter w;
  w.bytes(kSnapMagic);
  w.u32(kFormatVersion);
  w.u64(lsn);
  w.u64(count);
  put_fingerprint(w, fingerprint);
  return with_crc(w);
}

std::string snapshot_record(std::string_view payload) {
  ByteWriter w;
  w.u32(static_cast<std::uint32_t>(payload.size()));
  w.u32(crc32c(payload));
  w.bytes(payload);
  return std::move(w.data());
}

ReadResult read_wal_segment(const std::string& path, const WalVisitor& on_record) {
  ReadResult r;
  std::ifstream in(path, std::ios::binary);
  std::string header;
  if (!in || !read_exact(in, header, kWalHeaderBytes)) return r;
  r.header_ok = parse_header(header, kWalMagic, r, [&](ByteReader& rd) { return rd.u64(r.lsn); });
  if (!r.header_ok) return r;
  r.good_bytes = kWalHeaderBytes;
  std::string frame;
  std::string body;
  for (;;) {
    bool eof = false;
    if (!read_framed(in, frame, body, eof)) {
      r.clean = eof;
      return r;
    }
    ByteReader rd(body);
    std::uint8_t type = 0;
    std::uint64_t lsn = 0;
    if (!rd.u8(type) || !rd.u64(lsn) || (type != 1 && type != 2)) return r;
    const auto payload = std::string_view(body).substr(rd.pos());
    if (!on_record(r, static_cast<WalRecordType>(type), lsn, payload)) return r;
    ++r.records;
    r.good_bytes += 8 + body.size();
  }
}

ReadResult read_snapshot(const std::string& path, const SnapshotVisitor& on_record) {
  ReadResult r;
  std::ifstream in(path, std::ios::binary);
  std::string header;
  if (!in || !read_exact(in, header, kSnapHeaderBytes)) return r;
  r.header_ok = parse_header(header, kSnapMagic, r, [&](ByteReader& rd) { return rd.u64(r.lsn) && rd.u64(r.count); });
  if (!r.header_ok) return r;
  r.good_bytes = kSnapHeaderBytes;
  std::string frame;
  std::string body;
  for (std::uint64_t i = 0; i < r.count; ++i) {
    bool eof = false;
    if (!read_framed(in, frame, body, eof) || !on_record(r, body)) return r;
    ++r.records;
    r.good_bytes += 8 + body.size();
  }
  in.peek();
  r.clean = in.eof();  // exactly `count` records and nothing after them
  return r;
}

}  // namespace semcache
