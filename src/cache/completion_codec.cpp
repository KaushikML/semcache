#include "cache/completion_codec.hpp"

#include <algorithm>

namespace semcache {
namespace {

constexpr std::string_view kDone = "[DONE]";

void append_or_set(std::optional<std::string>& out, const nlohmann::json& piece) {
  if (!piece.is_string()) return;
  if (!out) out.emplace();
  *out += piece.get<std::string>();
}

std::string sse(const nlohmann::json& j) { return "data: " + j.dump() + "\n\n"; }

nlohmann::json chunk(const nlohmann::json& stored, std::string_view id, std::int64_t created, nlohmann::json delta,
                     nlohmann::json finish_reason) {
  nlohmann::json c = {{"id", id},
                      {"object", "chat.completion.chunk"},
                      {"created", created},
                      {"model", stored.value("model", "")},
                      {"choices", nlohmann::json::array({{{"index", 0},
                                                          {"delta", std::move(delta)},
                                                          {"finish_reason", std::move(finish_reason)}}})}};
  if (stored.contains("system_fingerprint")) c["system_fingerprint"] = stored["system_fingerprint"];
  return c;
}

bool utf8_continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Pieces of about `n` bytes, cut after a space when possible; a run without spaces is cut at a
// code point boundary once it reaches 4n bytes.
std::vector<std::string_view> split_text(std::string_view text, std::size_t n) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const std::size_t len = i + 1 - start;
    const bool at_space = text[i] == ' ' && len >= n;
    const bool too_long = len >= 4 * n && (i + 1 == text.size() || !utf8_continuation(text[i + 1]));
    if (at_space || too_long) {
      out.push_back(text.substr(start, len));
      start = i + 1;
    }
  }
  if (start < text.size()) out.push_back(text.substr(start));
  return out;
}

}  // namespace

void StreamAssembler::feed(std::string_view bytes) {
  if (overflowed_ || bad_) return;
  seen_ += bytes.size();
  if (seen_ > max_bytes_) {
    overflowed_ = true;
    pending_.clear();
    return;
  }
  pending_.append(bytes);
  // Events end with a blank line; tolerate CRLF.
  for (;;) {
    auto end = pending_.find("\n\n");
    std::size_t sep = 2;
    if (auto crlf = pending_.find("\r\n\r\n"); crlf != std::string::npos && (end == std::string::npos || crlf < end)) {
      end = crlf;
      sep = 4;
    }
    if (end == std::string::npos) break;
    std::string event = pending_.substr(0, end);
    pending_.erase(0, end + sep);

    std::string data;  // multiple data: lines join with \n (SSE spec); other fields are ignored
    std::size_t pos = 0;
    while (pos <= event.size()) {
      auto nl = event.find('\n', pos);
      std::string_view line(event.data() + pos, (nl == std::string::npos ? event.size() : nl) - pos);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.starts_with("data:")) {
        line.remove_prefix(5);
        if (line.starts_with(' ')) line.remove_prefix(1);
        if (!data.empty()) data.push_back('\n');
        data.append(line);
      }
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    if (!data.empty()) on_event(data);
  }
}

void StreamAssembler::on_event(std::string_view data) {
  if (done_) return;
  if (data == kDone) {
    done_ = true;
    return;
  }
  auto j = nlohmann::json::parse(data, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object() || j.contains("error")) {
    bad_ = true;
    return;
  }
  for (const char* k : {"id", "created", "model", "system_fingerprint"}) {
    if (!head_.contains(k) && j.contains(k) && !j[k].is_null()) head_[k] = j[k];
  }
  if (j.contains("usage") && j["usage"].is_object()) usage_ = j["usage"];
  if (!j.contains("choices") || !j["choices"].is_array()) return;
  for (const auto& c : j["choices"]) {
    if (!c.is_object() || c.value("index", 0) != 0) continue;  // only n = 1 is cacheable
    if (c.contains("finish_reason") && !c["finish_reason"].is_null()) finish_reason_ = c["finish_reason"];
    if (!c.contains("delta") || !c["delta"].is_object()) continue;
    const auto& d = c["delta"];
    if (d.contains("role") && d["role"].is_string()) role_ = d["role"].get<std::string>();
    if (d.contains("content")) append_or_set(content_, d["content"]);
    if (d.contains("refusal")) append_or_set(refusal_, d["refusal"]);
    if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
      for (const auto& tc : d["tool_calls"]) {
        const auto idx = tc.value("index", 0);
        if (idx < 0) continue;
        while (tool_calls_.size() <= static_cast<std::size_t>(idx)) {
          tool_calls_.push_back({{"id", ""}, {"type", "function"}, {"function", {{"name", ""}, {"arguments", ""}}}});
        }
        auto& slot = tool_calls_[static_cast<std::size_t>(idx)];
        if (tc.contains("id") && tc["id"].is_string()) slot["id"] = tc["id"];
        if (tc.contains("type") && tc["type"].is_string()) slot["type"] = tc["type"];
        if (tc.contains("function") && tc["function"].is_object()) {
          const auto& f = tc["function"];
          if (f.contains("name") && f["name"].is_string()) {
            slot["function"]["name"] = slot["function"]["name"].get<std::string>() + f["name"].get<std::string>();
          }
          if (f.contains("arguments") && f["arguments"].is_string()) {
            slot["function"]["arguments"] =
                slot["function"]["arguments"].get<std::string>() + f["arguments"].get<std::string>();
          }
        }
      }
    }
  }
}

std::optional<nlohmann::json> StreamAssembler::completion() const {
  if (!done_ || bad_ || overflowed_ || finish_reason_.is_null()) return std::nullopt;
  nlohmann::json message = {{"role", role_}, {"content", content_ ? nlohmann::json(*content_) : nlohmann::json()}};
  if (refusal_) message["refusal"] = *refusal_;
  if (!tool_calls_.empty()) message["tool_calls"] = tool_calls_;
  nlohmann::json out = {{"id", head_.value("id", "")},
                        {"object", "chat.completion"},
                        {"created", head_.value("created", 0)},
                        {"model", head_.value("model", "")},
                        {"choices", nlohmann::json::array({{{"index", 0},
                                                            {"message", std::move(message)},
                                                            {"finish_reason", finish_reason_}}})}};
  if (head_.contains("system_fingerprint")) out["system_fingerprint"] = head_["system_fingerprint"];
  if (!usage_.is_null()) out["usage"] = usage_;
  return std::optional<nlohmann::json>(std::in_place, std::move(out));
}

std::optional<StorableCompletion> make_storable(const nlohmann::json& completion, const CacheConfig& cfg,
                                                std::string& why) {
  if (!completion.is_object() || !completion.contains("choices") || !completion["choices"].is_array() ||
      completion["choices"].size() != 1 || !completion["choices"][0].is_object()) {
    why = "shape";
    return std::nullopt;
  }
  const auto& finish = completion["choices"][0].value("finish_reason", nlohmann::json());
  if (!finish.is_string() || std::find(cfg.store_finish_reasons.begin(), cfg.store_finish_reasons.end(),
                                       finish.get<std::string>()) == cfg.store_finish_reasons.end()) {
    why = "finish_reason";
    return std::nullopt;
  }
  StorableCompletion s;
  s.json = completion.dump();
  if (s.json.size() > cfg.max_response_bytes) {
    why = "too_large";
    return std::nullopt;
  }
  if (completion.contains("usage") && completion["usage"].is_object()) {
    const auto& t = completion["usage"].value("total_tokens", nlohmann::json());
    if (t.is_number_integer()) s.total_tokens = t.get<std::int64_t>();
  }
  return s;
}

std::string hit_body(const std::string& stored, std::string_view id, std::int64_t created) {
  auto j = nlohmann::json::parse(stored);
  j["id"] = id;
  j["created"] = created;
  return j.dump();
}

std::vector<std::string> hit_stream_events(const std::string& stored_json, std::string_view id, std::int64_t created,
                                           std::size_t chunk_chars, bool include_usage) {
  const auto stored = nlohmann::json::parse(stored_json);
  const auto& choice = stored["choices"][0];
  const auto& message = choice["message"];
  std::vector<std::string> out;

  // First delta carries the role; content mirrors the stored type (null for tool-call-only answers).
  const bool has_text = message.contains("content") && message["content"].is_string();
  out.push_back(sse(chunk(stored, id, created,
                          {{"role", message.value("role", "assistant")},
                           {"content", has_text ? nlohmann::json("") : nlohmann::json()}},
                          nullptr)));
  if (has_text) {
    const auto text = message["content"].get<std::string>();
    for (auto piece : split_text(text, chunk_chars)) {
      out.push_back(sse(chunk(stored, id, created, {{"content", piece}}, nullptr)));
    }
  }
  if (message.contains("refusal") && message["refusal"].is_string()) {
    out.push_back(sse(chunk(stored, id, created, {{"refusal", message["refusal"]}}, nullptr)));
  }
  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    auto calls = message["tool_calls"];
    for (std::size_t i = 0; i < calls.size(); ++i) calls[i]["index"] = i;
    out.push_back(sse(chunk(stored, id, created, {{"tool_calls", calls}}, nullptr)));
  }
  out.push_back(sse(chunk(stored, id, created, nlohmann::json::object(), choice["finish_reason"])));
  if (include_usage && stored.contains("usage")) {
    auto u = chunk(stored, id, created, nlohmann::json::object(), nullptr);
    u["choices"] = nlohmann::json::array();
    u["usage"] = stored["usage"];
    out.push_back(sse(u));
  }
  out.emplace_back("data: [DONE]\n\n");
  return out;
}

}  // namespace semcache
