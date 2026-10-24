#pragma once

// Dashboard files (web/*), compiled into the binary (ADR-010). In development,
// server.dashboard_dir serves the same names from disk instead.

#include <optional>
#include <string>
#include <string_view>

namespace semcache::web {

// Contents of web/<name> as embedded at build time.
[[nodiscard]] std::optional<std::string_view> embedded_file(std::string_view name);

// The file for `name` (a bare file name, e.g. "app.js"): from `dir` when set, else embedded.
// Names with a path separator or a leading dot are refused, so a request can never leave web/.
[[nodiscard]] std::optional<std::string> load(std::string_view name, const std::string& dir);

[[nodiscard]] std::string_view content_type(std::string_view name);

}  // namespace semcache::web
