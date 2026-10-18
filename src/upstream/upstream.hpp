#pragma once

// Interface to the upstream LLM API. Implementations deliver the response incrementally to an
// UpstreamSink so streaming bodies are passed through as they arrive.

#include <string>
#include <string_view>

#include "http/response_writer.hpp"
#include "net/asio.hpp"

namespace semcache {

struct UpstreamRequest {
  std::string target;  // path, e.g. "/v1/chat/completions" (base_url prefix is added by the client)
  std::string body;
  std::string content_type = "application/json";
};

struct UpstreamResponseHead {
  unsigned status = 0;
  std::string content_type;
  HeaderList passthrough_headers;  // allow-listed upstream headers to relay to the client
};

class UpstreamSink {
 public:
  virtual ~UpstreamSink() = default;
  // Return false to abort the exchange (e.g. client went away).
  virtual Task<bool> on_head(const UpstreamResponseHead& head) = 0;
  virtual Task<bool> on_body(std::string_view chunk) = 0;
};

enum class UpstreamError {
  none,
  connect,   // DNS / TCP / TLS failure before the request was sent
  timeout,   // any of the configured timeouts fired
  protocol,  // malformed HTTP from upstream
  reset,     // connection closed before the response completed
  aborted,   // the sink asked to stop
};

[[nodiscard]] std::string_view to_string(UpstreamError e);

struct UpstreamResult {
  UpstreamError error = UpstreamError::none;
  std::string detail;            // for logs; never contains request content or credentials
  bool head_delivered = false;   // true once on_head was called
  double first_byte_ms = 0;
  double total_ms = 0;
};

class Upstream {
 public:
  virtual ~Upstream() = default;
  virtual Task<UpstreamResult> send(const UpstreamRequest& request, UpstreamSink& sink) = 0;
};

}  // namespace semcache
