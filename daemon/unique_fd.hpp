#pragma once

#include <dirent.h>
#include <unistd.h>

#include <memory>

namespace ncvfs {

class unique_fd {
 public:
  unique_fd() = default;
  explicit unique_fd(int descriptor) : descriptor_(descriptor) {}
  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;
  unique_fd(unique_fd&& other) noexcept : descriptor_(other.release()) {}
  unique_fd& operator=(unique_fd&& other) noexcept {
    reset(other.release());
    return *this;
  }
  ~unique_fd() { reset(); }

  int get() const { return descriptor_; }

  int release() {
    const int released = descriptor_;
    descriptor_ = -1;
    return released;
  }

  void reset(int descriptor = -1) {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
    descriptor_ = descriptor;
  }

  explicit operator bool() const { return descriptor_ >= 0; }

 private:
  int descriptor_ = -1;
};

struct directory_closer {
  void operator()(DIR* directory) const { ::closedir(directory); }
};

using unique_dir = std::unique_ptr<DIR, directory_closer>;

}  // namespace ncvfs
