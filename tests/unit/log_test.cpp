#include <gtest/gtest.h>

#include "log/log.hpp"

namespace semcache::log {
namespace {

TEST(Log, FormatsOneJsonObjectWithFixedLeadingKeys) {
  const auto t = std::chrono::sys_days{std::chrono::year{2026} / 1 / 2} + std::chrono::hours(3) +
                 std::chrono::milliseconds(4567);
  const auto line = format_event(spdlog::level::info, "request", {{"status", 200}, {"tenant", "acme"}}, t);
  EXPECT_EQ(line, R"({"ts":"2026-01-02T03:00:04.567Z","level":"info","msg":"request","status":200,"tenant":"acme"})");
}

TEST(Log, InvalidUtf8IsReplacedNotThrown) {
  const auto line = format_event(spdlog::level::warn, "x", {{"v", std::string("\xff")}}, {});
  EXPECT_NE(line.find(R"("level":"warning")"), std::string::npos);
  EXPECT_NE(line.find("\xEF\xBF\xBD"), std::string::npos);
}

}  // namespace
}  // namespace semcache::log
