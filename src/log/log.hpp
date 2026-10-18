#pragma once

// Structured JSON-lines logging on top of spdlog. Each call emits one JSON object with ts and level
// added. Callers must not put secrets or (unless logging.log_prompts) prompt text into fields.

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <string_view>

namespace semcache::log {

void init(std::string_view level);

// Pure formatter behind event(); exposed for tests.
[[nodiscard]] std::string format_event(spdlog::level::level_enum level, std::string_view msg,
                                       const nlohmann::json& fields, std::chrono::system_clock::time_point now);

void event(spdlog::level::level_enum level, std::string_view msg, nlohmann::json fields = nlohmann::json::object());

inline void info(std::string_view msg, nlohmann::json fields = nlohmann::json::object()) {
  event(spdlog::level::info, msg, std::move(fields));
}
inline void warn(std::string_view msg, nlohmann::json fields = nlohmann::json::object()) {
  event(spdlog::level::warn, msg, std::move(fields));
}
inline void error(std::string_view msg, nlohmann::json fields = nlohmann::json::object()) {
  event(spdlog::level::err, msg, std::move(fields));
}

}  // namespace semcache::log
