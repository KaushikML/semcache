#include "persist/files.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace semcache {
namespace {

std::string err(const std::string& what, const std::string& path) {
  return what + " " + path + ": " + std::strerror(errno);
}

}  // namespace

AppendFile::~AppendFile() { close(); }

AppendFile& AppendFile::operator=(AppendFile&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    path_ = std::move(o.path_);
    o.fd_ = -1;
  }
  return *this;
}

bool AppendFile::create(const std::string& path, std::string& error) {
  close();
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    error = err("open", path);
    return false;
  }
  path_ = path;
  return true;
}

bool AppendFile::write(std::string_view data, std::string& error) {
  while (!data.empty()) {
    const auto n = ::write(fd_, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      error = err("write", path_);
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

bool AppendFile::sync(std::string& error) {
  if (::fdatasync(fd_) != 0) {
    error = err("fdatasync", path_);
    return false;
  }
  return true;
}

void AppendFile::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool sync_dir(const std::string& dir, std::string& error) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    error = err("open dir", dir);
    return false;
  }
  const bool ok = ::fsync(fd) == 0;
  if (!ok) error = err("fsync dir", dir);
  ::close(fd);
  return ok;
}

}  // namespace semcache
