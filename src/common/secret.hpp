#pragma once

#include <string>
#include <utility>

namespace semcache {

// Holds a credential. Deliberately has no stream/format support so it cannot end up in a log line
// by accident; callers must ask for the value explicitly with reveal().
class Secret {
 public:
  Secret() = default;
  explicit Secret(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] const std::string& reveal() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

 private:
  std::string value_;
};

}  // namespace semcache
