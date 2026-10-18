#include "http/errors.hpp"

#include <nlohmann/json.hpp>

namespace semcache {

std::string openai_error_json(std::string_view message, std::string_view type, std::string_view code) {
  nlohmann::json err = {{"message", message}, {"type", type}};
  err["code"] = code.empty() ? nlohmann::json(nullptr) : nlohmann::json(code);
  return nlohmann::json{{"error", err}}.dump();
}

}  // namespace semcache
