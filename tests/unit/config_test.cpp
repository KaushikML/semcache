#include <gtest/gtest.h>

#include <map>

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
  auto cfg = load_config_from_string(std::string("[server]\nport = 9000\n") + std::string(kMinimal),
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
