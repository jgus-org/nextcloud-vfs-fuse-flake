#pragma once

#include <fuse_log.h>
#include <sys/types.h>
#include <sys/xattr.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "proc_fd.hpp"

namespace ncvfs {

inline constexpr char placeholder_attribute[] = "user.nextcloud.vfs_fuse";
inline constexpr char incomplete_directory_attribute[] = "user.nextcloud.vfs_fuse.incomplete";
inline constexpr char support_probe_attribute[] = "user.nextcloud.vfs_fuse.probe";

inline constexpr std::chrono::seconds marker_warning_interval(60);

inline bool is_internal_attribute(std::string_view name) {
  return name == placeholder_attribute || name == incomplete_directory_attribute || name == support_probe_attribute;
}

inline std::expected<bool, int> directory_is_incomplete(int backing_fd) {
  const ProcFdPath backing_path(backing_fd);
  char value;
  const auto length = ::getxattr(backing_path.c_str(), incomplete_directory_attribute, &value, sizeof value);
  if (length >= 0) {
    return true;
  }
  if (errno == ENODATA) {
    return false;
  }
  return std::unexpected(errno);
}

class WarningThrottle {
 public:
  explicit WarningThrottle(std::chrono::steady_clock::duration interval) : interval_(interval) {}

  std::optional<uint64_t> admit() {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard guard(mutex_);
    if (now < next_) {
      ++withheld_;
      return std::nullopt;
    }
    next_ = now + interval_;
    return std::exchange(withheld_, 0);
  }

 private:
  std::mutex mutex_;
  std::chrono::steady_clock::duration interval_;
  std::chrono::steady_clock::time_point next_ = std::chrono::steady_clock::time_point::min();
  uint64_t withheld_ = 0;
};

inline std::string withheld_note(uint64_t withheld) {
  return withheld == 0 ? std::string() : std::format(" ({} like reports were withheld)", withheld);
}

inline WarningThrottle oversized_marker_warnings(marker_warning_interval);
inline WarningThrottle unreadable_marker_warnings(marker_warning_interval);
inline WarningThrottle malformed_marker_warnings(marker_warning_interval);

enum class PlaceholderMarkerError {
  oversized,
  unreadable,
  malformed,
};

using PlaceholderMarkerResult = std::expected<std::optional<off_t>, PlaceholderMarkerError>;

inline bool placeholder_withholds_content(const PlaceholderMarkerResult& marker) {
  return !marker || marker->has_value();
}

inline PlaceholderMarkerResult read_placeholder_marker(int backing_fd) {
  char value[64];
  const ProcFdPath backing_path(backing_fd);
  const auto length = ::getxattr(backing_path.c_str(), placeholder_attribute, value, sizeof value);
  if (length < 0) {
    const int failure = errno;
    if (failure == ENODATA) {
      return {};
    }
    if (failure == ERANGE) {
      if (const auto withheld = oversized_marker_warnings.admit()) {
        fuse_log(
            FUSE_LOG_WARNING,
            "%s carries a %s value longer than %zu bytes, so its remote size is unavailable%s\n",
            describe_fd(backing_fd).c_str(),
            placeholder_attribute,
            sizeof value,
            withheld_note(*withheld).c_str());
      }
      return std::unexpected(PlaceholderMarkerError::oversized);
    }
    if (const auto withheld = unreadable_marker_warnings.admit()) {
      fuse_log(
          FUSE_LOG_WARNING,
          "%s cannot be examined for %s: %s%s\n",
          describe_fd(backing_fd).c_str(),
          placeholder_attribute,
          std::strerror(failure),
          withheld_note(*withheld).c_str());
    }
    return std::unexpected(PlaceholderMarkerError::unreadable);
  }

  off_t remote_size = 0;
  const char* const end = value + length;
  const auto parsed = std::from_chars(value, end, remote_size);
  if (parsed.ec != std::errc{} || parsed.ptr != end || remote_size < 0) {
    if (const auto withheld = malformed_marker_warnings.admit()) {
      fuse_log(
          FUSE_LOG_WARNING,
          "%s carries a %s value that is not a non-negative decimal size, so its remote size is unavailable%s\n",
          describe_fd(backing_fd).c_str(),
          placeholder_attribute,
          withheld_note(*withheld).c_str());
    }
    return std::unexpected(PlaceholderMarkerError::malformed);
  }
  return std::optional<off_t>{remote_size};
}

}  // namespace ncvfs
