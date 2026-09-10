#pragma once

#include <fcntl.h>
#include <sys/stat.h>

#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <cstdint>

#include "protocol.hpp"

namespace ncvfs {

inline QString replacement_lock_path(const QString& store) {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
      .filePath(protocol::control_server_name(store) + QStringLiteral(".replacement-lock"));
}

inline off_t replacement_lock_offset(const struct stat& attributes) {
  auto hash = static_cast<uint64_t>(attributes.st_dev);
  hash ^= static_cast<uint64_t>(attributes.st_ino) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  return static_cast<off_t>(hash & 0x7fffffffffffffffULL);
}

inline int set_replacement_lock(int descriptor, const struct stat& attributes, short type) {
  struct flock lock = {
      .l_type = type,
      .l_whence = SEEK_SET,
      .l_start = replacement_lock_offset(attributes),
      .l_len = 1,
      .l_pid = 0,
  };
  return ::fcntl(descriptor, F_OFD_SETLK, &lock);
}

}  // namespace ncvfs
