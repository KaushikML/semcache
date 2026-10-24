#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <sstream>

#include "config/config.hpp"

namespace semcache {
namespace {

EnvLookup fake_env(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    auto it = vars.find(std::string(name));
    if (it == vars.end()) return std::nullopt;
    return it->second;
  };
}

constexpr std::string_view kMinimal = R"(
[[tenants]]
id = "acme"
key_env = "KEY_ACME"
)";

TEST(Config, MinimalFileUsesDefaults) {
  auto cfg = load_config_from_string(kMinimal, fake_env({{"KEY_ACME", "sk-sc-acme"}}));
  EXPECT_EQ(cfg.mode, Mode::live);
  EXPECT_EQ(cfg.server.port, 8080);
  EXPECT_EQ(cfg.server.max_body_bytes, 1u << 20);
  EXPECT_TRUE(cfg.upstream.tls);
  EXPECT_EQ(cfg.upstream.host, "api.openai.com");
  EXPECT_EQ(cfg.upstream.port, 443);
  ASSERT_EQ(cfg.tenants.size(), 1u);
  EXPECT_EQ(cfg.tenants[0].id, "acme");
  EXPECT_EQ(cfg.tenants[0].key_sha256, sha256("sk-sc-acme"));
  EXPECT_FALSE(cfg.logging.log_prompts);
}

TEST(Config, FileValuesAreRead) {
  auto cfg = load_config_from_string(R"(
mode = "demo"
[server]
port = 9000
read_timeout_ms = 1234
[upstream]
base_url = "http://localhost:9090/prefix/"
[limits]
rps = 2.5
burst = 5
[[tenants]]
id = "acme"
key_env = "KEY_ACME"
)",
                                     fake_env({{"KEY_ACME", "k"}}));
  EXPECT_EQ(cfg.mode, Mode::demo);
  EXPECT_EQ(cfg.server.port, 9000);
  EXPECT_EQ(cfg.server.read_timeout, Millis(1234));
  EXPECT_FALSE(cfg.upstream.tls);
  EXPECT_EQ(cfg.upstream.host, "localhost");
  EXPECT_EQ(cfg.upstream.port, 9090);
  EXPECT_EQ(cfg.upstream.path_prefix, "/prefix");
  EXPECT_DOUBLE_EQ(cfg.limits.rps, 2.5);
  EXPECT_DOUBLE_EQ(cfg.limits.burst, 5.0);
}

TEST(Config, EnvOverridesFile) {
  auto cfg = load_config_from_string(std::string("[server]\nport = 9000\n[upstream]\nbase_url = \"http://127.0.0.1:9090\"\n") +
                                         std::string(kMinimal),
                                     fake_env({{"KEY_ACME", "k"},
                                               {"SEMCACHE_SERVER__PORT", "9999"},
                                               {"SEMCACHE_MODE", "demo"},
                                               {"SEMCACHE_LOGGING__LOG_PROMPTS", "true"},
                                               {"SEMCACHE_LIMITS__RPS", "7.5"}}));
  EXPECT_EQ(cfg.server.port, 9999);
  EXPECT_EQ(cfg.mode, Mode::demo);
  EXPECT_TRUE(cfg.logging.log_prompts);
  EXPECT_DOUBLE_EQ(cfg.limits.rps, 7.5);
}

constexpr std::string_view kDemoUpstream = "mode = \"demo\"\n[upstream]\nbase_url = \"http://127.0.0.1:9090\"\n";

TEST(Config, DemoModeAddsThePublicDemoTenantAndCapsMemory) {
  auto cfg = load_config_from_string(std::string(kDemoUpstream) + "[cache]\nmax_bytes = 999999999999\n" +
                                         "[demo]\npublic_key = \"sk-sc-pub\"\nmax_bytes = 1000000\n" +
                                         std::string(kMinimal),
                                     fake_env({{"KEY_ACME", "k"}}));
  ASSERT_EQ(cfg.tenants.size(), 2u);
  EXPECT_EQ(cfg.tenants[1].id, "demo");
  EXPECT_EQ(cfg.tenants[1].key_sha256, sha256("sk-sc-pub"));
  EXPECT_EQ(cfg.cache.max_bytes, 1000000u);
}

TEST(Config, DemoModeRefusesARealUpstream) {
  const auto env = fake_env({{"KEY_ACME", "k"}});
  try {
    (void)load_config_from_string(std::string("mode = \"demo\"\n[upstream]\nbase_url = \"https://api.openai.com\"\n") +
                                      std::string(kMinimal),
                                  env);
    FAIL() << "expected ConfigError";
  } catch (const ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("allowed_upstreams"), std::string::npos) << e.what();
  }
  // Allow-listing it explicitly works only without an upstream key.
  const auto custom = std::string("mode = \"demo\"\n[upstream]\nbase_url = \"http://mockhost:7000\"\n") +
                      "[demo]\nallowed_upstreams = [\"mockhost:7000\"]\n" + std::string(kMinimal);
  EXPECT_NO_THROW((void)load_config_from_string(custom, env));
  EXPECT_THROW((void)load_config_from_string(custom, fake_env({{"KEY_ACME", "k"}, {"SEMCACHE_UPSTREAM_API_KEY", "sk-real"}})),
               ConfigError);
}

TEST(Config, DemoModeValidatesItsLimits) {
  const auto env = fake_env({{"KEY_ACME", "k"}});
  for (const char* bad : {"public_key = \"\"", "per_ip_rps = 0", "per_ip_burst = 0", "max_prompt_chars = 0",
                          "max_tracked_ips = 0"}) {
    EXPECT_THROW((void)load_config_from_string(std::string(kDemoUpstream) + "[demo]\n" + bad + "\n" + std::string(kMinimal), env),
                 ConfigError)
        << bad;
  }
  // The same values are ignored in live mode, which has no demo tenant.
  auto live = load_config_from_string(std::string("[demo]\npublic_key = \"\"\n") + std::string(kMinimal), env);
  EXPECT_EQ(live.tenants.size(), 1u);
}

TEST(Config, StatsValuesAreValidated) {
  const auto env = fake_env({{"KEY_ACME", "k"}});
  auto cfg = load_config_from_string(std::string("[stats]\nstream_interval_ms = 250\nrecent_requests = 7\n") + std::string(kMinimal), env);
  EXPECT_EQ(cfg.stats.stream_interval, Millis(250));
  EXPECT_EQ(cfg.stats.recent_requests, 7u);
  EXPECT_THROW((void)load_config_from_string(std::string("[stats]\nmax_stream_clients = 0\n") + std::string(kMinimal), env),
               ConfigError);
}

TEST(Config, ShippedConfigFilesAreValid) {
  // SEMCACHE_MODEL_DIR is <repo>/models.
  const std::string repo = std::string(SEMCACHE_MODEL_DIR) + "/..";
  for (const char* name : {"semcache.example.toml", "dev.toml", "docker.toml", "docker-demo.toml"}) {
    std::ifstream in(repo + "/config/" + name);
    ASSERT_TRUE(in) << name;
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_NO_THROW((void)load_config_from_string(ss.str(), fake_env({{"SEMCACHE_KEY_ACME", "a"}, {"SEMCACHE_KEY_DEV", "d"}, {"SEMCACHE_KEY_DEFAULT", "k"}})))
        << name;
  }
}

TEST(Config, UpstreamKeyComesOnlyFromEnv) {
  auto cfg = load_config_from_string(kMinimal, fake_env({{"KEY_ACME", "k"}, {"SEMCACHE_UPSTREAM_API_KEY", "sk-up"}}));
  EXPECT_EQ(cfg.upstream.api_key.reveal(), "sk-up");
  EXPECT_THROW((void)load_config_from_string(std::string("[upstream]\napi_key = \"x\"\n") + std::string(kMinimal),
                                             fake_env({{"KEY_ACME", "k"}})),
               ConfigError);
}

TEST(Config, UnknownKeyIsRejected) {
  try {
    (void)load_config_from_string(std::string("[server]\nprot = 1\n") + std::string(kMinimal), fake_env({{"KEY_ACME", "k"}}));
    FAIL() << "expected ConfigError";
  } catch (const ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("server.prot"), std::string::npos);
  }
}

TEST(Config, WrongTypeIsRejected) {
  EXPECT_THROW((void)load_config_from_string(std::string("[server]\nport = \"x\"\n") + std::string(kMinimal),
                                             fake_env({{"KEY_ACME", "k"}})),
               ConfigError);
  EXPECT_THROW((void)load_config_from_string(kMinimal, fake_env({{"KEY_ACME", "k"}, {"SEMCACHE_SERVER__PORT", "abc"}})),
               ConfigError);
  EXPECT_THROW((void)load_config_from_string(kMinimal, fake_env({{"KEY_ACME", "k"}, {"SEMCACHE_SERVER__PORT", "70000"}})),
               ConfigError);
}

TEST(Config, MissingTenantKeyEnvNamesTheVariable) {
  try {
    (void)load_config_from_string(kMinimal, fake_env({}));
    FAIL() << "expected ConfigError";
  } catch (const ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("KEY_ACME"), std::string::npos);
  }
}

TEST(Config, ValidationErrors) {
  auto env = fake_env({{"KEY_ACME", "k"}, {"KEY_B", "k"}});
  EXPECT_THROW((void)load_config_from_string("", env), ConfigError);  // no tenants
  EXPECT_THROW((void)load_config_from_string(std::string("[upstream]\nbase_url = \"ftp://x\"\n") + std::string(kMinimal), env),
               ConfigError);
  EXPECT_THROW((void)load_config_from_string(std::string(kMinimal) + "[[tenants]]\nid = \"acme\"\nkey_env = \"KEY_B\"\n", env),
               ConfigError);  // duplicate id
  EXPECT_THROW((void)load_config_from_string(std::string(kMinimal) + "[[tenants]]\nid = \"b\"\nkey_env = \"KEY_B\"\n", env),
               ConfigError);  // same key value as acme
  EXPECT_THROW((void)load_config_from_string(std::string("[server]\nmax_inflight = 0\n") + std::string(kMinimal), env),
               ConfigError);
  EXPECT_THROW((void)load_config_from_string("mode = \"prod\"\n" + std::string(kMinimal), env), ConfigError);
}

TEST(Config, EmbedderSection) {
  auto env = fake_env({{"KEY_ACME", "k"}, {"SEMCACHE_EMBEDDER__MAX_TOKENS", "128"}});
  auto cfg = load_config_from_string(std::string("[embedder]\nmodel_path = \"/m.onnx\"\nvocab_path = \"/v.txt\"\n") +
                                         std::string(kMinimal),
                                     env);
  EXPECT_EQ(cfg.embedder.model_path, "/m.onnx");
  EXPECT_EQ(cfg.embedder.vocab_path, "/v.txt");
  EXPECT_EQ(cfg.embedder.max_tokens, 128u);
  EXPECT_EQ(cfg.embedder.intra_op_threads, 1);

  auto plain = fake_env({{"KEY_ACME", "k"}});
  EXPECT_THROW((void)load_config_from_string(std::string("[embedder]\nmax_tokens = 2\n") + std::string(kMinimal), plain),
               ConfigError);
  EXPECT_THROW(
      (void)load_config_from_string(std::string("[embedder]\nintra_op_threads = 0\n") + std::string(kMinimal), plain),
      ConfigError);
}

TEST(Config, IndexSection) {
  auto env = fake_env({{"KEY_ACME", "k"}, {"SEMCACHE_INDEX__HNSW_EF_SEARCH", "32"}});
  auto def = load_config_from_string(kMinimal, env);
  EXPECT_EQ(def.index.kind, IndexKind::automatic);
  EXPECT_EQ(def.index.hnsw_min_partition_size, 5000u) << "measured switch point (bench/results/phase5)";
  EXPECT_EQ(def.index.hnsw_ef_search, 32u) << "env override";
  auto brute = load_config_from_string(std::string("[index]\nkind = \"brute\"\n") + std::string(kMinimal), env);
  EXPECT_EQ(brute.index.kind, IndexKind::brute);
  auto plain = fake_env({{"KEY_ACME", "k"}});
  EXPECT_THROW((void)load_config_from_string(std::string("[index]\nkind = \"faiss\"\n") + std::string(kMinimal), plain),
               ConfigError);
  EXPECT_THROW((void)load_config_from_string(std::string("[index]\nhnsw_m = 1\n") + std::string(kMinimal), plain),
               ConfigError);
  EXPECT_THROW(
      (void)load_config_from_string(std::string("[index]\nhnsw_max_tombstone_ratio = 1.5\n") + std::string(kMinimal), plain),
      ConfigError);
}

TEST(Config, ParseErrorReportsLine) {
  try {
    (void)load_config_from_string("[server\nport = 1\n", fake_env({}));
    FAIL() << "expected ConfigError";
  } catch (const ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("line 1"), std::string::npos) << e.what();
  }
}

}  // namespace
}  // namespace semcache
