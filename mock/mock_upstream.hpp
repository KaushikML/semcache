#pragma once

// Deterministic OpenAI-compatible mock upstream for tests, benchmarks and the public demo.
// Same request (model + last user message) => same response, so runs are reproducible and free.

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "net/asio.hpp"
#include "net/listener.hpp"

namespace semcache::mock {

struct MockConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 9090;
  std::chrono::milliseconds latency{800};      // delay before the first byte (simulated model time)
  std::chrono::milliseconds token_delay{20};   // delay between streamed tokens
  int response_words = 40;
  bool allow_faults = false;                   // honour the "mock_fault" request body field
  // Fake prices used only for the X-Mock-Cost-Usd header; not tied to any real provider.
  double usd_per_1k_input_tokens = 0.0;
  double usd_per_1k_output_tokens = 0.0;
};

struct MockCompletion {
  std::string id;
  std::string model;
  std::vector<std::string> tokens;  // content pieces as streamed; concatenated = message content
  int prompt_tokens = 0;
  int completion_tokens = 0;

  [[nodiscard]] std::string content() const;
};

// Pure and deterministic. Token counting is a whitespace word count (a stand-in for a tokenizer).
[[nodiscard]] MockCompletion make_completion(const nlohmann::json& request, int response_words);
[[nodiscard]] std::string completion_json(const MockCompletion& c);
// SSE events (each "data: ...\n\n") for a streamed response, including the final [DONE].
[[nodiscard]] std::vector<std::string> stream_events(const MockCompletion& c, bool include_usage);

// Fault injection, parsed from the request body field "mock_fault": "status=503;reset-after=3;delay=500;hang".
// A body field (not a header) because the proxy forwards bodies verbatim but not client headers.
struct Fault {
  unsigned status = 0;     // respond with this error status instead of a completion
  int reset_after = -1;    // close the socket after N streamed content chunks (0 = before any)
  int delay_ms = 0;        // extra delay before the first byte
  bool hang = false;       // never respond
};
[[nodiscard]] Fault parse_fault(std::string_view header);

class MockUpstream {
 public:
  MockUpstream(net::io_context& ioc, MockConfig cfg);

  [[nodiscard]] std::uint16_t port() const { return listener_.port(); }
  void start() { listener_.start(); }
  void stop() { listener_.stop(); }
  [[nodiscard]] std::size_t active_sessions() const { return listener_.active_sessions(); }

  [[nodiscard]] std::uint64_t chat_requests() const { return chat_requests_.load(); }
  [[nodiscard]] std::string last_authorization() const;
  void reset_stats();

 private:
  using Request = http::request<http::string_body>;
  Task<void> session(beast::tcp_stream stream, tcp::endpoint remote);
  // Returns false when the connection must be closed.
  Task<bool> handle_chat(beast::tcp_stream& stream, const Request& req);

  MockConfig cfg_;
  std::atomic<std::uint64_t> chat_requests_{0};
  mutable std::mutex mu_;
  std::string last_authorization_;
  Listener listener_;
};

}  // namespace semcache::mock
