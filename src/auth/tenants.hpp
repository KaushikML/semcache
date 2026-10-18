#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/sha256.hpp"
#include "config/config.hpp"

namespace semcache {

struct Tenant {
  std::string id;
};

// Maps client API keys to tenants. Only SHA-256 digests of keys are stored, and lookup is by
// digest, so the raw key is never compared byte-by-byte against secrets in memory.
class TenantDirectory {
 public:
  explicit TenantDirectory(const std::vector<TenantConfig>& tenants);

  // `authorization` is the raw Authorization header value ("Bearer <key>").
  [[nodiscard]] std::optional<Tenant> authenticate(std::string_view authorization) const;

 private:
  std::map<Sha256Digest, Tenant> by_key_digest_;
};

}  // namespace semcache
