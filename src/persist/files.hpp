#pragma once

// Minimal POSIX file helpers for durable writes (write-all, fdatasync, atomic rename, directory
// fsync). Errors are returned as errno-style messages; callers decide (persistence disables
// itself on any write error, design §8 "disk full").

#include <string>

namespace semcache {

class AppendFile {
 public:
  AppendFile() = default;
  ~AppendFile();
  AppendFile(const AppendFile&) = delete;
  AppendFile& operator=(const AppendFile&) = delete;
  AppendFile(AppendFile&& o) noexcept : fd_(o.fd_), path_(std::move(o.path_)) { o.fd_ = -1; }
  AppendFile& operator=(AppendFile&& o) noexcept;

  // Creates (or truncates) `path`. Returns false and sets `error` on failure.
  bool create(const std::string& path, std::string& error);
  bool write(std::string_view data, std::string& error);
  bool sync(std::string& error);  // fdatasync
  void close();
  [[nodiscard]] bool is_open() const { return fd_ >= 0; }
  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  int fd_ = -1;
  std::string path_;
};

// fsync on a directory makes a rename/create inside it durable.
bool sync_dir(const std::string& dir, std::string& error);

}  // namespace semcache
