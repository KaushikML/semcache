#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "net/asio.hpp"

namespace semcache {

// Accepts TCP connections and spawns one coroutine per connection on its own strand.
// Shared by the proxy and the mock upstream.
//
// Shutdown: stop() closes the acceptor and emits terminal cancellation to every live session (on
// that session's strand), which aborts its pending I/O, including upstream calls it awaits.
// Owners wait for active_sessions() == 0 before destroying anything sessions reference.
class Listener {
 public:
  using Handler = std::function<Task<void>(beast::tcp_stream, tcp::endpoint remote)>;

  // Binds immediately (throws on failure, e.g. port in use). Port 0 picks an ephemeral port.
  Listener(net::io_context& ioc, const tcp::endpoint& endpoint, Handler handler);

  [[nodiscard]] std::uint16_t port() const { return port_; }
  void start();
  void stop();
  [[nodiscard]] std::size_t active_sessions() const;

 private:
  struct Session {
    std::shared_ptr<net::cancellation_signal> signal;
    net::any_io_executor executor;
  };

  Task<void> accept_loop();

  net::io_context& ioc_;
  tcp::acceptor acceptor_;
  Handler handler_;
  std::uint16_t port_;

  mutable std::mutex mu_;  // guards sessions_, next_id_, stopping_
  std::unordered_map<std::uint64_t, Session> sessions_;
  std::uint64_t next_id_ = 0;
  bool stopping_ = false;
};

}  // namespace semcache
