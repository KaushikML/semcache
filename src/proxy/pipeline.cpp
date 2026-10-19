#include "proxy/pipeline.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <random>

#include "cache/completion_codec.hpp"
#include "http/errors.hpp"

namespace semcache {
namespace {

using Clock = RequestPipeline::Clock;

constexpr std::string_view kJson = "application/json";
constexpr std::string_view kEventStream = "text/event-stream";

double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string latency_ms(Clock::time_point since) {
  return std::to_string(static_cast<long long>(seconds_since(since) * 1000.0 + 0.5));
}

std::string format_similarity(float s) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(s));
  return buf;
}

HeaderList semcache_headers(CacheResult result, Clock::time_point received, std::optional<float> similarity) {
  HeaderList h = {{"X-Semcache", std::string(to_string(result))}, {"X-Semcache-Latency-Ms", latency_ms(received)}};
  if (similarity) h.emplace_back("X-Semcache-Similarity", format_similarity(*similarity));
  return h;
}

// A fresh id for each HIT, so clients never see two responses with the same id.
std::string new_completion_id() {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  char buf[48];
  std::snprintf(buf, sizeof(buf), "chatcmpl-sc-%016llx%08llx", static_cast<unsigned long long>(rng()),
                static_cast<unsigned long long>(rng() & 0xffffffffu));
  return buf;
}

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(CacheClock::now().time_since_epoch()).count();
}

// Relays the upstream response to the client: streamed bodies chunk by chunk, others buffered
// (bounded) so status, headers and body are sent together. Optionally reassembles a streamed body
// so it can be stored.
class ForwardSink final : public UpstreamSink {
 public:
  ForwardSink(ResponseWriter& out, const UpstreamConfig& cfg, CacheResult result, Clock::time_point received,
              std::optional<float> similarity, bool capture_stream)
      : out_(out), cfg_(cfg), result_(result), received_(received), similarity_(similarity) {
    if (capture_stream) assembler_.emplace(cfg.max_response_bytes);
  }

  Task<bool> on_head(const UpstreamResponseHead& head) override {
    head_ = head;
    streaming_ = head.content_type.starts_with(kEventStream);
    if (!streaming_) co_return true;
    auto headers = headers_now();
    headers.insert(headers.end(), head.passthrough_headers.begin(), head.passthrough_headers.end());
    client_ok_ = co_await out_.begin_stream(head.status, head.content_type, headers);
    co_return client_ok_;
  }

  Task<bool> on_body(std::string_view chunk) override {
    if (streaming_) {
      if (assembler_) assembler_->feed(chunk);
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
  // X-Semcache-* headers; latency is measured when they are sent.
  HeaderList headers_now() const { return semcache_headers(result_, received_, similarity_); }
  bool streaming() const { return streaming_; }
  bool client_ok() const { return client_ok_; }
  bool too_large() const { return too_large_; }
  const std::string& body() const { return body_; }
  std::string take_body() { return std::move(body_); }
  const std::optional<StreamAssembler>& assembler() const { return assembler_; }

 private:
  ResponseWriter& out_;
  const UpstreamConfig& cfg_;
  CacheResult result_;
  Clock::time_point received_;
  std::optional<float> similarity_;
  UpstreamResponseHead head_;
  bool streaming_ = false;
  bool client_ok_ = true;
  bool too_large_ = false;
  std::string body_;
  std::optional<StreamAssembler> assembler_;
};

// Only checks what we must understand to proxy and classify safely; upstream does full validation.
bool valid_chat_request(const nlohmann::json& j) {
  return j.is_object() && j.contains("messages") && j["messages"].is_array() && !j["messages"].empty();
}

bool wants_stream(const nlohmann::json& req) {
  auto it = req.find("stream");
  return it != req.end() && it->is_boolean() && it->get<bool>();
}

bool wants_usage(const nlohmann::json& req) {
  auto so = req.find("stream_options");
  if (so == req.end() || !so->is_object()) return false;
  auto iu = so->find("include_usage");
  return iu != so->end() && iu->is_boolean() && iu->get<bool>();
}

}  // namespace

struct RequestPipeline::Lookup {
  enum class Kind { hit, miss, bypass } kind = Kind::miss;
  EntryPtr entry;                      // hit
  float similarity = 0;                // hit
  std::optional<float> best;           // miss: best score found by a semantic search
  std::optional<Embedding> embedding;  // miss: stored with the entry (absent => exact-only)
  std::string bypass_reason;
};

RequestPipeline::RequestPipeline(const Config& cfg, Upstream& upstream, ProxyMetrics& metrics, CacheServices cache)
    : cfg_(cfg), upstream_(upstream), metrics_(metrics), cache_(cache) {}

Task<RequestPipeline::Lookup> RequestPipeline::lookup(const CacheableRequest& req) {
  Lookup out;
  const auto now = CacheClock::now();
  try {
    if (auto e = cache_.store->exact_lookup(req.context, req.prompt_hash, now)) {
      out.kind = Lookup::Kind::hit;
      out.entry = std::move(e);
      out.similarity = 1.0f;
      co_return out;
    }
  } catch (const std::exception&) {
    metrics_.internal_errors.inc();
    out.kind = Lookup::Kind::bypass;
    out.bypass_reason = "cache_error";
    co_return out;
  }
  if (cache_.embedder == nullptr || cache_.embed_pool == nullptr) co_return out;  // exact-only mode

  struct EmbedSearch {
    EmbedError error = EmbedError::none;
    Embedding vec{};
    NearestMatch match;
    double embed_s = 0;
    double lookup_s = 0;
  };
  // Runs on an embed pool thread: ORT Run and the index scan never block an I/O thread (§7.1).
  // `req` lives in this coroutine frame, which stays alive until the job completes.
  auto r = co_await cache_.embed_pool->run<EmbedSearch>([this, &req, now] {
    EmbedSearch s;
    const auto t0 = Clock::now();
    std::string_view text[] = {req.embed_text};
    auto e = cache_.embedder->embed(text);
    s.embed_s = seconds_since(t0);
    if (e.error != EmbedError::none) {
      s.error = e.error;
      return s;
    }
    s.vec = e.embeddings.at(0);
    const auto t1 = Clock::now();
    s.match = cache_.store->nearest(req.context, s.vec, now);
    s.lookup_s = seconds_since(t1);
    return s;
  });

  if (r.status == PoolStatus::rejected) {
    metrics_.embed_rejected.inc();
    out.kind = Lookup::Kind::bypass;
    out.bypass_reason = "embed_queue_full";
    co_return out;
  }
  if (r.status == PoolStatus::failed) {
    metrics_.internal_errors.inc();
    out.kind = Lookup::Kind::bypass;
    out.bypass_reason = "cache_error";
    co_return out;
  }
  auto& s = *r.value;
  metrics_.embed_seconds.observe(s.embed_s);
  if (s.error == EmbedError::too_long) co_return out;  // exact-only entry, never a truncated match (§5.4)
  if (s.error != EmbedError::none) {
    metrics_.embed_errors.inc();
    out.kind = Lookup::Kind::bypass;
    out.bypass_reason = "embed_error";
    co_return out;
  }
  metrics_.lookup_seconds.observe(s.lookup_s);
  if (s.match.entry) metrics_.best_similarity.observe(s.match.score);
  if (s.match.entry && s.match.score >= cfg_.cache.similarity_threshold) {
    out.kind = Lookup::Kind::hit;
    out.entry = std::move(s.match.entry);
    out.similarity = s.match.score;
    co_return out;
  }
  if (s.match.entry) out.best = s.match.score;
  out.embedding = s.vec;
  co_return out;
}

Task<PipelineOutcome> RequestPipeline::handle_chat(const Tenant& tenant, std::string body, bool bypass_header,
                                                   ResponseWriter& out, Clock::time_point received) {
  PipelineOutcome outcome;
  auto request = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (!valid_chat_request(request)) {
    metrics_.bad_requests.inc();
    outcome.status = 400;
    co_await out.send(400, kJson,
                      openai_error_json("request body must be a JSON object with a non-empty 'messages' array",
                                        "invalid_request_error"),
                      semcache_headers(CacheResult::bypass, received, std::nullopt));
    co_return outcome;
  }

  if (cache_.store == nullptr) {
    outcome.bypass_reason = "no_cache";
    co_await forward(std::move(body), CacheResult::bypass, std::nullopt, out, received, outcome);
    co_return outcome;
  }
  auto cls = classify_request(request, tenant.id, cfg_.cache, bypass_header);
  if (!cls.request) {
    metrics_.bypass[static_cast<std::size_t>(cls.reason)]->inc();
    outcome.bypass_reason = std::string(to_string(cls.reason));
    co_await forward(std::move(body), CacheResult::bypass, std::nullopt, out, received, outcome);
    co_return outcome;
  }

  auto lk = co_await lookup(*cls.request);
  if (lk.kind == Lookup::Kind::hit) {
    co_await serve_hit(*lk.entry, lk.similarity, request, out, received, outcome);
    if (outcome.result == CacheResult::hit) co_return outcome;
    lk.kind = Lookup::Kind::bypass;  // rendering the stored entry failed before anything was sent
    lk.bypass_reason = "cache_error";
  }
  if (lk.kind == Lookup::Kind::bypass) {
    outcome.bypass_reason = lk.bypass_reason;
    co_await forward(std::move(body), CacheResult::bypass, std::nullopt, out, received, outcome);
    co_return outcome;
  }

  outcome.similarity = lk.best;
  NewEntry entry;
  entry.context = cls.request->context;
  entry.prompt_hash = cls.request->prompt_hash;
  entry.tenant = tenant.id;
  entry.prompt = std::move(cls.request->embed_text);
  entry.embedding = lk.embedding;
  co_await forward(std::move(body), CacheResult::miss, std::move(entry), out, received, outcome);
  co_return outcome;
}

Task<void> RequestPipeline::serve_hit(const CacheEntry& entry, float similarity, const nlohmann::json& request,
                                      ResponseWriter& out, Clock::time_point received, PipelineOutcome& outcome) {
  const bool stream = wants_stream(request);
  const auto id = new_completion_id();
  const auto created = unix_now();
  // Render before sending anything, so a bad entry can still fail open to upstream.
  std::string body;
  std::vector<std::string> events;
  try {
    if (stream) {
      events = hit_stream_events(entry.response, id, created, cfg_.cache.restream_chunk_chars, wants_usage(request));
    } else {
      body = hit_body(entry.response, id, created);
    }
  } catch (const std::exception&) {
    metrics_.internal_errors.inc();
    co_return;
  }

  CacheStore::touch(entry);
  if (entry.total_tokens > 0) metrics_.tokens_saved.inc(static_cast<std::uint64_t>(entry.total_tokens));
  outcome.result = CacheResult::hit;
  outcome.status = 200;
  outcome.similarity = similarity;
  outcome.streamed = stream;
  const auto headers = semcache_headers(CacheResult::hit, received, similarity);

  if (!stream) {
    outcome.keep_alive = co_await out.send(200, kJson, std::move(body), headers);
    co_return;
  }
  if (!co_await out.begin_stream(200, kEventStream, headers)) {
    outcome.keep_alive = false;
    co_return;
  }
  net::steady_timer pause(co_await net::this_coro::executor);
  for (const auto& e : events) {
    if (cfg_.cache.restream_delay.count() > 0) {
      pause.expires_after(cfg_.cache.restream_delay);
      co_await pause.async_wait(use_nothrow);
    }
    if (!co_await out.write_chunk(e)) {
      metrics_.client_disconnects.inc();
      outcome.keep_alive = false;
      co_return;
    }
  }
  // Headers went out before the body, so report total time in-band (design §6.5).
  co_await out.write_chunk(": semcache-total-ms=" + latency_ms(received) + "\n\n");
  outcome.keep_alive = co_await out.end_stream();
}

Task<void> RequestPipeline::forward(std::string body, CacheResult result, std::optional<NewEntry> store_as,
                                    ResponseWriter& out, Clock::time_point received, PipelineOutcome& outcome) {
  outcome.result = result;
  ForwardSink sink(out, cfg_.upstream, result, received, outcome.similarity, store_as.has_value());
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
    co_return;
  }

  // The upstream response as a completion, if this exchange may be stored.
  std::optional<nlohmann::json> completion;
  auto skip = [&](std::string_view why) {
    if (!store_as) return;
    outcome.store = why;
    metrics_.store_skipped(why).inc();
    store_as.reset();
  };

  if (sink.streaming()) {
    outcome.status = sink.head().status;
    if (r.error == UpstreamError::none) {
      outcome.keep_alive = co_await out.end_stream();
      if (store_as && outcome.status == 200) {
        completion = sink.assembler()->completion();
        if (!completion) skip(sink.assembler()->overflowed() ? "too_large" : "incomplete");
      }
    } else {
      // Headers (200) are already on the wire: report the failure in-band, then close.
      auto event = "data: " + openai_error_json("upstream stream interrupted: " + std::string(to_string(r.error)),
                                                "upstream_error") + "\n\n";
      co_await out.write_chunk(event);
      co_await out.end_stream();
      outcome.keep_alive = false;
      skip("incomplete");
    }
  } else if (r.error == UpstreamError::none) {
    outcome.status = sink.head().status;
    if (store_as && outcome.status == 200) {
      auto j = nlohmann::json::parse(sink.body(), nullptr, /*allow_exceptions=*/false);
      if (j.is_discarded()) {
        skip("shape");
      } else {
        completion = std::move(j);
      }
    }
    auto headers = sink.headers_now();
    headers.insert(headers.end(), sink.head().passthrough_headers.begin(), sink.head().passthrough_headers.end());
    auto ct = sink.head().content_type.empty() ? std::string(kJson) : sink.head().content_type;
    outcome.keep_alive = co_await out.send(outcome.status, ct, sink.take_body(), headers);
  } else {
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
    co_await out.send(outcome.status, kJson, openai_error_json(message, "upstream_error", code), sink.headers_now());
    skip("upstream_error");
    co_return;
  }

  if (!store_as) co_return;
  if (outcome.status != 200) {
    skip("status");
    co_return;
  }
  std::string why;
  auto storable = completion ? make_storable(*completion, cfg_.cache, why) : std::nullopt;
  if (!storable) {
    if (completion) skip(why);
    co_return;
  }
  store_as->response = std::move(storable->json);
  store_as->total_tokens = storable->total_tokens;
  try {
    // Phase 3: synchronous insert under the writer mutex (short, no I/O). Phase 4 moves it to the
    // writer thread.
    auto res = cache_.store->insert(std::move(*store_as), CacheClock::now());
    outcome.store = res == InsertOutcome::stored ? "stored" : "too_large";
  } catch (const std::exception&) {
    metrics_.internal_errors.inc();
    outcome.store = "error";
  }
}

}  // namespace semcache
