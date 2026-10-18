#include "config/config.hpp"

#include <boost/url/parse.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace semcache {
namespace {

std::string env_name(std::string_view section, std::string_view key) {
  std::string name = "SEMCACHE_";
  if (!section.empty()) {
    name.append(section);
    name.append("__");
  }
  name.append(key);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return name;
}

std::string dotted(std::string_view section, std::string_view key) {
  return section.empty() ? std::string(key) : std::string(section) + "." + std::string(key);
}

template <class Int>
Int parse_int(std::string_view text, const std::string& what) {
  Int value{};
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    throw ConfigError(what + ": expected an integer, got '" + std::string(text) + "'");
  }
  return value;
}

double parse_double(std::string_view text, const std::string& what) {
  // std::from_chars for double is available in libstdc++ 11+.
  double value{};
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    throw ConfigError(what + ": expected a number, got '" + std::string(text) + "'");
  }
  return value;
}

bool parse_bool(std::string_view text, const std::string& what) {
  if (text == "true" || text == "1") return true;
  if (text == "false" || text == "0") return false;
  throw ConfigError(what + ": expected true/false, got '" + std::string(text) + "'");
}

// Reads one scalar: env override first, then the TOML file, else leaves the default untouched.
// Records the key as known so unknown keys in the file can be reported as errors.
class Reader {
 public:
  Reader(const toml::table& root, const EnvLookup& env) : root_(root), env_(env) {}

  void read(std::string_view section, std::string_view key, std::string& out) {
    if (auto v = from_env(section, key)) {
      out = *v;
    } else if (auto node = lookup(section, key)) {
      auto s = node->value<std::string>();
      if (!s) throw ConfigError(dotted(section, key) + ": expected a string");
      out = *s;
    }
  }

  void read(std::string_view section, std::string_view key, bool& out) {
    if (auto v = from_env(section, key)) {
      out = parse_bool(*v, env_name(section, key));
    } else if (auto node = lookup(section, key)) {
      auto b = node->value<bool>();
      if (!b) throw ConfigError(dotted(section, key) + ": expected a boolean");
      out = *b;
    }
  }

  void read(std::string_view section, std::string_view key, double& out) {
    if (auto v = from_env(section, key)) {
      out = parse_double(*v, env_name(section, key));
    } else if (auto node = lookup(section, key)) {
      auto d = node->value<double>();  // toml++ converts integers to double here
      if (!d) throw ConfigError(dotted(section, key) + ": expected a number");
      out = *d;
    }
  }

  template <class Int>
    requires std::is_integral_v<Int>
  void read(std::string_view section, std::string_view key, Int& out) {
    std::int64_t raw{};
    if (auto v = from_env(section, key)) {
      raw = parse_int<std::int64_t>(*v, env_name(section, key));
    } else if (auto node = lookup(section, key)) {
      auto i = node->value<std::int64_t>();
      if (!i || !node->is_integer()) throw ConfigError(dotted(section, key) + ": expected an integer");
      raw = *i;
    } else {
      return;
    }
    if (raw < static_cast<std::int64_t>(std::numeric_limits<Int>::min()) ||
        static_cast<std::uint64_t>(std::max<std::int64_t>(raw, 0)) >
            static_cast<std::uint64_t>(std::numeric_limits<Int>::max())) {
      throw ConfigError(dotted(section, key) + ": value out of range");
    }
    out = static_cast<Int>(raw);
  }

  void read(std::string_view section, std::string_view key, Millis& out) {
    std::int64_t ms = out.count();
    read(section, key, ms);
    out = Millis{ms};
  }

  // Throws if the file contains keys this binary does not understand (typos would otherwise be
  // silently ignored and the default used).
  void reject_unknown_keys() const {
    for (const auto& [k, node] : root_) {
      std::string top(k.str());
      if (top == "tenants") continue;  // validated separately
      if (const auto* tbl = node.as_table()) {
        for (const auto& [sub, unused] : *tbl) {
          std::string full = top + "." + std::string(sub.str());
          if (!known_.contains(full)) throw ConfigError("unknown config key: " + full);
        }
      } else if (!known_.contains(top)) {
        throw ConfigError("unknown config key: " + top);
      }
    }
  }

 private:
  std::optional<std::string> from_env(std::string_view section, std::string_view key) {
    known_.insert(dotted(section, key));
    return env_(env_name(section, key));
  }

  std::optional<toml::node_view<const toml::node>> lookup(std::string_view section,
                                                          std::string_view key) const {
    auto view = section.empty() ? root_[key] : root_[section][key];
    if (!view) return std::nullopt;
    return view;
  }

  const toml::table& root_;
  const EnvLookup& env_;
  std::set<std::string> known_;
};

void read_tenants(const toml::table& root, const EnvLookup& env, Config& cfg) {
  const auto* arr = root["tenants"].as_array();
  if (arr == nullptr) return;
  for (const auto& node : *arr) {
    const auto* t = node.as_table();
    if (t == nullptr) throw ConfigError("tenants: each entry must be a table");
    for (const auto& [k, unused] : *t) {
      if (k != "id" && k != "key_env") throw ConfigError("unknown config key: tenants." + std::string(k.str()));
    }
    TenantConfig tc;
    tc.id = (*t)["id"].value_or(std::string{});
    tc.key_env = (*t)["key_env"].value_or(std::string{});
    if (tc.id.empty()) throw ConfigError("tenants: 'id' is required");
    if (tc.key_env.empty()) throw ConfigError("tenants." + tc.id + ": 'key_env' is required");
    auto key = env(tc.key_env);
    if (!key || key->empty()) {
      throw ConfigError("tenants." + tc.id + ": env var " + tc.key_env + " is not set");
    }
    tc.key_sha256 = sha256(*key);
    cfg.tenants.push_back(std::move(tc));
  }
}

}  // namespace

EnvLookup process_env() {
  return [](std::string_view name) -> std::optional<std::string> {
    const char* v = std::getenv(std::string(name).c_str());
    if (v == nullptr) return std::nullopt;
    return std::string(v);
  };
}

void finalize_config(Config& cfg) {
  auto& s = cfg.server;
  if (s.io_threads < 0) throw ConfigError("server.io_threads must be >= 0");
  if (s.max_inflight <= 0) throw ConfigError("server.max_inflight must be > 0");
  if (s.max_body_bytes == 0) throw ConfigError("server.max_body_bytes must be > 0");
  if (s.read_timeout.count() <= 0 || s.write_timeout.count() <= 0 || s.shutdown_timeout.count() <= 0) {
    throw ConfigError("server timeouts must be > 0");
  }

  auto& u = cfg.upstream;
  auto parsed = boost::urls::parse_uri(u.base_url);
  if (!parsed) throw ConfigError("upstream.base_url is not a valid URL: " + u.base_url);
  const auto& url = *parsed;
  if (url.scheme() == "https") {
    u.tls = true;
  } else if (url.scheme() == "http") {
    u.tls = false;
  } else {
    throw ConfigError("upstream.base_url scheme must be http or https");
  }
  u.host = std::string(url.host());
  if (u.host.empty()) throw ConfigError("upstream.base_url has no host");
  u.port = url.has_port() ? url.port_number() : static_cast<std::uint16_t>(u.tls ? 443 : 80);
  u.path_prefix = std::string(url.path());
  while (!u.path_prefix.empty() && u.path_prefix.back() == '/') u.path_prefix.pop_back();
  for (auto t : {u.connect_timeout, u.first_byte_timeout, u.idle_timeout, u.total_timeout}) {
    if (t.count() <= 0) throw ConfigError("upstream timeouts must be > 0");
  }
  if (u.max_response_bytes == 0) throw ConfigError("upstream.max_response_bytes must be > 0");

  if (cfg.limits.rps <= 0 || cfg.limits.burst < 1) {
    throw ConfigError("limits.rps must be > 0 and limits.burst >= 1");
  }

  if (cfg.tenants.empty()) throw ConfigError("at least one [[tenants]] entry is required");
  std::set<std::string> ids;
  std::set<Sha256Digest> keys;
  for (const auto& t : cfg.tenants) {
    if (!ids.insert(t.id).second) throw ConfigError("duplicate tenant id: " + t.id);
    if (!keys.insert(t.key_sha256).second) throw ConfigError("tenant " + t.id + " reuses another tenant's key");
  }
}

Config load_config_from_string(std::string_view toml_text, const EnvLookup& env) {
  toml::table root;
  try {
    root = toml::parse(toml_text);
  } catch (const toml::parse_error& e) {
    std::ostringstream msg;
    msg << "config parse error at line " << e.source().begin.line << ": " << e.description();
    throw ConfigError(msg.str());
  }

  Config cfg;
  Reader r(root, env);

  std::string mode = "live";
  r.read("", "mode", mode);
  if (mode == "live") {
    cfg.mode = Mode::live;
  } else if (mode == "demo") {
    cfg.mode = Mode::demo;
  } else {
    throw ConfigError("mode must be 'live' or 'demo'");
  }

  auto& s = cfg.server;
  r.read("server", "host", s.host);
  r.read("server", "port", s.port);
  r.read("server", "io_threads", s.io_threads);
  r.read("server", "max_inflight", s.max_inflight);
  r.read("server", "max_body_bytes", s.max_body_bytes);
  r.read("server", "read_timeout_ms", s.read_timeout);
  r.read("server", "write_timeout_ms", s.write_timeout);
  r.read("server", "shutdown_timeout_ms", s.shutdown_timeout);

  auto& u = cfg.upstream;
  r.read("upstream", "base_url", u.base_url);
  r.read("upstream", "verify_tls", u.verify_tls);
  r.read("upstream", "connect_timeout_ms", u.connect_timeout);
  r.read("upstream", "first_byte_timeout_ms", u.first_byte_timeout);
  r.read("upstream", "idle_timeout_ms", u.idle_timeout);
  r.read("upstream", "total_timeout_ms", u.total_timeout);
  r.read("upstream", "max_response_bytes", u.max_response_bytes);
  if (auto key = env("SEMCACHE_UPSTREAM_API_KEY"); key && !key->empty()) u.api_key = Secret(*key);

  r.read("limits", "rps", cfg.limits.rps);
  r.read("limits", "burst", cfg.limits.burst);

  r.read("logging", "level", cfg.logging.level);
  r.read("logging", "log_prompts", cfg.logging.log_prompts);

  r.reject_unknown_keys();
  read_tenants(root, env, cfg);
  finalize_config(cfg);
  return cfg;
}

Config load_config_file(const std::string& path, const EnvLookup& env) {
  std::ifstream in(path);
  if (!in) throw ConfigError("cannot open config file: " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return load_config_from_string(ss.str(), env);
}

}  // namespace semcache
