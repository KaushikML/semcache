#include "proxy/pipeline.hpp"

#include <nlohmann/json.hpp>

#include "http/errors.hpp"

namespace semcache {
namespace {

using Clock = RequestPipeline::Clock;

constexpr std::string_view kJson = "application/json";
constexpr std::string_view kEventStream = "text/event-stream";

std::string latency_ms(Clock::time_point since) {
  auto ms = std::chrono::duration<double, std::milli>(Clock::now() - since).count();
  return std::to_string(static_cast<long long>(ms + 0.5));
}

HeaderList semcache_headers(CacheResult result, Clock::time_point received) {
  return {{"X-Semcache", std::string(to_string(result))}, {"X-Semcache-Latency-Ms", latency_ms(received)}};
}

// Relays the upstream response to the client: streamed bodies chunk by chunk, others buffered
// (bounded) so status, headers and body are sent together.
class PassthroughSink final : public UpstreamSink {
 public:
  PassthroughSink(ResponseWriter& out, const UpstreamConfig& cfg, Clock::time_point received)
      : out_(out), cfg_(cfg), received_(received) {}

  Task<bool> on_head(const UpstreamResponseHead& head) override {
    head_ = head;
    streaming_ = head.content_type.starts_with(kEventStream);
    if (!streaming_) co_return true;
    auto headers = semcache_headers(CacheResult::bypass, received_);
    headers.insert(headers.end(), head.passthrough_headers.begin(), head.passthrough_headers.end());
    client_ok_ = co_await out_.begin_stream(head.status, head.content_type, headers);
    co_return client_ok_;
  }

  Task<bool> on_body(std::string_view chunk) override {
    if (streaming_) {
      client_ok_ = co_await out_.write_chunk(chunk);
      co_return client_ok_;
    }
    if (body_.size() + chunk.size() > cfg_.max_response_bytes) {
      too_large_ = true;
      co_return false;
    }
    body_.append(chunk);
    co_return true;
  }

  const UpstreamResponseHead& head() const { return head_; }
  bool streaming() const { return streaming_; }
  bool client_ok() const { return client_ok_; }
  bool too_large() const { return too_large_; }
  std::string take_body() { return std::move(body_); }

 private:
  ResponseWriter& out_;
  const UpstreamConfig& cfg_;
  Clock::time_point received_;
  UpstreamResponseHead head_;
  bool streaming_ = false;
  bool client_ok_ = true;
  bool too_large_ = false;
  std::string body_;
};

// Only checks what we must understand to proxy safely; upstream does full validation.
bool looks_like_chat_request(const std::string& body) {
  auto j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  return j.is_object() && j.contains("messages") && j["messages"].is_array();
}

}  // namespace

RequestPipeline::RequestPipeline(const Config& cfg, Upstream& upstream, ProxyMetrics& metrics)
    : cfg_(cfg), upstream_(upstream), metrics_(metrics) {}

Task<PipelineOutcome> RequestPipeline::handle_chat(const Tenant& /*tenant*/, std::string body,
                                                   ResponseWriter& out, Clock::time_point received) {
  PipelineOutcome outcome;
  if (!looks_like_chat_request(body)) {
    metrics_.bad_requests.inc();
    outcome.status = 400;
    co_await out.send(400, kJson,
                      openai_error_json("request body must be a JSON object with a 'messages' array",
                                        "invalid_request_error"),
                      semcache_headers(CacheResult::bypass, received));
    co_return outcome;
  }

  PassthroughSink sink(out, cfg_.upstream, received);
  UpstreamRequest req{.target = "/v1/chat/completions", .body = std::move(body)};
  auto r = co_await upstream_.send(req, sink);

  outcome.upstream_error = r.error;
  outcome.upstream_ms = r.total_ms;
  outcome.streamed = sink.streaming();
  metrics_.upstream_seconds.observe(r.total_ms / 1000.0);
  if (r.head_delivered) metrics_.count_upstream_status(sink.head().status);
  if (r.error != UpstreamError::none && r.error != UpstreamError::aborted) {
    metrics_.upstream_errors[static_cast<std::size_t>(r.error)]->inc();
  }

  if (!sink.client_ok()) {
    metrics_.client_disconnects.inc();
    outcome.status = sink.head().status;
    outcome.keep_alive = false;
    co_return outcome;
  }

  if (sink.streaming()) {
    outcome.status = sink.head().status;
    if (r.error == UpstreamError::none) {
      outcome.keep_alive = co_await out.end_stream();
    } else {
      // Headers (200) are already on the wire: report the failure in-band, then close.
      auto event = "data: " + openai_error_json("upstream stream interrupted: " + std::string(to_string(r.error)),
                                                "upstream_error") + "\n\n";
      co_await out.write_chunk(event);
      co_await out.end_stream();
      outcome.keep_alive = false;
    }
    co_return outcome;
  }

  if (r.error == UpstreamError::none) {
    outcome.status = sink.head().status;
    auto headers = semcache_headers(CacheResult::bypass, received);
    headers.insert(headers.end(), sink.head().passthrough_headers.begin(), sink.head().passthrough_headers.end());
    auto ct = sink.head().content_type.empty() ? std::string(kJson) : sink.head().content_type;
    outcome.keep_alive = co_await out.send(outcome.status, ct, sink.take_body(), headers);
    co_return outcome;
  }

  // Nothing sent to the client yet: map the failure to a gateway error.
  std::string message;
  std::string code;
  if (sink.too_large()) {
    outcome.status = 502;
    message = "upstream response exceeds upstream.max_response_bytes";
    code = "upstream_response_too_large";
  } else if (r.error == UpstreamError::timeout) {
    outcome.status = 504;
    message = "upstream timed out";
    code = "upstream_timeout";
  } else {
    outcome.status = 502;
    message = "upstream request failed: " + std::string(to_string(r.error));
    code = "upstream_unavailable";
  }
  co_await out.send(outcome.status, kJson, openai_error_json(message, "upstream_error", code),
                    semcache_headers(CacheResult::bypass, received));
  co_return outcome;
}

}  // namespace semcache
