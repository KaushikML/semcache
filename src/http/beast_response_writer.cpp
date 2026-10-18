#include "http/beast_response_writer.hpp"

namespace semcache {
namespace {

template <class Body>
void apply_headers(http::response<Body>& res, std::string_view content_type, const HeaderList& headers) {
  res.set(http::field::server, "semcache");
  res.set(http::field::content_type, content_type);
  for (const auto& [k, v] : headers) res.set(k, v);
}

}  // namespace

Task<bool> BeastResponseWriter::send(unsigned status, std::string_view content_type, std::string body,
                                     const HeaderList& headers) {
  responded_ = true;
  http::response<http::string_body> res{static_cast<http::status>(status), version_};
  apply_headers(res, content_type, headers);
  res.keep_alive(keep_alive_);
  res.body() = std::move(body);
  res.prepare_payload();
  stream_.expires_after(write_timeout_);
  auto [ec, n] = co_await http::async_write(stream_, res, use_nothrow);
  if (ec) healthy_ = false;
  co_return !ec;
}

Task<bool> BeastResponseWriter::begin_stream(unsigned status, std::string_view content_type,
                                             const HeaderList& headers) {
  responded_ = true;
  http::response<http::empty_body> res{static_cast<http::status>(status), version_};
  apply_headers(res, content_type, headers);
  res.set(http::field::cache_control, "no-cache");
  res.keep_alive(keep_alive_);
  res.chunked(true);
  http::response_serializer<http::empty_body> sr{res};
  stream_.expires_after(write_timeout_);
  auto [ec, n] = co_await http::async_write_header(stream_, sr, use_nothrow);
  if (ec) healthy_ = false;
  co_return !ec;
}

Task<bool> BeastResponseWriter::write_chunk(std::string_view data) {
  if (!healthy_) co_return false;
  if (data.empty()) co_return true;  // an empty chunk would terminate the stream
  stream_.expires_after(write_timeout_);
  auto [ec, n] = co_await net::async_write(stream_, http::make_chunk(net::buffer(data)), use_nothrow);
  if (ec) healthy_ = false;
  co_return !ec;
}

Task<bool> BeastResponseWriter::end_stream() {
  if (!healthy_) co_return false;
  stream_.expires_after(write_timeout_);
  auto [ec, n] = co_await net::async_write(stream_, http::make_chunk_last(), use_nothrow);
  if (ec) healthy_ = false;
  co_return !ec;
}

}  // namespace semcache
