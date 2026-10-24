#include "cache/context_key.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <memory>

namespace semcache {
namespace {

constexpr std::string_view kKeyVersion = "semcache-context-v1";

// Top-level fields that never change the generated answer. Everything else, including fields
// added to the API in the future, is part of the context key: unknown params must split
// partitions rather than collide.
constexpr std::array<std::string_view, 6> kNotInKey = {"messages", "stream", "stream_options",
                                                       "user",     "metadata", "store"};

bool is_system(const nlohmann::json& m) {
  if (!m.is_object()) return false;
  const auto role = m.value("role", "");
  return role == "system" || role == "developer";
}

void append_number(std::string& out, const nlohmann::json& j) {
  if (j.is_number_integer() && j.is_number_unsigned()) {
    out += std::to_string(j.get<std::uint64_t>());
  } else if (j.is_number_integer()) {
    out += std::to_string(j.get<std::int64_t>());
  } else {
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), j.get<double>());
    out.append(buf, ec == std::errc{} ? end : buf);
  }
}

void append_canonical(std::string& out, const nlohmann::json& j) {
  switch (j.type()) {
    case nlohmann::json::value_t::object: {
      out.push_back('{');
      bool first = true;
      for (const auto& [k, v] : j.items()) {  // nlohmann::json objects are std::map: sorted keys
        if (!first) out.push_back(',');
        first = false;
        out += nlohmann::json(k).dump();
        out.push_back(':');
        append_canonical(out, v);
      }
      out.push_back('}');
      break;
    }
    case nlohmann::json::value_t::array: {
      out.push_back('[');
      for (std::size_t i = 0; i < j.size(); ++i) {
        if (i > 0) out.push_back(',');
        append_canonical(out, j[i]);
      }
      out.push_back(']');
      break;
    }
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
    case nlohmann::json::value_t::number_float:
      append_number(out, j);
      break;
    default:  // string, bool, null, binary
      out += j.dump();
  }
}

// Length-prefixed fields so that ("ab","c") and ("a","bc") encode differently.
void append_field(std::string& out, std::string_view value) {
  const auto n = static_cast<std::uint64_t>(value.size());
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((n >> (8 * i)) & 0xff));
  out.append(value);
}

std::optional<std::string> text_content(const nlohmann::json& content) {
  if (content.is_string()) return content.get<std::string>();
  if (!content.is_array()) return std::nullopt;
  std::string text;
  for (const auto& part : content) {
    if (!part.is_object() || part.value("type", "") != "text" || !part.contains("text") || !part["text"].is_string()) {
      return std::nullopt;
    }
    text += part["text"].get<std::string>();
  }
  return text;
}

std::string nfc_trimmed(std::string_view s) {
  std::string out;
  std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> nfc(
      utf8proc_NFC(reinterpret_cast<const utf8proc_uint8_t*>(std::string(s).c_str())), &std::free);
  if (nfc) {
    out = reinterpret_cast<const char*>(nfc.get());
  } else {
    out = s;  // invalid UTF-8: hash the bytes as given
  }
  constexpr std::string_view kSpace = " \t\n\r\f\v";
  const auto b = out.find_first_not_of(kSpace);
  if (b == std::string::npos) return {};
  const auto e = out.find_last_not_of(kSpace);
  return out.substr(b, e - b + 1);
}

// The text behind the exact key (ADR-013): NFC + Unicode case folding, whitespace runs collapsed to
// one space, and a sentence-final run of . ? ! (… 。 ？ ！) dropped when it follows a letter. Nothing
// else is folded: internal punctuation ("1.2" vs "12", "C++"), a final mark after a digit ("5!"),
// accents and every word stay significant. Case adds no risk the model does not already take: it is
// uncased, so case variants embed identically. The model's own input (embed_text) is not folded.
std::string exact_key_text(std::string_view s) {
  utf8proc_uint8_t* folded = nullptr;
  const auto n = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(s.data()), static_cast<utf8proc_ssize_t>(s.size()),
                              &folded, static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
  std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(folded, &std::free);
  if (n < 0) return nfc_trimmed(s);  // invalid UTF-8: as before, no folding

  auto is_space = [](utf8proc_int32_t c) {
    const auto cat = utf8proc_category(c);
    return c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || cat == UTF8PROC_CATEGORY_ZS ||
           cat == UTF8PROC_CATEGORY_ZL || cat == UTF8PROC_CATEGORY_ZP;
  };
  std::vector<utf8proc_int32_t> cps;
  bool pending_space = false;
  for (utf8proc_ssize_t i = 0; i < n;) {
    utf8proc_int32_t c = 0;
    i += utf8proc_iterate(folded + i, n - i, &c);
    if (is_space(c)) {
      pending_space = !cps.empty();
      continue;
    }
    if (pending_space) cps.push_back(' ');
    pending_space = false;
    cps.push_back(c);
  }

  auto is_final_mark = [](utf8proc_int32_t c) {
    return c == '.' || c == '?' || c == '!' || c == 0x2026 || c == 0x3002 || c == 0xFF1F || c == 0xFF01;
  };
  auto is_letter = [](utf8proc_int32_t c) {
    const auto cat = utf8proc_category(c);
    return cat >= UTF8PROC_CATEGORY_LU && cat <= UTF8PROC_CATEGORY_LO;
  };
  std::size_t end = cps.size();
  while (end > 0 && is_final_mark(cps[end - 1])) --end;
  if (end < cps.size()) {
    std::size_t before = end;
    if (before > 0 && cps[before - 1] == ' ') --before;  // "policy ?"
    if (before > 0 && is_letter(cps[before - 1])) cps.resize(before);
  }

  std::string out;
  utf8proc_uint8_t buf[4];
  for (auto c : cps) out.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(utf8proc_encode_char(c, buf)));
  return out;
}

}  // namespace

std::string_view to_string(NotCacheable r) {
  switch (r) {
    case NotCacheable::cache_disabled: return "cache_disabled";
    case NotCacheable::bypass_header: return "bypass_header";
    case NotCacheable::last_not_user: return "last_not_user";
    case NotCacheable::non_text_content: return "non_text_content";
    case NotCacheable::multiple_choices: return "multiple_choices";
    case NotCacheable::logprobs: return "logprobs";
    case NotCacheable::temperature: return "temperature";
    case NotCacheable::invalid_tags: return "invalid_tags";
  }
  return "unknown";
}

std::optional<std::vector<std::string>> parse_tags(std::string_view header, const CacheConfig& cfg) {
  std::vector<std::string> tags;
  auto valid_char = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == ':' || c == '/' || c == '-';
  };
  while (!header.empty()) {
    const auto comma = header.find(',');
    auto item = header.substr(0, comma);
    header = comma == std::string_view::npos ? std::string_view{} : header.substr(comma + 1);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
    if (item.empty() || item.size() > cfg.max_tag_bytes || !std::all_of(item.begin(), item.end(), valid_char)) {
      return std::nullopt;
    }
    tags.emplace_back(item);
  }
  std::sort(tags.begin(), tags.end());
  tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
  if (tags.size() > cfg.max_tags_per_entry) return std::nullopt;
  return tags;
}

std::string canonical_json(const nlohmann::json& j) {
  std::string out;
  append_canonical(out, j);
  return out;
}

Classification classify_request(const nlohmann::json& body, std::string_view tenant_id, const CacheConfig& cfg,
                                bool bypass_header) {
  auto no = [](NotCacheable r) { return Classification{std::nullopt, r}; };
  if (!cfg.enabled) return no(NotCacheable::cache_disabled);
  if (bypass_header) return no(NotCacheable::bypass_header);

  if (auto it = body.find("n"); it != body.end() && !it->is_null() &&
                                !(it->is_number_integer() && it->get<std::int64_t>() == 1)) {
    return no(NotCacheable::multiple_choices);
  }
  if (auto it = body.find("logprobs"); it != body.end() && it->is_boolean() && it->get<bool>()) {
    return no(NotCacheable::logprobs);
  }
  if (auto it = body.find("temperature"); it != body.end() && it->is_number() &&
                                          it->get<double>() > cfg.max_temperature) {
    return no(NotCacheable::temperature);
  }

  const auto& messages = body["messages"];
  const auto& last = messages.back();
  if (!last.is_object() || last.value("role", "") != "user") return no(NotCacheable::last_not_user);
  auto text = text_content(last.value("content", nlohmann::json()));
  if (!text) return no(NotCacheable::non_text_content);

  std::string enc;
  append_field(enc, kKeyVersion);
  append_field(enc, tenant_id);

  nlohmann::json params = nlohmann::json::object();
  for (const auto& [k, v] : body.items()) {
    if (std::find(kNotInKey.begin(), kNotInKey.end(), k) == kNotInKey.end()) params[k] = v;
  }
  append_field(enc, canonical_json(params));

  // System/developer messages always count in full, wherever they appear.
  std::vector<const nlohmann::json*> system;
  std::vector<const nlohmann::json*> prior;
  for (std::size_t i = 0; i + 1 < messages.size(); ++i) {
    (is_system(messages[i]) ? system : prior).push_back(&messages[i]);
  }
  append_field(enc, std::to_string(system.size()));
  for (const auto* m : system) append_field(enc, canonical_json(*m));

  std::size_t first_prior = 0;
  if (cfg.history_k >= 0 && prior.size() > static_cast<std::size_t>(cfg.history_k)) {
    first_prior = prior.size() - static_cast<std::size_t>(cfg.history_k);
  }
  append_field(enc, std::to_string(prior.size() - first_prior));
  for (std::size_t i = first_prior; i < prior.size(); ++i) append_field(enc, canonical_json(*prior[i]));

  CacheableRequest r;
  r.context = sha256(enc);
  r.embed_text = nfc_trimmed(*text);
  r.prompt_hash = sha256(exact_key_text(*text));
  return Classification{std::move(r), {}};
}

}  // namespace semcache
