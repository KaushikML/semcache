#include "mock_upstream.hpp"

#include <array>
#include <charconv>
#include <iomanip>
#include <sstream>

#include "http/beast_response_writer.hpp"
#include "http/errors.hpp"

namespace semcache::mock {
namespace {

// Fixed `created` timestamp keeps responses byte-identical across runs.
constexpr std::int64_t kCreated = 1700000000;
constexpr std::string_view kJson = "application/json";
// How long a "hang" fault waits; far beyond any sane client timeout.
constexpr auto kHangDuration = std::chrono::hours(1);

constexpr std::array<std::string_view, 32> kVocabulary = {
    "the",     "cache",   "answer",  "request", "model",   "system", "latency", "token",
    "vector",  "query",   "result",  "policy",  "support", "plan",   "refund",  "account",
    "service", "network", "memory",  "index",   "similar", "prompt", "context", "value",
    "user",    "update",  "simple",  "fast",    "stable",  "check",  "review",  "data"};

std::uint64_t fnv1a(std::string_view s) {
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::uint64_t splitmix64(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

int word_count(std::string_view s) {
  int n = 0;
  bool in_word = false;
  for (char c : s) {
    const bool space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
    if (!space && !in_word) ++n;
    in_word = !space;
  }
  return n;
}

std::string text_of(const nlohmann::json& content) {
  if (content.is_string()) return content.get<std::string>();
  std::string out;
  if (content.is_array()) {
    for (const auto& part : content) {
      if (part.is_object() && part.value("type", "") == "text") out += part.value("text", "");
    }
  }
  return out;
}

std::string hex16(std::uint64_t v) {
  std::ostringstream os;
  os << std::hex << std::setw(16) << std::setfill('0') << v;
  return os.str();
}

nlohmann::json chunk_json(const MockCompletion& c, nlohmann::json delta, nlohmann::json finish) {
  return {{"id", c.id},
          {"object", "chat.completion.chunk"},
          {"created", kCreated},
          {"model", c.model},
          {"choices", nlohmann::json::array({{{"index", 0}, {"delta", std::move(delta)}, {"finish_reason", std::move(finish)}}})}};
}

nlohmann::json usage_json(const MockCompletion& c) {
  return {{"prompt_tokens", c.prompt_tokens},
          {"completion_tokens", c.completion_tokens},
          {"total_tokens", c.prompt_tokens + c.completion_tokens}};
}

}  // namespace

std::string MockCompletion::content() const {
  std::string out;
  for (const auto& t : tokens) out += t;
  return out;
}

MockCompletion make_completion(const nlohmann::json& request, int response_words) {
  MockCompletion c;
  c.model = request.value("model", "mock-model");
  std::string last_user;
  for (const auto& m : request.value("messages", nlohmann::json::array())) {
    const auto text = text_of(m.value("content", nlohmann::json()));
    c.prompt_tokens += word_count(text);
    if (m.value("role", "") == "user") last_user = text;
  }
  const auto seed = fnv1a(c.model + "\n" + last_user);
  c.id = "chatcmpl-mock-" + hex16(seed);
  std::uint64_t state = seed;
  c.tokens.emplace_back("Mock");
  c.tokens.emplace_back(" answer:");
  for (int i = 0; i < response_words; ++i) {
    c.tokens.push_back(" " + std::string(kVocabulary[splitmix64(state) % kVocabulary.size()]));
  }
  c.completion_tokens = static_cast<int>(c.tokens.size());
  return c;
}

std::string completion_json(const MockCompletion& c) {
  nlohmann::json j = {
      {"id", c.id},
      {"object", "chat.completion"},
      {"created", kCreated},
      {"model", c.model},
      {"choices", nlohmann::json::array({{{"index", 0},
                                          {"message", {{"role", "assistant"}, {"content", c.content()}}},
                                          {"finish_reason", "stop"}}})},
      {"usage", usage_json(c)}};
  return j.dump();
}

std::vector<std::string> stream_events(const MockCompletion& c, bool include_usage) {
  auto event = [](const nlohmann::json& j) { return "data: " + j.dump() + "\n\n"; };
  std::vector<std::string> out;
  out.push_back(event(chunk_json(c, {{"role", "assistant"}, {"content", ""}}, nullptr)));
  for (const auto& t : c.tokens) out.push_back(event(chunk_json(c, {{"content", t}}, nullptr)));
  out.push_back(event(chunk_json(c, nlohmann::json::object(), "stop")));
  if (include_usage) {
    auto j = chunk_json(c, nlohmann::json::object(), nullptr);
    j["choices"] = nlohmann::json::array();
    j["usage"] = usage_json(c);
    out.push_back(event(j));
  }
  out.emplace_back("data: [DONE]\n\n");
  return out;
}

Fault parse_fault(std::string_view header) {
  Fault f;
  while (!header.empty()) {
    auto end = header.find(';');
    auto item = header.substr(0, end);
    header = end == std::string_view::npos ? std::string_view{} : header.substr(end + 1);
    auto eq = item.find('=');
    auto key = item.substr(0, eq);
    auto val = eq == std::string_view::npos ? std::string_view{} : item.substr(eq + 1);
    int n = 0;
    std::from_chars(val.data(), val.data() + val.size(), n);
    if (key == "status") f.status = static_cast<unsigned>(n);
    else if (key == "reset-after") f.reset_after = n;
    else if (key == "delay") f.delay_ms = n;
    else if (key == "hang") f.hang = true;
  }
  return f;
}

MockUpstream::MockUpstream(net::io_context& ioc, MockConfig cfg)
    : cfg_(std::move(cfg)),
      listener_(ioc, tcp::endpoint(net::ip::make_address(cfg_.host), cfg_.port),
                [this](beast::tcp_stream s, tcp::endpoint r) { return session(std::move(s), r); }) {}

std::string MockUpstream::last_authorization() const {
  std::lock_guard lock(mu_);
  return last_authorization_;
}

void MockUpstream::reset_stats() {
  chat_requests_.store(0);
  std::lock_guard lock(mu_);
  last_authorization_.clear();
}

Task<void> MockUpstream::session(beast::tcp_stream stream, tcp::endpoint /*remote*/) {
  beast::flat_buffer buffer;
  constexpr auto kWriteTimeout = std::chrono::seconds(30);
  for (;;) {
    Request req;
    stream.expires_never();
    auto [ec, n] = co_await http::async_read(stream, buffer, req, use_nothrow);
    if (ec) break;
    const auto target = std::string_view(req.target());
    BeastResponseWriter w(stream, req.version(), req.keep_alive(), kWriteTimeout);
    bool keep = req.keep_alive();
    if (target == "/v1/chat/completions" && req.method() == http::verb::post) {
      keep = co_await handle_chat(stream, req) && keep;
    } else if (target == "/mock/stats" && req.method() == http::verb::get) {
      nlohmann::json j = {{"chat_requests", chat_requests()}};
      keep = co_await w.send(200, kJson, j.dump(), kNoHeaders) && keep;
    } else if (target == "/mock/reset" && req.method() == http::verb::post) {
      reset_stats();
      keep = co_await w.send(200, kJson, R"({"ok":true})", kNoHeaders) && keep;
    } else if (target == "/healthz") {
      keep = co_await w.send(200, kJson, R"({"status":"ok"})", kNoHeaders) && keep;
    } else {
      keep = co_await w.send(404, kJson, openai_error_json("not found", "invalid_request_error"), kNoHeaders) && keep;
    }
    if (!keep) break;
  }
  beast::error_code ignored;
  stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
}

Task<bool> MockUpstream::handle_chat(beast::tcp_stream& stream, const Request& req) {
  chat_requests_.fetch_add(1);
  {
    std::lock_guard lock(mu_);
    last_authorization_ = std::string(req[http::field::authorization]);
  }
  BeastResponseWriter w(stream, req.version(), req.keep_alive(), std::chrono::seconds(30));
  auto executor = co_await net::this_coro::executor;
  net::steady_timer timer(executor);
  auto sleep = [&](std::chrono::milliseconds d) -> Task<void> {
    if (d.count() <= 0) co_return;
    timer.expires_after(d);
    co_await timer.async_wait(use_nothrow);
  };

  auto body = nlohmann::json::parse(req.body(), nullptr, false);
  Fault fault;
  if (cfg_.allow_faults && body.is_object() && body.contains("mock_fault") && body["mock_fault"].is_string()) {
    fault = parse_fault(body["mock_fault"].get<std::string>());
  }
  if (fault.hang) {
    timer.expires_after(kHangDuration);
    co_await timer.async_wait(use_nothrow);
    co_return false;
  }
  co_await sleep(cfg_.latency + std::chrono::milliseconds(fault.delay_ms));

  if (fault.status != 0) {
    co_return co_await w.send(fault.status, kJson, openai_error_json("injected fault", "server_error"), kNoHeaders);
  }

  if (!body.is_object() || !body.contains("messages")) {
    co_return co_await w.send(400, kJson, openai_error_json("invalid request", "invalid_request_error"), kNoHeaders);
  }
  const auto completion = make_completion(body, cfg_.response_words);
  const double cost = completion.prompt_tokens / 1000.0 * cfg_.usd_per_1k_input_tokens +
                      completion.completion_tokens / 1000.0 * cfg_.usd_per_1k_output_tokens;
  HeaderList headers = {{"x-request-id", completion.id}, {"X-Mock-Cost-Usd", std::to_string(cost)}};

  if (!body.value("stream", false)) {
    if (fault.reset_after >= 0) co_return false;  // drop the connection without answering
    co_return co_await w.send(200, kJson, completion_json(completion), headers);
  }

  const bool include_usage =
      body.contains("stream_options") && body["stream_options"].value("include_usage", false);
  const auto events = stream_events(completion, include_usage);
  if (!co_await w.begin_stream(200, "text/event-stream", headers)) co_return false;
  // events[0] is the role chunk; content chunks follow.
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (fault.reset_after >= 0 && static_cast<int>(i) > fault.reset_after) {
      beast::error_code ignored;
      stream.socket().close(ignored);  // abrupt: no terminating chunk
      co_return false;
    }
    if (i > 0 && i < events.size() - 1) co_await sleep(cfg_.token_delay);
    if (!co_await w.write_chunk(events[i])) co_return false;
  }
  co_return co_await w.end_stream();
}

}  // namespace semcache::mock
