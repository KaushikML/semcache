#include "web/assets.hpp"

#include <fstream>
#include <sstream>

namespace semcache::web {

std::optional<std::string> load(std::string_view name, const std::string& dir) {
  if (name.empty() || name.front() == '.' || name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos) {
    return std::nullopt;
  }
  if (dir.empty()) {
    auto data = embedded_file(name);
    if (!data) return std::nullopt;
    return std::string(*data);
  }
  std::ifstream in(dir + "/" + std::string(name), std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string_view content_type(std::string_view name) {
  if (name.ends_with(".html")) return "text/html; charset=utf-8";
  if (name.ends_with(".css")) return "text/css; charset=utf-8";
  if (name.ends_with(".js")) return "text/javascript; charset=utf-8";
  if (name.ends_with(".svg")) return "image/svg+xml";
  return "application/octet-stream";
}

}  // namespace semcache::web
