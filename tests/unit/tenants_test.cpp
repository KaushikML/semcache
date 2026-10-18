#include <gtest/gtest.h>

#include "auth/tenants.hpp"

namespace semcache {
namespace {

TEST(Tenants, AuthenticatesByBearerKey) {
  TenantDirectory dir({{"acme", "E1", sha256("sk-a")}, {"beta", "E2", sha256("sk-b")}});
  auto a = dir.authenticate("Bearer sk-a");
  ASSERT_TRUE(a);
  EXPECT_EQ(a->id, "acme");
  auto b = dir.authenticate("Bearer sk-b");
  ASSERT_TRUE(b);
  EXPECT_EQ(b->id, "beta");
}

TEST(Tenants, RejectsBadHeaders) {
  TenantDirectory dir({{"acme", "E1", sha256("sk-a")}});
  EXPECT_FALSE(dir.authenticate(""));
  EXPECT_FALSE(dir.authenticate("sk-a"));
  EXPECT_FALSE(dir.authenticate("Basic sk-a"));
  EXPECT_FALSE(dir.authenticate("Bearer "));
  EXPECT_FALSE(dir.authenticate("Bearer sk-a "));
  EXPECT_FALSE(dir.authenticate("Bearer sk-unknown"));
}

}  // namespace
}  // namespace semcache
