#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "web/assets.hpp"

namespace semcache::web {
namespace {

TEST(WebAssets, DashboardFilesAreEmbedded) {
  for (const char* name : {"index.html", "app.css", "app.js"}) {
    auto f = embedded_file(name);
    ASSERT_TRUE(f.has_value()) << name;
    EXPECT_FALSE(f->empty()) << name;
  }
  EXPECT_NE(embedded_file("index.html")->find("/dashboard/app.js"), std::string_view::npos);
  EXPECT_FALSE(embedded_file("missing.js").has_value());
}

TEST(WebAssets, NamesThatCouldLeaveTheDirectoryAreRefused) {
  for (const char* bad : {"../secret", "a/b.js", "..", ".hidden", "a\\b.js", ""}) {
    EXPECT_FALSE(load(bad, "").has_value()) << bad;
    EXPECT_FALSE(load(bad, "/tmp").has_value()) << bad;
  }
}

TEST(WebAssets, DirectoryOverridesEmbeddedFiles) {
  const auto dir = std::filesystem::temp_directory_path() / ("semcache_web_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "app.js") << "// dev copy";
  EXPECT_EQ(load("app.js", dir.string()), "// dev copy");
  EXPECT_FALSE(load("app.css", dir.string()).has_value()) << "no fallback to embedded when a dir is set";
  EXPECT_EQ(load("app.js", ""), std::string(*embedded_file("app.js")));
  std::filesystem::remove_all(dir);
}

TEST(WebAssets, ContentTypes) {
  EXPECT_EQ(content_type("index.html"), "text/html; charset=utf-8");
  EXPECT_EQ(content_type("app.css"), "text/css; charset=utf-8");
  EXPECT_EQ(content_type("app.js"), "text/javascript; charset=utf-8");
  EXPECT_EQ(content_type("x.svg"), "image/svg+xml");
}

}  // namespace
}  // namespace semcache::web
