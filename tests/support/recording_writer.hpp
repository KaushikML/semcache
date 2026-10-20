#pragma once

// ResponseWriter that records what it was asked to write, for tests without sockets.

#include <string>
#include <vector>

#include "http/response_writer.hpp"

namespace semcache::test {

class RecordingWriter final : public ResponseWriter {
 public:
  Task<bool> send(unsigned s, std::string_view ct, std::string b, const HeaderList& h) override {
    status = s;
    content_type = ct;
    body = std::move(b);
    headers = h;
    complete = true;
    co_return ok;
  }
  Task<bool> begin_stream(unsigned s, std::string_view ct, const HeaderList& h) override {
    status = s;
    content_type = ct;
    headers = h;
    streaming = true;
    co_return ok;
  }
  Task<bool> write_chunk(std::string_view d) override {
    chunks.emplace_back(d);
    co_return ok;
  }
  Task<bool> end_stream() override {
    complete = true;
    co_return ok;
  }

  [[nodiscard]] std::string header(const std::string& name) const {
    for (const auto& [k, v] : headers) {
      if (k == name) return v;
    }
    return {};
  }
  [[nodiscard]] std::string joined() const {
    std::string s;
    for (const auto& c : chunks) s += c;
    return s;
  }

  bool ok = true;  // what every call returns (false simulates a client that went away)
  unsigned status = 0;
  std::string content_type;
  std::string body;
  HeaderList headers;
  std::vector<std::string> chunks;
  bool streaming = false;
  bool complete = false;
};

}  // namespace semcache::test
