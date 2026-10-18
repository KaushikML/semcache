#include "auth/tenants.hpp"

namespace semcache {

TenantDirectory::TenantDirectory(const std::vector<TenantConfig>& tenants) {
  for (const auto& t : tenants) by_key_digest_.emplace(t.key_sha256, Tenant{t.id});
}

std::optional<Tenant> TenantDirectory::authenticate(std::string_view authorization) const {
  constexpr std::string_view kBearer = "Bearer ";
  if (!authorization.starts_with(kBearer)) return std::nullopt;
  auto key = authorization.substr(kBearer.size());
  if (key.empty()) return std::nullopt;
  auto it = by_key_digest_.find(sha256(key));
  if (it == by_key_digest_.end()) return std::nullopt;
  return it->second;
}

}  // namespace semcache
