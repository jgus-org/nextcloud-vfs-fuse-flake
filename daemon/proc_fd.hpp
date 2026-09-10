#pragma once

#include <unistd.h>

#include <array>
#include <climits>
#include <format>
#include <string>

namespace ncvfs {

class ProcFdPath {
 public:
  explicit ProcFdPath(int descriptor) {
    const auto written = std::format_to_n(text_.data(), text_.size() - 1, "/proc/self/fd/{}", descriptor);
    *written.out = '\0';
  }

  const char* c_str() const { return text_.data(); }

 private:
  std::array<char, 32> text_ = {};
};

inline std::string describe_fd(int descriptor) {
  const ProcFdPath link(descriptor);
  char target[PATH_MAX];
  const auto length = ::readlink(link.c_str(), target, sizeof target);
  if (length < 0) {
    return link.c_str();
  }
  return std::string(target, static_cast<size_t>(length));
}

}  // namespace ncvfs
