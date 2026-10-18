#pragma once

// Runtime configuration. Every tunable lives here with its default; nothing else in the code base
// hard-codes thresholds, timeouts or capacities. See docs/design.md §12 for the full schema.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/secret.hpp"
#include "common/sha256.hpp"

namespace semcache {

using Millis = std::chrono::milliseconds;

enum class Mode { live, demo };

struct ServerConfig {
  std::string host = "0.0.0.0";
  std::uint16_t port = 8080;
  int io_threads = 0;  // 0 = std::thread::hardware_concurrency()
  int max_inflight = 1024;
  std::size_t max_body_bytes = 1 << 20;  // 1 MiB, see design §5.4
  Millis read_timeout{10'000};           // whole request (headers + body), also keep-alive idle
  Millis write_timeout{30'000};          // per write to the client
  Millis shutdown_timeout{5'000};        // max wait for in-flight sessions to drain on SIGTERM
};

struct UpstreamConfig {
  std::string base_url = "https://api.openai.com";
  // Derived from base_url during validation.
  bool tls = true;
  std::string host = "api.openai.com";
  std::uint16_t port = 443;
  std::string path_prefix;  // e.g. "/openai" for gateways that mount the API under a prefix

  bool verify_tls = true;
  Millis connect_timeout{3'000};
  Millis first_byte_timeout{30'000};
  Millis idle_timeout{30'000};  // max gap between body reads
  Millis total_timeout{120'000};
  std::size_t max_response_bytes = 8 << 20;  // non-streaming bodies are buffered up to this

  Secret api_key;  // from SEMCACHE_UPSTREAM_API_KEY only, never from the file
};

struct LimitsConfig {
  double rps = 50.0;  // per-tenant token bucket refill rate
  double burst = 100.0;
};

struct LoggingConfig {
  std::string level = "info";
  bool log_prompts = false;
};

struct TenantConfig {
  std::string id;
  std::string key_env;      // name of the env var holding the client key
  Sha256Digest key_sha256;  // only the digest is retained
};

struct Config {
  Mode mode = Mode::live;
  ServerConfig server;
  UpstreamConfig upstream;
  LimitsConfig limits;
  LoggingConfig logging;
  std::vector<TenantConfig> tenants;
};

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

[[nodiscard]] EnvLookup process_env();

// Parses TOML text, applies SEMCACHE_<SECTION>__<KEY> env overrides, resolves secrets and
// validates. Throws ConfigError with a message naming the offending key (never a secret value).
[[nodiscard]] Config load_config_from_string(std::string_view toml_text, const EnvLookup& env);
[[nodiscard]] Config load_config_file(const std::string& path, const EnvLookup& env);

// Validates and fills derived fields (upstream host/port/tls). Called by the loaders; exposed so
// tests that build a Config in code get the same checks.
void finalize_config(Config& cfg);

}  // namespace semcache
