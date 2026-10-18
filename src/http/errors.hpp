#pragma once

#include <string>
#include <string_view>

namespace semcache {

// OpenAI-style error body: {"error":{"message":..., "type":..., "code":...}}
[[nodiscard]] std::string openai_error_json(std::string_view message, std::string_view type,
                                            std::string_view code = {});

}  // namespace semcache
