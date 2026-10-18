#include "upstream/http_upstream.hpp"

#include <array>

namespace semcache {
namespace {

using Clock = std::chrono::steady_clock;

// Read buffer for streamed upstream bodies. SSE events are small, so this rarely fills.
constexpr std::size_t kReadChunkBytes = 16 * 1024;

// Upstream headers worth relaying to the client; everything else (cookies, rate-limit details of
// the shared upstream account, ...) is dropped.
constexpr std::array<std::string_view, 3> kPassthroughHeaders = {
    "x-request-id", "openai-processing-ms", "openai-model"};

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

UpstreamError classify(const beast::error_code& ec) {
  if (ec == beast::error::timeout) return UpstreamError::timeout;
  if (ec == http::error::end_of_stream || ec == http::error::partial_message ||
      ec == net::error::eof || ec == net::error::connection_reset ||
      ec == net::ssl::error::stream_truncated) {
    return UpstreamError::reset;
  }
  return UpstreamError::protocol;
}

}  // namespace

std::string_view to_string(UpstreamError e) {
  switch (e) {
    case UpstreamError::none: return "none";
    case UpstreamError::connect: return "connect";
    case UpstreamError::timeout: return "timeout";
    case UpstreamError::protocol: return "protocol";
    case UpstreamError::reset: return "reset";
    case UpstreamError::aborted: return "aborted";
  }
  return "unknown";
}

ssl::context make_client_tls_context(bool verify_peer) {
  ssl::context ctx(ssl::context::tls_client);
  ctx.set_default_verify_paths();
  ctx.set_verify_mode(verify_peer ? ssl::verify_peer : ssl::verify_none);
  return ctx;
}

HttpUpstream::HttpUpstream(const UpstreamConfig& cfg, ssl::context& tls_ctx) : cfg_(cfg), tls_ctx_(tls_ctx) {}

Task<UpstreamResult> HttpUpstream::send(const UpstreamRequest& request, UpstreamSink& sink) {
  const auto start = Clock::now();
  auto executor = co_await net::this_coro::executor;
  UpstreamResult fail;

  tcp::resolver resolver(executor);
  auto [rec, endpoints] = co_await resolver.async_resolve(cfg_.host, std::to_string(cfg_.port), use_nothrow);
  if (rec) {
    fail.error = UpstreamError::connect;
    fail.detail = "resolve: " + rec.message();
    fail.total_ms = ms_since(start);
    co_return fail;
  }

  beast::tcp_stream tcp_stream(executor);
  tcp_stream.expires_after(cfg_.connect_timeout);
  auto [cec, ep] = co_await tcp_stream.async_connect(endpoints, use_nothrow);
  if (cec) {
    fail.error = cec == beast::error::timeout ? UpstreamError::timeout : UpstreamError::connect;
    fail.detail = "connect: " + cec.message();
    fail.total_ms = ms_since(start);
    co_return fail;
  }

  if (!cfg_.tls) {
    auto result = co_await exchange(tcp_stream, request, sink, start);
    beast::error_code ignored;
    tcp_stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    co_return result;
  }

  beast::ssl_stream<beast::tcp_stream> tls_stream(std::move(tcp_stream), tls_ctx_);
  // SSL_set_tlsext_host_name is a macro with a C-style cast; call SSL_ctrl directly.
  if (SSL_ctrl(tls_stream.native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
               const_cast<char*>(cfg_.host.c_str())) != 1) {
    fail.error = UpstreamError::connect;
    fail.detail = "tls: failed to set SNI";
    co_return fail;
  }
  if (cfg_.verify_tls) tls_stream.set_verify_callback(ssl::host_name_verification(cfg_.host));
  beast::get_lowest_layer(tls_stream).expires_after(cfg_.connect_timeout);
  auto [hec] = co_await tls_stream.async_handshake(ssl::stream_base::client, use_nothrow);
  if (hec) {
    fail.error = hec == beast::error::timeout ? UpstreamError::timeout : UpstreamError::connect;
    fail.detail = "tls handshake: " + hec.message();
    fail.total_ms = ms_since(start);
    co_return fail;
  }
  auto result = co_await exchange(tls_stream, request, sink, start);
  // Best-effort close; many servers drop TLS without close_notify, so errors are ignored.
  beast::get_lowest_layer(tls_stream).expires_after(cfg_.connect_timeout);
  co_await tls_stream.async_shutdown(use_nothrow);
  co_return result;
}

template <class Stream>
Task<UpstreamResult> HttpUpstream::exchange(Stream& stream, const UpstreamRequest& request,
                                            UpstreamSink& sink, Clock::time_point start) {
  UpstreamResult result;
  const auto deadline = start + cfg_.total_timeout;
  auto& lowest = beast::get_lowest_layer(stream);
  // Each operation gets the smaller of its own budget and what is left of the total budget.
  auto arm = [&](std::chrono::milliseconds budget) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    lowest.expires_after(std::max(std::chrono::milliseconds{1}, std::min(budget, remaining)));
  };
  auto finish = [&](UpstreamError e, std::string detail) {
    result.error = e;
    result.detail = std::move(detail);
    result.total_ms = ms_since(start);
    return result;
  };

  http::request<http::string_body> req{http::verb::post, cfg_.path_prefix + request.target, 11};
  req.set(http::field::host, cfg_.host);
  req.set(http::field::user_agent, "semcache/0.1");
  req.set(http::field::content_type, request.content_type);
  req.set(http::field::accept, "application/json, text/event-stream");
  if (!cfg_.api_key.empty()) req.set(http::field::authorization, "Bearer " + cfg_.api_key.reveal());
  req.keep_alive(false);
  req.body() = request.body;
  req.prepare_payload();

  arm(cfg_.idle_timeout);
  auto [wec, wn] = co_await http::async_write(stream, req, use_nothrow);
  if (wec) co_return finish(classify(wec), "write: " + wec.message());

  beast::flat_buffer buffer;
  http::response_parser<http::buffer_body> parser;
  parser.body_limit(boost::none);  // the sink enforces size limits where it buffers

  arm(cfg_.first_byte_timeout);
  auto [hec, hn] = co_await http::async_read_header(stream, buffer, parser, use_nothrow);
  if (hec) co_return finish(classify(hec), "read header: " + hec.message());
  result.first_byte_ms = ms_since(start);

  UpstreamResponseHead head;
  const auto& msg = parser.get();
  head.status = msg.result_int();
  head.content_type = std::string(msg[http::field::content_type]);
  for (auto name : kPassthroughHeaders) {
    if (auto it = msg.find(name); it != msg.end()) head.passthrough_headers.emplace_back(name, it->value());
  }
  result.head_delivered = true;
  if (!co_await sink.on_head(head)) co_return finish(UpstreamError::aborted, "sink aborted on head");

  std::array<char, kReadChunkBytes> chunk{};
  while (!parser.is_done()) {
    parser.get().body().data = chunk.data();
    parser.get().body().size = chunk.size();
    arm(cfg_.idle_timeout);
    auto [rec, rn] = co_await http::async_read_some(stream, buffer, parser, use_nothrow);
    if (rec == http::error::need_buffer) rec = {};
    const std::size_t got = chunk.size() - parser.get().body().size;
    if (got > 0 && !co_await sink.on_body(std::string_view(chunk.data(), got))) {
      co_return finish(UpstreamError::aborted, "sink aborted on body");
    }
    if (rec) co_return finish(classify(rec), "read body: " + rec.message());
  }
  co_return finish(UpstreamError::none, {});
}

}  // namespace semcache
