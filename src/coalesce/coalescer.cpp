#include "coalesce/coalescer.hpp"

#include <cstring>

#include "http/errors.hpp"

namespace semcache {
namespace {

bool is_semcache_header(std::string_view name) {
  constexpr std::string_view kPrefix = "x-semcache";
  if (name.size() < kPrefix.size()) return false;
  for (std::size_t i = 0; i < kPrefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(name[i])) != kPrefix[i]) return false;
  }
  return true;
}

HeaderList shared_headers(const HeaderList& headers) {
  HeaderList out;
  for (const auto& h : headers) {
    if (!is_semcache_header(h.first)) out.push_back(h);
  }
  return out;
}

HeaderList merge(HeaderList own, const HeaderList& shared) {
  own.insert(own.end(), shared.begin(), shared.end());
  return own;
}

}  // namespace

std::size_t FlightKeyHash::operator()(const FlightKey& k) const noexcept {
  std::size_t a;
  std::size_t b;
  std::memcpy(&a, k.context.data(), sizeof(a));  // SHA-256 bytes: already uniform
  std::memcpy(&b, k.prompt.data(), sizeof(b));
  return a ^ (b * 0x9e3779b97f4a7c15ull) ^ (k.stream ? 0x1u : 0u) ^ (k.include_usage ? 0x2u : 0u);
}

// ---------------------------------------------------------------- Flight

bool Flight::finish_locked(State s) {
  if (state_ != State::running) return false;
  state_ = s;
  return true;
}

void Flight::record_send(unsigned status, std::string_view content_type, const std::string& body,
                         const HeaderList& headers) {
  {
    std::lock_guard lock(mu_);
    if (state_ != State::running || head_) return;
    head_ = Head{status, std::string(content_type), shared_headers(headers), false};
    body_ = body;
    finish_locked(State::done);
  }
  notify();
}

void Flight::record_begin(unsigned status, std::string_view content_type, const HeaderList& headers) {
  {
    std::lock_guard lock(mu_);
    if (state_ != State::running || head_) return;
    head_ = Head{status, std::string(content_type), shared_headers(headers), true};
  }
  notify();
}

void Flight::record_chunk(std::string_view data) {
  {
    std::lock_guard lock(mu_);
    if (state_ != State::running || !head_ || !head_->streaming) return;
    log_bytes_ += data.size();
    if (log_bytes_ > max_log_bytes_) {
      finish_locked(State::aborted);  // bounded memory: followers get an error, the leader carries on
    } else {
      chunks_.push_back(std::make_shared<const std::string>(data));
    }
  }
  notify();
}

void Flight::record_end() {
  {
    std::lock_guard lock(mu_);
    if (!finish_locked(State::done)) return;
  }
  notify();
}

void Flight::abort() {
  {
    std::lock_guard lock(mu_);
    if (!finish_locked(State::aborted)) return;
  }
  notify();
}

std::optional<std::uint64_t> Flight::add_follower(std::shared_ptr<Wakeup> wakeup, std::size_t max_followers) {
  std::lock_guard lock(mu_);
  if (waiters_.size() >= max_followers) return std::nullopt;
  const auto id = next_follower_++;
  waiters_.emplace(id, std::move(wakeup));
  follower_count_.store(waiters_.size(), std::memory_order_release);
  return id;
}

void Flight::remove_follower(std::uint64_t id) {
  std::lock_guard lock(mu_);
  waiters_.erase(id);
  follower_count_.store(waiters_.size(), std::memory_order_release);
}

Flight::View Flight::read(std::size_t from) const {
  std::lock_guard lock(mu_);
  View v;
  v.head = head_;
  v.state = state_;
  if (head_ && !head_->streaming) v.body = body_;
  if (from < chunks_.size()) v.chunks.assign(chunks_.begin() + static_cast<std::ptrdiff_t>(from), chunks_.end());
  return v;
}

void Flight::notify() {
  std::vector<std::shared_ptr<Wakeup>> waiters;
  {
    std::lock_guard lock(mu_);
    waiters.reserve(waiters_.size());
    for (const auto& [id, w] : waiters_) waiters.push_back(w);
  }
  // Capacity-1 channels: a wakeup sent while the follower is busy is kept, so none is lost; a
  // second one while the first is pending is redundant and dropped.
  for (const auto& w : waiters) w->try_send(boost::system::error_code{});
}

// ---------------------------------------------------------------- Coalescer

Coalescer::Coalescer(const CoalescerConfig& cfg, std::size_t max_log_bytes, MetricsRegistry& m)
    : cfg_(cfg),
      max_log_bytes_(max_log_bytes),
      active_gauge_(m.gauge("semcache_flights_active", "Upstream calls currently shared by coalescing")) {}

Coalescer::Join Coalescer::join(const FlightKey& key) {
  std::lock_guard lock(mu_);
  auto [it, inserted] = flights_.try_emplace(key);
  if (inserted) {
    it->second = std::make_shared<Flight>(max_log_bytes_);
    active_gauge_.set(static_cast<std::int64_t>(flights_.size()));
  }
  return Join{inserted, it->second};
}

void Coalescer::retire(const FlightKey& key, const FlightPtr& flight) {
  std::lock_guard lock(mu_);
  if (auto it = flights_.find(key); it != flights_.end() && it->second == flight) {
    flights_.erase(it);
    active_gauge_.set(static_cast<std::int64_t>(flights_.size()));
  }
}

std::size_t Coalescer::active() const {
  std::lock_guard lock(mu_);
  return flights_.size();
}

// ---------------------------------------------------------------- LeaderWriter

Task<bool> LeaderWriter::send(unsigned status, std::string_view content_type, std::string body,
                              const HeaderList& headers) {
  flight_->record_send(status, content_type, body, headers);  // before the body is moved out
  if (client_ok_) client_ok_ = co_await client_.send(status, content_type, std::move(body), headers);
  co_return keep_going();
}

Task<bool> LeaderWriter::begin_stream(unsigned status, std::string_view content_type, const HeaderList& headers) {
  flight_->record_begin(status, content_type, headers);
  if (client_ok_) client_ok_ = co_await client_.begin_stream(status, content_type, headers);
  co_return keep_going();
}

Task<bool> LeaderWriter::write_chunk(std::string_view data) {
  flight_->record_chunk(data);
  if (client_ok_) client_ok_ = co_await client_.write_chunk(data);
  co_return keep_going();
}

Task<bool> LeaderWriter::end_stream() {
  flight_->record_end();
  if (client_ok_) client_ok_ = co_await client_.end_stream();
  co_return client_ok_;
}

// ---------------------------------------------------------------- follow

Task<FollowResult> follow(FlightPtr flight_ptr, std::size_t max_followers, ResponseWriter& out,
                          std::function<HeaderList()> headers) {
  Flight& flight = *flight_ptr;
  FollowResult res;
  auto wakeup = std::make_shared<Flight::Wakeup>(co_await net::this_coro::executor, 1);
  auto id = flight.add_follower(wakeup, max_followers);
  if (!id) co_return res;  // too many followers: serve independently
  struct Leave {
    Flight& f;
    std::uint64_t id;
    ~Leave() { f.remove_follower(id); }
  } leave{flight, *id};

  std::size_t next = 0;
  bool started = false;
  for (;;) {
    auto v = flight.read(next);
    if (!v.head) {
      if (v.state != Flight::State::running) co_return res;  // leader gone before responding: fail open
    } else if (!v.head->streaming) {
      res.served = true;
      res.status = v.head->status;
      res.keep_alive = co_await out.send(v.head->status, v.head->content_type, std::move(v.body),
                                         merge(headers(), v.head->headers));
      co_return res;
    } else {
      if (!started) {
        res.served = true;
        res.streamed = true;
        res.status = v.head->status;
        started = true;
        if (!co_await out.begin_stream(v.head->status, v.head->content_type, merge(headers(), v.head->headers))) {
          res.keep_alive = false;
          co_return res;
        }
      }
      for (const auto& c : v.chunks) {
        if (!co_await out.write_chunk(*c)) {
          res.keep_alive = false;
          co_return res;
        }
      }
      next += v.chunks.size();
      // `state` was read together with the chunks, so done/aborted here means nothing is missing.
      if (v.state == Flight::State::done) {
        res.keep_alive = co_await out.end_stream();
        co_return res;
      }
      if (v.state == Flight::State::aborted) {
        auto event = "data: " + openai_error_json("shared upstream response was interrupted", "upstream_error") + "\n\n";
        co_await out.write_chunk(event);
        co_await out.end_stream();
        res.keep_alive = false;
        co_return res;
      }
    }
    auto [ec] = co_await wakeup->async_receive(use_nothrow);
    if (ec) {  // cancelled (shutdown or client gone)
      res.keep_alive = false;
      if (!started) res.served = true;  // do not start serving independently while shutting down
      co_return res;
    }
  }
}

}  // namespace semcache
