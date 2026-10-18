#include "net/listener.hpp"

namespace semcache {

Listener::Listener(net::io_context& ioc, const tcp::endpoint& endpoint, Handler handler)
    : ioc_(ioc), acceptor_(net::make_strand(ioc)), handler_(std::move(handler)) {
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(net::socket_base::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen(net::socket_base::max_listen_connections);
  port_ = acceptor_.local_endpoint().port();
}

void Listener::start() { net::co_spawn(acceptor_.get_executor(), accept_loop(), net::detached); }

void Listener::stop() {
  net::post(acceptor_.get_executor(), [this] {
    beast::error_code ignored;
    acceptor_.close(ignored);
  });
  std::lock_guard lock(mu_);
  stopping_ = true;
  for (auto& [id, s] : sessions_) {
    net::post(s.executor, [sig = s.signal] { sig->emit(net::cancellation_type::terminal); });
  }
}

std::size_t Listener::active_sessions() const {
  std::lock_guard lock(mu_);
  return sessions_.size();
}

Task<void> Listener::accept_loop() {
  for (;;) {
    auto [ec, socket] = co_await acceptor_.async_accept(net::make_strand(ioc_), use_nothrow);
    if (ec) {
      if (ec == net::error::operation_aborted || !acceptor_.is_open()) co_return;
      continue;  // transient (e.g. EMFILE); keep accepting
    }
    beast::error_code rec;
    auto remote = socket.remote_endpoint(rec);
    auto executor = socket.get_executor();
    auto signal = std::make_shared<net::cancellation_signal>();
    std::uint64_t id = 0;
    {
      std::lock_guard lock(mu_);
      if (stopping_) co_return;
      id = next_id_++;
      sessions_.emplace(id, Session{signal, executor});
    }
    auto on_done = [this, id, signal](std::exception_ptr) {
      std::lock_guard lock(mu_);
      sessions_.erase(id);
    };
    net::co_spawn(executor, handler_(beast::tcp_stream(std::move(socket)), remote),
                  net::bind_cancellation_slot(signal->slot(), std::move(on_done)));
  }
}

}  // namespace semcache
