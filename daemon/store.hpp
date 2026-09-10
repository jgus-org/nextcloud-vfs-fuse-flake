#pragma once

#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <string_view>

#include "unique_fd.hpp"

namespace ncvfs {

struct StoreLayout {
  std::string store;
  std::string mount;
};

inline bool encloses(std::string_view directory, std::string_view candidate) {
  if (!candidate.starts_with(directory)) {
    return false;
  }
  if (directory == "/") {
    return candidate.size() > 1;
  }
  return candidate.size() > directory.size() && candidate[directory.size()] == '/';
}

inline std::expected<std::string, std::string> resolve_directory(const std::string& path, std::string_view role) {
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) == nullptr) {
    return std::unexpected(std::format("the {} path {} cannot be resolved: {}", role, path, std::strerror(errno)));
  }
  struct stat attributes = {};
  if (::stat(resolved, &attributes) != 0) {
    return std::unexpected(std::format("the {} path {} cannot be inspected: {}", role, path, std::strerror(errno)));
  }
  if (!S_ISDIR(attributes.st_mode)) {
    return std::unexpected(std::format("the {} path {} is not a directory", role, path));
  }
  return std::string(resolved);
}

inline std::expected<void, std::string> require_empty_directory(const std::string& path) {
  unique_dir entries(::opendir(path.c_str()));
  if (!entries) {
    return std::unexpected(std::format("the mount point {} cannot be read: {}", path, std::strerror(errno)));
  }

  errno = 0;
  while (const auto* entry = ::readdir(entries.get())) {
    if (std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..") {
      return std::unexpected(std::format("the mount point {} is not empty", path));
    }
  }
  if (errno != 0) {
    return std::unexpected(std::format("the mount point {} cannot be read: {}", path, std::strerror(errno)));
  }
  return {};
}

inline std::expected<StoreLayout, std::string> resolve_layout(const std::string& store, const std::string& mount) {
  const auto store_root = resolve_directory(store, "store");
  if (!store_root) {
    return std::unexpected(store_root.error());
  }
  const auto mount_root = resolve_directory(mount, "mount point");
  if (!mount_root) {
    return std::unexpected(mount_root.error());
  }

  if (*store_root == *mount_root) {
    return std::unexpected(std::format("the store and the mount point are both {}", *store_root));
  }
  if (encloses(*store_root, *mount_root)) {
    return std::unexpected(
        std::format(
            "the mount point {} lies inside the store {}, where the sync client would "
            "descend into it",
            *mount_root,
            *store_root));
  }
  if (encloses(*mount_root, *store_root)) {
    return std::unexpected(std::format("the store {} lies inside the mount point {}", *store_root, *mount_root));
  }

  const auto empty_mount = require_empty_directory(*mount_root);
  if (!empty_mount) {
    return std::unexpected(empty_mount.error());
  }

  return StoreLayout{.store = *store_root, .mount = *mount_root};
}

}  // namespace ncvfs
