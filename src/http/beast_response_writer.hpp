#pragma once

#include <chrono>

#include "http/response_writer.hpp"

namespace semcache {

// ResponseWriter over a Beast TCP stream. Streams use chunked transfer encoding.
class BeastResponseWriter final : public ResponseWriter {
 public:
  BeastResponseWriter(beast::tcp_stream& stream, unsigned http_version, bool keep_alive,
                      std::chrono::milliseconds write_timeout)
      : stream_(stream), version_(http_version), keep_alive_(keep_alive), write_timeout_(write_timeout) {}

  Task<bool> send(unsigned status, std::string_view content_type, std::string body,
                  const HeaderList& headers) override;
  Task<bool> begin_stream(unsigned status, std::string_view content_type,
                          const HeaderList& headers) override;
  Task<bool> write_chunk(std::string_view data) override;
  Task<bool> end_stream() override;

  // False once a write failed or a stream was aborted; the connection must then be closed.
  [[nodiscard]] bool healthy() const { return healthy_; }
  [[nodiscard]] bool responded() const { return responded_; }

 private:
  beast::tcp_stream& stream_;
  unsigned version_;
  bool keep_alive_;
  std::chrono::milliseconds write_timeout_;
  bool healthy_ = true;
  bool responded_ = false;
};

}  // namespace semcache
