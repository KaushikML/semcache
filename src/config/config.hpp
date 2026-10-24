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
  std::string dashboard_dir;  // development: serve /dashboard files from disk; empty = embedded copy
};

struct StatsConfig {
  Millis stream_interval{1'000};       // /stats/stream event period
  std::size_t max_stream_clients = 32;  // concurrent /stats/stream connections; more => 503
  std::size_t recent_requests = 50;     // ring shown by /stats (never contains prompt text)
};

// Public demo (design §10): only the mock upstream, a public tenant key, per-IP limits.
struct DemoConfig {
  std::vector<std::string> allowed_upstreams = {"mock:9090", "127.0.0.1:9090", "localhost:9090"};
  std::string public_key = "sk-sc-demo-public";  // public by design; exists only in demo mode
  double per_ip_rps = 2;
  double per_ip_burst = 10;
  std::size_t max_prompt_chars = 500;
  std::size_t max_bytes = 64 << 20;  // cache budget in demo mode (caps cache.max_bytes)
  std::size_t max_tracked_ips = 10'000;  // per-IP buckets kept; idle ones are evicted first
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

struct EmbedderConfig {
  std::string model_path = "models/all-MiniLM-L6-v2.onnx";
  std::string vocab_path = "models/vocab.txt";
  // Longer prompts are not embedded (exact-match only), see design §5.4. 256 is the model's
  // training max_seq_length; it includes [CLS] and [SEP].
  std::size_t max_tokens = 256;
  int intra_op_threads = 1;  // ORT threads per Run; parallelism comes from the embed pool (ADR-005)
  int threads = 4;                    // embed pool size (design §7.1)
  std::size_t queue_capacity = 1024;  // pending embed jobs; when full, requests fail open (BYPASS)
};

using Seconds = std::chrono::seconds;

struct CacheConfig {
  bool enabled = true;
  // Measured in Phase 7 (bench/results/phase7): the lowest threshold with <= 1% false hits on
  // QQP's non-duplicate pairs for all-MiniLM-L6-v2. Lower it per deployment only with your own data.
  float similarity_threshold = 0.97f;
  int history_k = 2;                   // prior non-system messages hashed into the context key; -1 = all
  double max_temperature = 2.0;        // requests above this are not cached
  Seconds default_ttl{86'400};
  std::size_t max_bytes = std::size_t{1} << 30;      // global budget, design §5.1
  std::size_t max_response_bytes = 64 << 10;         // larger responses are served, not stored (§5.4)
  std::vector<std::string> store_finish_reasons = {"stop", "tool_calls"};
  std::size_t restream_chunk_chars = 16;  // re-streamed HIT chunk size (§6.5)
  Millis restream_delay{0};               // pause between re-streamed chunks
  Millis sweep_interval{60'000};          // active TTL expiry
  std::size_t writer_queue_capacity = 4096;  // pending inserts; when full, inserts are dropped (§7.4)
  // Tags (X-Semcache-Tags) a client may attach; invalid tags make the request BYPASS.
  std::size_t max_tags_per_entry = 16;
  std::size_t max_tag_bytes = 64;
  // How long a purge also blocks inserts from requests that started before it. Must exceed the
  // longest request (upstream.total_timeout_ms).
  Seconds purge_guard_window{300};
};

struct PersistenceConfig {
  bool enabled = true;
  std::string dir = "data";
  Seconds snapshot_interval{300};
  Millis fsync_interval{1000};  // at most this window of inserts is lost on a crash (design §3)
  Millis retry_interval{30'000};  // after a disk error, how often to try writing again
  std::size_t max_wal_bytes = std::size_t{256} << 20;  // a snapshot starts early past this
};

enum class IndexKind { brute, hnsw, automatic };

struct IndexConfig {
  IndexKind kind = IndexKind::automatic;
  // auto: a partition switches from brute force to HNSW at this many vectors (one-way). Set from the
  // Phase 5 measurements, see bench/results/phase5.
  std::size_t hnsw_min_partition_size = 5'000;
  std::size_t hnsw_m = 16;                // graph degree (layer 0 keeps 2M)
  std::size_t hnsw_ef_construction = 200;  // beam width while inserting
  std::size_t hnsw_ef_search = 64;         // beam width while searching (>= k)
  double hnsw_max_tombstone_ratio = 0.25;  // rebuild a partition's graph beyond this share of deletes
  std::uint64_t hnsw_seed = 42;            // level RNG seed: graphs are reproducible
};

struct CoalescerConfig {
  bool enabled = true;
  std::size_t max_waiters_per_flight = 1000;  // beyond this, a duplicate is served independently
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
  std::optional<std::size_t> max_bytes;  // cache quota; defaults to cache.max_bytes
  std::optional<Seconds> ttl;            // entry TTL; defaults to cache.default_ttl
};

struct Config {
  Mode mode = Mode::live;
  ServerConfig server;
  UpstreamConfig upstream;
  EmbedderConfig embedder;
  CacheConfig cache;
  CoalescerConfig coalescer;
  IndexConfig index;
  PersistenceConfig persistence;
  StatsConfig stats;
  DemoConfig demo;
  Secret admin_key;  // SEMCACHE_ADMIN_KEY only; empty => /admin/* disabled
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
