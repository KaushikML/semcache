#include "log/log.hpp"

#include <spdlog/sinks/stdout_sinks.h>

#include <chrono>
#include <format>

namespace semcache::log {

void init(std::string_view level) {
  auto logger = spdlog::stdout_logger_mt("semcache");
  logger->set_pattern("%v");
  logger->set_level(spdlog::level::from_str(std::string(level)));
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(logger);
}

std::string format_event(spdlog::level::level_enum level, std::string_view msg, const nlohmann::json& fields,
                         std::chrono::system_clock::time_point now) {
  // ordered_json keeps ts/level/msg first so lines read naturally.
  nlohmann::ordered_json line;
  line["ts"] = std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(now));
  const auto lvl = spdlog::level::to_string_view(level);
  line["level"] = std::string(lvl.data(), lvl.size());
  line["msg"] = msg;
  for (const auto& [k, v] : fields.items()) line[k] = v;
  return line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void event(spdlog::level::level_enum level, std::string_view msg, nlohmann::json fields) {
  if (!spdlog::should_log(level)) return;
  spdlog::log(level, "{}", format_event(level, msg, fields, std::chrono::system_clock::now()));
}

}  // namespace semcache::log
