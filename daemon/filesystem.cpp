#include "filesystem.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <QFile>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "placeholder.hpp"
#include "proc_fd.hpp"
#include "replacement_lock.hpp"
#include "unique_fd.hpp"

namespace ncvfs {

namespace {

constexpr double attribute_cache_seconds = 0.0;
constexpr double entry_cache_seconds = 0.0;
constexpr uint32_t preferred_write_size = 4 * 1024 * 1024;

struct FileHandle {
  unique_fd descriptor;
  unique_fd replacement_lease;
};

struct DirectoryHandle {
  std::mutex mutex;
  unique_dir stream;
  off_t offset = 0;
};

Filesystem& filesystem(fuse_req_t request) { return *static_cast<Filesystem*>(fuse_req_userdata(request)); }

unique_fd borrow_backing_or_reply(fuse_req_t request, fuse_ino_t identifier) {
  unique_fd backing = filesystem(request).borrow_backing(identifier);
  if (!backing) {
    fuse_reply_err(request, ESTALE);
  }
  return backing;
}

unique_fd borrow_populated_directory_or_reply(fuse_req_t request, fuse_ino_t identifier) {
  auto backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return {};
  }
  const int error = filesystem(request).populate(backing.get());
  if (error != 0) {
    fuse_reply_err(request, error);
    return {};
  }
  return backing;
}

FileHandle& file_handle(fuse_file_info* info) { return *reinterpret_cast<FileHandle*>(info->fh); }

DirectoryHandle& directory_handle(fuse_file_info* info) { return *reinterpret_cast<DirectoryHandle*>(info->fh); }

bool is_dot_or_dotdot(const char* name) {
  return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

bool equal_case_insensitive(char left, char right) {
  return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
}

bool starts_with_case_insensitive(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin(), equal_case_insensitive);
}

bool contains_case_insensitive(std::string_view value, std::string_view part) {
  return std::search(value.begin(), value.end(), part.begin(), part.end(), equal_case_insensitive) != value.end();
}

bool is_sync_database_entry(std::string_view name) {
  return contains_case_insensitive(name, ".db") &&
         (starts_with_case_insensitive(name, ".sync_") || starts_with_case_insensitive(name, "._sync_") ||
          starts_with_case_insensitive(name, ".csync_journal.db"));
}

bool reject_sync_database_entry(fuse_req_t request, const char* name, int error) {
  if (!is_sync_database_entry(name)) {
    return false;
  }
  fuse_reply_err(request, error);
  return true;
}

void override_placeholder_size(struct stat& attributes, int backing_fd) {
  if (!S_ISREG(attributes.st_mode)) {
    return;
  }
  const auto marker = read_placeholder_marker(backing_fd);
  if (!marker || !marker->has_value()) {
    return;
  }
  attributes.st_size = marker->value();
  attributes.st_blocks = 0;
}

int attach_passthrough(fuse_req_t request, Filesystem& fs, Inode& node, int descriptor, fuse_file_info* info) {
  if (node.backing_id == 0) {
    node.backing_id = fuse_passthrough_open(request, descriptor);
    if (node.backing_id == 0) {
      const int failure = errno == 0 ? EIO : errno;
      if (fs.passthrough_required()) {
        fuse_log(
            FUSE_LOG_ERR,
            "the kernel refused the required backing file for inode %ju: %s\n",
            static_cast<uintmax_t>(node.number),
            std::strerror(failure));
        fs.fail_passthrough();
        return failure;
      }
      fuse_log(
          FUSE_LOG_WARNING,
          "the kernel refused a backing file for inode %ju, so reads and writes run through the daemon\n",
          static_cast<uintmax_t>(node.number));
      fs.disable_passthrough();
      return 0;
    }
  }
  info->backing_id = node.backing_id;
  return 0;
}

int apply_requested_truncation(int flags, int descriptor, bool withholds_content) {
  if ((flags & O_TRUNC) == 0) {
    return 0;
  }
  if (withholds_content) {
    return EIO;
  }
  const ProcFdPath opened_path(descriptor);
  return ::truncate(opened_path.c_str(), 0) == 0 ? 0 : errno;
}

struct OpenedForCreation {
  unique_fd descriptor;
  bool created = false;
};

std::expected<OpenedForCreation, int> open_for_creation(int parent_backing, const char* name, int flags, mode_t mode) {
  const int reopen_flags = flags & ~(O_NOFOLLOW | O_TRUNC);
  OpenedForCreation opened;

  opened.descriptor.reset(::openat(parent_backing, name, reopen_flags | O_CREAT | O_EXCL, mode));
  if (opened.descriptor) {
    opened.created = true;
    return opened;
  }
  if (errno != EEXIST || (flags & O_EXCL) != 0) {
    const int failure = errno;
    return std::unexpected(failure);
  }

  opened.descriptor.reset(::openat(parent_backing, name, reopen_flags, mode));
  if (!opened.descriptor) {
    const int failure = errno;
    return std::unexpected(failure);
  }
  return opened;
}

void discard_creation(int parent_backing, const char* name, bool created) {
  if (created) {
    ::unlinkat(parent_backing, name, 0);
  }
}

int begin_open(
    fuse_req_t request,
    Filesystem& fs,
    Inode& node,
    int descriptor,
    bool withholds_content,
    fuse_file_info* info) {
  info->parallel_direct_writes = 1;
  info->keep_cache = 0;
  info->direct_io = static_cast<unsigned int>(withholds_content);

  const std::lock_guard guard(node.mutex);
  ++node.open_count;
  if (!withholds_content && fs.passthrough()) {
    const int error = attach_passthrough(request, fs, node, descriptor, info);
    if (error != 0) {
      --node.open_count;
      return error;
    }
  }
  return 0;
}

int acquire_replacement_lease(Filesystem& fs, FileHandle& handle) {
  auto lease = fs.acquire_replacement_lease(handle.descriptor.get());
  if (!lease) {
    return lease.error();
  }
  handle.replacement_lease = std::move(*lease);
  return 0;
}

std::expected<unique_fd, int> acquire_replacement_lease(Filesystem& fs, int descriptor) {
  return fs.acquire_replacement_lease(descriptor);
}

void reply_attributes(fuse_req_t request, int backing) {
  struct stat attributes = {};
  if (::fstatat(backing, "", &attributes, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) != 0) {
    fuse_reply_err(request, errno);
    return;
  }
  override_placeholder_size(attributes, backing);
  fuse_reply_attr(request, &attributes, attribute_cache_seconds);
}

int apply_attributes(int backing, const struct stat& attributes, int valid, fuse_file_info* info) {
  const ProcFdPath backing_path(backing);

  if ((valid & FUSE_SET_ATTR_MODE) != 0) {
    const int result = info != nullptr ? ::fchmod(file_handle(info).descriptor.get(), attributes.st_mode)
                                       : ::chmod(backing_path.c_str(), attributes.st_mode);
    if (result != 0) {
      return errno;
    }
  }

  if ((valid & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) != 0) {
    const uid_t owner = (valid & FUSE_SET_ATTR_UID) != 0 ? attributes.st_uid : static_cast<uid_t>(-1);
    const gid_t group = (valid & FUSE_SET_ATTR_GID) != 0 ? attributes.st_gid : static_cast<gid_t>(-1);
    if (::fchownat(backing, "", owner, group, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) != 0) {
      return errno;
    }
  }

  if ((valid & FUSE_SET_ATTR_SIZE) != 0) {
    const int result = info != nullptr ? ::ftruncate(file_handle(info).descriptor.get(), attributes.st_size)
                                       : ::truncate(backing_path.c_str(), attributes.st_size);
    if (result != 0) {
      return errno;
    }
  }

  if ((valid & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) != 0) {
    struct timespec times[2] = {{.tv_sec = 0, .tv_nsec = UTIME_OMIT}, {.tv_sec = 0, .tv_nsec = UTIME_OMIT}};
    if ((valid & FUSE_SET_ATTR_ATIME_NOW) != 0) {
      times[0].tv_nsec = UTIME_NOW;
    } else if ((valid & FUSE_SET_ATTR_ATIME) != 0) {
      times[0] = attributes.st_atim;
    }
    if ((valid & FUSE_SET_ATTR_MTIME_NOW) != 0) {
      times[1].tv_nsec = UTIME_NOW;
    } else if ((valid & FUSE_SET_ATTR_MTIME) != 0) {
      times[1] = attributes.st_mtim;
    }
    const int result = info != nullptr ? ::futimens(file_handle(info).descriptor.get(), times)
                                       : ::utimensat(AT_FDCWD, backing_path.c_str(), times, 0);
    if (result != 0) {
      return errno;
    }
  }

  return 0;
}

std::expected<std::vector<char>, int> read_attribute_names(const char* backing_path) {
  for (;;) {
    const auto expected_size = ::listxattr(backing_path, nullptr, 0);
    if (expected_size < 0) {
      return std::unexpected(errno);
    }
    std::vector<char> names(static_cast<size_t>(expected_size));
    if (names.empty()) {
      return names;
    }
    const auto listed = ::listxattr(backing_path, names.data(), names.size());
    if (listed >= 0 && static_cast<size_t>(listed) <= names.size()) {
      names.resize(static_cast<size_t>(listed));
      return names;
    }
    if (listed >= 0) {
      continue;
    }
    if (errno != ERANGE) {
      return std::unexpected(errno);
    }
  }
}

std::string visible_attribute_names(std::span<const char> names) {
  std::string visible;
  for (size_t start = 0; start < names.size();) {
    const std::string_view name(names.data() + start);
    start += name.size() + 1;
    if (is_internal_attribute(name)) {
      continue;
    }
    visible.append(name);
    visible.push_back('\0');
  }
  return visible;
}

void op_init(void* userdata, fuse_conn_info* connection) {
  auto& fs = *static_cast<Filesystem*>(userdata);

  if (fs.passthrough() && !fuse_set_feature_flag(connection, FUSE_CAP_PASSTHROUGH)) {
    fs.disable_passthrough();
    if (fs.passthrough_required()) {
      fuse_log(FUSE_LOG_ERR, "the kernel does not offer the required FUSE_CAP_PASSTHROUGH feature\n");
      fs.fail_passthrough();
    } else {
      fuse_log(
          FUSE_LOG_WARNING,
          "the kernel does not offer FUSE_CAP_PASSTHROUGH, so hydrated files are read through the daemon\n");
    }
  }

  fuse_set_feature_flag(connection, FUSE_CAP_FLOCK_LOCKS);
  fuse_set_feature_flag(connection, FUSE_CAP_SPLICE_READ);
  fuse_set_feature_flag(connection, FUSE_CAP_SPLICE_WRITE);
  fuse_set_feature_flag(connection, FUSE_CAP_SPLICE_MOVE);
  fuse_set_feature_flag(connection, FUSE_CAP_DIRECT_IO_ALLOW_MMAP);
  fuse_set_feature_flag(connection, FUSE_CAP_NO_EXPORT_SUPPORT);

  connection->no_interrupt = 1;
  connection->max_write = preferred_write_size;
}

void reply_entry(fuse_req_t request, Filesystem& fs, const fuse_entry_param& entry) {
  if (fuse_reply_entry(request, &entry) != 0) {
    fs.forget_one(entry.ino, 1);
  }
}

void op_lookup(fuse_req_t request, fuse_ino_t parent, const char* name) {
  if (reject_sync_database_entry(request, name, ENOENT)) {
    return;
  }
  Filesystem& fs = filesystem(request);
  fuse_entry_param entry = {};
  const int error = fs.lookup_entry(parent, name, entry);
  if (error != 0) {
    fuse_reply_err(request, error);
    return;
  }
  reply_entry(request, fs, entry);
}

void op_forget(fuse_req_t request, fuse_ino_t identifier, uint64_t count) {
  filesystem(request).forget_one(identifier, count);
  fuse_reply_none(request);
}

void op_forget_multi(fuse_req_t request, size_t count, fuse_forget_data* forgets) {
  Filesystem& fs = filesystem(request);
  for (size_t index = 0; index < count; ++index) {
    fs.forget_one(forgets[index].ino, forgets[index].nlookup);
  }
  fuse_reply_none(request);
}

void op_getattr(fuse_req_t request, fuse_ino_t identifier, fuse_file_info*) {
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  reply_attributes(request, backing.get());
}

void op_setattr(fuse_req_t request, fuse_ino_t identifier, struct stat* attributes, int valid, fuse_file_info* info) {
  Filesystem& fs = filesystem(request);
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  const int subject = info != nullptr ? file_handle(info).descriptor.get() : backing.get();
  unique_fd replacement_lease;
  if (info == nullptr) {
    auto lease = acquire_replacement_lease(fs, subject);
    if (!lease) {
      fuse_reply_err(request, lease.error());
      return;
    }
    replacement_lease = std::move(*lease);
  }

  if ((valid & FUSE_SET_ATTR_SIZE) != 0 && placeholder_withholds_content(read_placeholder_marker(subject))) {
    fuse_reply_err(request, EIO);
    return;
  }

  const int error = apply_attributes(backing.get(), *attributes, valid, info);
  if (error != 0) {
    fuse_reply_err(request, error);
    return;
  }
  reply_attributes(request, backing.get());
}

void op_readlink(fuse_req_t request, fuse_ino_t identifier) {
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  char target[PATH_MAX + 1];
  const auto length = ::readlinkat(backing.get(), "", target, sizeof target);
  if (length < 0) {
    fuse_reply_err(request, errno);
  } else if (static_cast<size_t>(length) == sizeof target) {
    fuse_reply_err(request, ENAMETOOLONG);
  } else {
    target[length] = '\0';
    fuse_reply_readlink(request, target);
  }
}

void reply_created_entry(fuse_req_t request, fuse_ino_t parent, const char* name, int creation_result) {
  if (creation_result != 0) {
    fuse_reply_err(request, errno);
    return;
  }
  Filesystem& fs = filesystem(request);
  fuse_entry_param entry = {};
  const int error = fs.lookup_entry(parent, name, entry);
  if (error != 0) {
    fuse_reply_err(request, error);
    return;
  }
  reply_entry(request, fs, entry);
}

void op_mknod(fuse_req_t request, fuse_ino_t parent, const char* name, mode_t mode, dev_t device) {
  if (reject_sync_database_entry(request, name, EACCES)) {
    return;
  }
  const unique_fd parent_backing = borrow_populated_directory_or_reply(request, parent);
  if (!parent_backing) {
    return;
  }
  reply_created_entry(request, parent, name, ::mknodat(parent_backing.get(), name, mode, device));
}

void op_mkdir(fuse_req_t request, fuse_ino_t parent, const char* name, mode_t mode) {
  if (reject_sync_database_entry(request, name, EACCES)) {
    return;
  }
  const unique_fd parent_backing = borrow_populated_directory_or_reply(request, parent);
  if (!parent_backing) {
    return;
  }
  reply_created_entry(request, parent, name, ::mkdirat(parent_backing.get(), name, mode));
}

void op_symlink(fuse_req_t request, const char* target, fuse_ino_t parent, const char* name) {
  if (reject_sync_database_entry(request, name, EACCES)) {
    return;
  }
  const unique_fd parent_backing = borrow_populated_directory_or_reply(request, parent);
  if (!parent_backing) {
    return;
  }
  reply_created_entry(request, parent, name, ::symlinkat(target, parent_backing.get(), name));
}

void op_link(fuse_req_t request, fuse_ino_t identifier, fuse_ino_t parent, const char* name) {
  if (reject_sync_database_entry(request, name, EACCES)) {
    return;
  }
  Filesystem& fs = filesystem(request);
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  const auto replacement_lease = acquire_replacement_lease(fs, backing.get());
  if (!replacement_lease) {
    fuse_reply_err(request, replacement_lease.error());
    return;
  }
  const unique_fd parent_backing = borrow_populated_directory_or_reply(request, parent);
  if (!parent_backing) {
    return;
  }
  const ProcFdPath source(backing.get());
  const int result = ::linkat(AT_FDCWD, source.c_str(), parent_backing.get(), name, AT_SYMLINK_FOLLOW);
  reply_created_entry(request, parent, name, result);
}

void op_unlink(fuse_req_t request, fuse_ino_t parent, const char* name) {
  Filesystem& fs = filesystem(request);
  const unique_fd parent_backing = borrow_populated_directory_or_reply(request, parent);
  if (!parent_backing) {
    return;
  }

  fuse_entry_param entry = {};
  const int lookup_error = fs.lookup_entry(parent, name, entry);
  if (lookup_error != 0) {
    fuse_reply_err(request, lookup_error);
    return;
  }
  const unique_fd backing = fs.borrow_backing(entry.ino);
  if (!backing) {
    fs.forget_one(entry.ino, 1);
    fuse_reply_err(request, ESTALE);
    return;
  }
  const auto replacement_lease = acquire_replacement_lease(fs, backing.get());
  if (!replacement_lease) {
    fs.forget_one(entry.ino, 1);
    fuse_reply_err(request, replacement_lease.error());
    return;
  }

  const int result = ::unlinkat(parent_backing.get(), name, 0);
  const int error = result == 0 ? 0 : errno;

  fs.forget_one(entry.ino, 1);

  fuse_reply_err(request, error);
}

void op_rmdir(fuse_req_t request, fuse_ino_t parent, const char* name) {
  if (reject_sync_database_entry(request, name, ENOENT)) {
    return;
  }
  const unique_fd parent_backing = borrow_populated_directory_or_reply(request, parent);
  if (!parent_backing) {
    return;
  }
  fuse_reply_err(request, ::unlinkat(parent_backing.get(), name, AT_REMOVEDIR) == 0 ? 0 : errno);
}

void op_rename(
    fuse_req_t request,
    fuse_ino_t parent,
    const char* name,
    fuse_ino_t new_parent,
    const char* new_name,
    unsigned int flags) {
  if (reject_sync_database_entry(request, name, ENOENT) || reject_sync_database_entry(request, new_name, EACCES)) {
    return;
  }
  Filesystem& fs = filesystem(request);
  const unique_fd parent_backing = borrow_populated_directory_or_reply(request, parent);
  if (!parent_backing) {
    return;
  }
  const unique_fd new_parent_backing = borrow_populated_directory_or_reply(request, new_parent);
  if (!new_parent_backing) {
    return;
  }

  const unique_fd source(::openat(parent_backing.get(), name, O_PATH | O_NOFOLLOW));
  if (!source) {
    fuse_reply_err(request, errno);
    return;
  }
  const auto source_lease = acquire_replacement_lease(fs, source.get());
  if (!source_lease) {
    fuse_reply_err(request, source_lease.error());
    return;
  }

  unique_fd destination(::openat(new_parent_backing.get(), new_name, O_PATH | O_NOFOLLOW));
  unique_fd destination_lease;
  if (destination) {
    auto lease = acquire_replacement_lease(fs, destination.get());
    if (!lease) {
      fuse_reply_err(request, lease.error());
      return;
    }
    destination_lease = std::move(*lease);
  } else if (errno != ENOENT) {
    fuse_reply_err(request, errno);
    return;
  }
  const int result = ::renameat2(parent_backing.get(), name, new_parent_backing.get(), new_name, flags);
  fuse_reply_err(request, result == 0 ? 0 : errno);
}

void op_open(fuse_req_t request, fuse_ino_t identifier, fuse_file_info* info) {
  Filesystem& fs = filesystem(request);
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  Inode& node = fs.inode(identifier);

  const ProcFdPath backing_path(backing.get());
  auto handle = std::make_unique<FileHandle>();
  handle->descriptor.reset(::open(backing_path.c_str(), info->flags & ~(O_NOFOLLOW | O_TRUNC)));
  if (!handle->descriptor) {
    fuse_reply_err(request, errno);
    return;
  }

  auto marker = read_placeholder_marker(handle->descriptor.get());
  auto withholds_content = placeholder_withholds_content(marker);
  if (withholds_content) {
    if (!marker || !marker->has_value() || (info->flags & O_ACCMODE) == O_WRONLY || (info->flags & O_TRUNC) != 0) {
      fuse_reply_err(request, EIO);
      return;
    }
    const int hydration_error = fs.hydrate(handle->descriptor.get());
    if (hydration_error != 0) {
      fuse_reply_err(request, hydration_error);
      return;
    }
    marker = read_placeholder_marker(handle->descriptor.get());
    withholds_content = placeholder_withholds_content(marker);
    if (withholds_content) {
      fuse_reply_err(request, EIO);
      return;
    }
  }
  const int lease_error = acquire_replacement_lease(fs, *handle);
  if (lease_error != 0) {
    fuse_reply_err(request, lease_error);
    return;
  }
  const int truncation_error = apply_requested_truncation(info->flags, handle->descriptor.get(), withholds_content);
  if (truncation_error != 0) {
    fuse_reply_err(request, truncation_error);
    return;
  }

  info->noflush = static_cast<unsigned int>((info->flags & O_ACCMODE) == O_RDONLY);
  const int open_error = begin_open(request, fs, node, handle->descriptor.get(), withholds_content, info);
  if (open_error != 0) {
    fuse_reply_err(request, open_error);
    return;
  }

  info->fh = reinterpret_cast<uint64_t>(handle.release());
  if (fuse_reply_open(request, info) != 0) {
    fs.release_one(node);
    delete reinterpret_cast<FileHandle*>(info->fh);
  }
}

void op_create(fuse_req_t request, fuse_ino_t parent, const char* name, mode_t mode, fuse_file_info* info) {
  if (reject_sync_database_entry(request, name, EACCES)) {
    return;
  }
  Filesystem& fs = filesystem(request);
  const unique_fd parent_backing = borrow_populated_directory_or_reply(request, parent);
  if (!parent_backing) {
    return;
  }

  auto opened = open_for_creation(parent_backing.get(), name, info->flags, mode);
  if (!opened) {
    fuse_reply_err(request, opened.error());
    return;
  }
  auto handle = std::make_unique<FileHandle>();
  handle->descriptor = std::move(opened->descriptor);

  const bool withholds_content = placeholder_withholds_content(read_placeholder_marker(handle->descriptor.get()));
  const int lease_error = acquire_replacement_lease(fs, *handle);
  if (lease_error != 0) {
    discard_creation(parent_backing.get(), name, opened->created);
    fuse_reply_err(request, lease_error);
    return;
  }
  const int truncation_error = apply_requested_truncation(info->flags, handle->descriptor.get(), withholds_content);
  if (truncation_error != 0) {
    discard_creation(parent_backing.get(), name, opened->created);
    fuse_reply_err(request, truncation_error);
    return;
  }

  fuse_entry_param entry = {};
  const int error = fs.entry_for_descriptor(handle->descriptor.get(), entry);
  if (error != 0) {
    discard_creation(parent_backing.get(), name, opened->created);
    fuse_reply_err(request, error);
    return;
  }

  info->noflush = static_cast<unsigned int>((info->flags & O_ACCMODE) == O_RDONLY);
  Inode& node = fs.inode(entry.ino);
  const int open_error = begin_open(request, fs, node, handle->descriptor.get(), withholds_content, info);
  if (open_error != 0) {
    fs.forget_one(entry.ino, 1);
    discard_creation(parent_backing.get(), name, opened->created);
    fuse_reply_err(request, open_error);
    return;
  }

  info->fh = reinterpret_cast<uint64_t>(handle.release());
  if (fuse_reply_create(request, &entry, info) != 0) {
    fs.release_one(node);
    fs.forget_one(entry.ino, 1);
    delete reinterpret_cast<FileHandle*>(info->fh);
  }
}

void op_release(fuse_req_t request, fuse_ino_t identifier, fuse_file_info* info) {
  Filesystem& fs = filesystem(request);
  fs.release_one(fs.inode(identifier));
  delete reinterpret_cast<FileHandle*>(info->fh);
  fuse_reply_err(request, 0);
}

void op_flush(fuse_req_t request, fuse_ino_t, fuse_file_info* info) {
  const int duplicate = ::dup(file_handle(info).descriptor.get());
  if (duplicate < 0) {
    fuse_reply_err(request, errno);
    return;
  }
  fuse_reply_err(request, ::close(duplicate) == 0 ? 0 : errno);
}

void op_fsync(fuse_req_t request, fuse_ino_t, int datasync, fuse_file_info* info) {
  const int descriptor = file_handle(info).descriptor.get();
  const int result = datasync != 0 ? ::fdatasync(descriptor) : ::fsync(descriptor);
  fuse_reply_err(request, result == 0 ? 0 : errno);
}

void op_read(fuse_req_t request, fuse_ino_t, size_t size, off_t offset, fuse_file_info* info) {
  FileHandle& handle = file_handle(info);
  if (placeholder_withholds_content(read_placeholder_marker(handle.descriptor.get()))) {
    fuse_reply_err(request, EIO);
    return;
  }

  fuse_bufvec source = FUSE_BUFVEC_INIT(size);
  source.buf[0].flags = static_cast<fuse_buf_flags>(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
  source.buf[0].fd = handle.descriptor.get();
  source.buf[0].pos = offset;
  fuse_reply_data(request, &source, FUSE_BUF_SPLICE_MOVE);
}

void op_write_buf(fuse_req_t request, fuse_ino_t, fuse_bufvec* incoming, off_t offset, fuse_file_info* info) {
  FileHandle& handle = file_handle(info);
  if (placeholder_withholds_content(read_placeholder_marker(handle.descriptor.get()))) {
    fuse_reply_err(request, EIO);
    return;
  }

  fuse_bufvec destination = FUSE_BUFVEC_INIT(fuse_buf_size(incoming));
  destination.buf[0].flags = static_cast<fuse_buf_flags>(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
  destination.buf[0].fd = handle.descriptor.get();
  destination.buf[0].pos = offset;

  const auto copied = fuse_buf_copy(&destination, incoming, FUSE_BUF_SPLICE_MOVE);
  if (copied < 0) {
    fuse_reply_err(request, static_cast<int>(-copied));
    return;
  }
  fuse_reply_write(request, static_cast<size_t>(copied));
}

void op_opendir(fuse_req_t request, fuse_ino_t identifier, fuse_file_info* info) {
  Filesystem& fs = filesystem(request);
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  const int discovery_error = fs.populate(backing.get());
  if (discovery_error != 0) {
    fuse_reply_err(request, discovery_error);
    return;
  }

  unique_fd descriptor(::openat(backing.get(), ".", O_RDONLY | O_DIRECTORY));
  if (!descriptor) {
    fuse_reply_err(request, errno);
    return;
  }

  auto handle = std::make_unique<DirectoryHandle>();
  handle->stream.reset(::fdopendir(descriptor.get()));
  if (!handle->stream) {
    fuse_reply_err(request, errno);
    return;
  }
  descriptor.release();

  info->fh = reinterpret_cast<uint64_t>(handle.release());
  if (fuse_reply_open(request, info) != 0) {
    delete reinterpret_cast<DirectoryHandle*>(info->fh);
  }
}

void do_readdir(fuse_req_t request, fuse_ino_t identifier, size_t size, off_t offset, fuse_file_info* info, bool plus) {
  Filesystem& fs = filesystem(request);
  DirectoryHandle& handle = directory_handle(info);

  std::vector<char> buffer(size);
  size_t used = 0;
  int error = 0;

  {
    const std::lock_guard guard(handle.mutex);

    if (offset != handle.offset) {
      ::seekdir(handle.stream.get(), offset);
      handle.offset = offset;
    }

    for (;;) {
      errno = 0;
      const dirent* entry = ::readdir(handle.stream.get());
      if (entry == nullptr) {
        error = errno;
        break;
      }
      handle.offset = entry->d_off;

      if (is_sync_database_entry(entry->d_name)) {
        continue;
      }

      fuse_entry_param reply = {};
      bool looked_up = false;
      if (plus && !is_dot_or_dotdot(entry->d_name)) {
        error = fs.lookup_entry(identifier, entry->d_name, reply);
        if (error != 0) {
          break;
        }
        looked_up = true;
      } else {
        reply.attr.st_ino = entry->d_ino;
        reply.attr.st_mode = static_cast<mode_t>(entry->d_type) << 12;
      }

      const size_t written =
          plus
              ? fuse_add_direntry_plus(request, buffer.data() + used, size - used, entry->d_name, &reply, entry->d_off)
              : fuse_add_direntry(request, buffer.data() + used, size - used, entry->d_name, &reply.attr, entry->d_off);

      if (written > size - used) {
        if (looked_up) {
          fs.forget_one(reply.ino, 1);
        }
        break;
      }
      used += written;
    }
  }

  if (error != 0 && used == 0) {
    fuse_reply_err(request, error);
    return;
  }
  fuse_reply_buf(request, buffer.data(), used);
}

void op_readdir(fuse_req_t request, fuse_ino_t identifier, size_t size, off_t offset, fuse_file_info* info) {
  do_readdir(request, identifier, size, offset, info, false);
}

void op_readdirplus(fuse_req_t request, fuse_ino_t identifier, size_t size, off_t offset, fuse_file_info* info) {
  do_readdir(request, identifier, size, offset, info, true);
}

void op_releasedir(fuse_req_t request, fuse_ino_t, fuse_file_info* info) {
  delete reinterpret_cast<DirectoryHandle*>(info->fh);
  fuse_reply_err(request, 0);
}

void op_fsyncdir(fuse_req_t request, fuse_ino_t, int datasync, fuse_file_info* info) {
  DirectoryHandle& handle = directory_handle(info);
  int error = 0;
  {
    const std::lock_guard guard(handle.mutex);
    const int descriptor = ::dirfd(handle.stream.get());
    const int result = datasync != 0 ? ::fdatasync(descriptor) : ::fsync(descriptor);
    error = result == 0 ? 0 : errno;
  }
  fuse_reply_err(request, error);
}

void op_statfs(fuse_req_t request, fuse_ino_t identifier) {
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  struct statvfs statistics = {};
  if (::fstatvfs(backing.get(), &statistics) != 0) {
    fuse_reply_err(request, errno);
    return;
  }
  fuse_reply_statfs(request, &statistics);
}

void op_access(fuse_req_t request, fuse_ino_t identifier, int mask) {
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  const ProcFdPath backing_path(backing.get());
  fuse_reply_err(request, ::access(backing_path.c_str(), mask) == 0 ? 0 : errno);
}

void op_flock(fuse_req_t request, fuse_ino_t, fuse_file_info* info, int operation) {
  fuse_reply_err(request, ::flock(file_handle(info).descriptor.get(), operation) == 0 ? 0 : errno);
}

void op_getxattr(fuse_req_t request, fuse_ino_t identifier, const char* name, size_t size) {
  if (is_internal_attribute(name)) {
    fuse_reply_err(request, ENODATA);
    return;
  }

  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  const ProcFdPath backing_path(backing.get());
  if (size == 0) {
    const auto length = ::getxattr(backing_path.c_str(), name, nullptr, 0);
    if (length < 0) {
      fuse_reply_err(request, errno);
      return;
    }
    fuse_reply_xattr(request, static_cast<size_t>(length));
    return;
  }

  std::vector<char> value(size);
  const auto length = ::getxattr(backing_path.c_str(), name, value.data(), value.size());
  if (length < 0) {
    fuse_reply_err(request, errno);
    return;
  }
  fuse_reply_buf(request, value.data(), static_cast<size_t>(length));
}

void op_listxattr(fuse_req_t request, fuse_ino_t identifier, size_t size) {
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  const ProcFdPath backing_path(backing.get());
  const auto names = read_attribute_names(backing_path.c_str());
  if (!names) {
    fuse_reply_err(request, names.error());
    return;
  }

  const std::string visible = visible_attribute_names(*names);
  if (size == 0) {
    fuse_reply_xattr(request, visible.size());
    return;
  }
  if (size < visible.size()) {
    fuse_reply_err(request, ERANGE);
    return;
  }
  fuse_reply_buf(request, visible.data(), visible.size());
}

void op_setxattr(
    fuse_req_t request,
    fuse_ino_t identifier,
    const char* name,
    const char* value,
    size_t size,
    int flags) {
  if (is_internal_attribute(name)) {
    fuse_reply_err(request, EPERM);
    return;
  }
  Filesystem& fs = filesystem(request);
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  const auto replacement_lease = acquire_replacement_lease(fs, backing.get());
  if (!replacement_lease) {
    fuse_reply_err(request, replacement_lease.error());
    return;
  }
  const ProcFdPath backing_path(backing.get());
  fuse_reply_err(request, ::setxattr(backing_path.c_str(), name, value, size, flags) == 0 ? 0 : errno);
}

void op_removexattr(fuse_req_t request, fuse_ino_t identifier, const char* name) {
  if (is_internal_attribute(name)) {
    fuse_reply_err(request, EPERM);
    return;
  }
  Filesystem& fs = filesystem(request);
  const unique_fd backing = borrow_backing_or_reply(request, identifier);
  if (!backing) {
    return;
  }
  const auto replacement_lease = acquire_replacement_lease(fs, backing.get());
  if (!replacement_lease) {
    fuse_reply_err(request, replacement_lease.error());
    return;
  }
  const ProcFdPath backing_path(backing.get());
  fuse_reply_err(request, ::removexattr(backing_path.c_str(), name) == 0 ? 0 : errno);
}

}  // namespace

Filesystem::Filesystem(
    unique_fd store_root,
    dev_t store_device,
    std::string store_path,
    PassthroughPolicy passthrough_policy)
    : store_device_(store_device),
      passthrough_policy_(passthrough_policy),
      passthrough_(passthrough_policy != PassthroughPolicy::Disabled),
      hydrator_(store_path),
      discoverer_(store_path),
      replacement_lock_path_(
          QFile::encodeName(ncvfs::replacement_lock_path(QString::fromStdString(store_path))).toStdString()) {
  root_.backing = std::move(store_root);
  root_.device = store_device;
  root_.lookup_count = 1;

  struct stat attributes = {};
  if (::fstatat(root_.backing.get(), "", &attributes, AT_EMPTY_PATH) == 0) {
    root_.number = attributes.st_ino;
  }
}

std::expected<unique_fd, int> Filesystem::acquire_replacement_lease(int backing_fd) const {
  struct stat attributes = {};
  if (::fstat(backing_fd, &attributes) != 0) {
    return std::unexpected(errno);
  }

  unique_fd lease(::open(replacement_lock_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
  if (!lease) {
    return std::unexpected(errno);
  }
  if (set_replacement_lock(lease.get(), attributes, F_RDLCK) != 0) {
    return std::unexpected(errno);
  }
  return lease;
}

void Filesystem::fail_passthrough() {
  failed_.store(true, std::memory_order_relaxed);
  fuse_session_exit(session_);
}

Inode& Filesystem::inode(fuse_ino_t identifier) {
  return identifier == FUSE_ROOT_ID ? root_ : *reinterpret_cast<Inode*>(identifier);
}

unique_fd Filesystem::borrow_backing(fuse_ino_t identifier) {
  const std::lock_guard table_guard(table_mutex_);
  Inode& node = inode(identifier);
  const std::lock_guard node_guard(node.mutex);
  if (!node.backing) {
    return {};
  }
  return unique_fd(::fcntl(node.backing.get(), F_DUPFD_CLOEXEC, 0));
}

int Filesystem::lookup_entry(fuse_ino_t parent, const char* name, fuse_entry_param& entry) {
  if (is_sync_database_entry(name)) {
    return ENOENT;
  }
  const unique_fd parent_backing = borrow_backing(parent);
  if (!parent_backing) {
    return ESTALE;
  }
  const int discovery_error = populate(parent_backing.get());
  if (discovery_error != 0) {
    return discovery_error;
  }
  unique_fd candidate(::openat(parent_backing.get(), name, O_PATH | O_NOFOLLOW));
  if (!candidate) {
    return errno;
  }
  return adopt_entry(std::move(candidate), entry);
}

int Filesystem::entry_for_descriptor(int descriptor, fuse_entry_param& entry) {
  const ProcFdPath opened_path(descriptor);
  unique_fd candidate(::open(opened_path.c_str(), O_PATH));
  if (!candidate) {
    return errno;
  }
  return adopt_entry(std::move(candidate), entry);
}

int Filesystem::adopt_entry(unique_fd candidate, fuse_entry_param& entry) {
  entry = {};
  entry.attr_timeout = attribute_cache_seconds;
  entry.entry_timeout = entry_cache_seconds;

  if (::fstatat(candidate.get(), "", &entry.attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) != 0) {
    return errno;
  }

  if (entry.attr.st_dev != store_device_) {
    fuse_log(
        FUSE_LOG_WARNING,
        "%s sits on a filesystem other than the store, which the mount cannot represent\n",
        describe_fd(candidate.get()).c_str());
    return ENOTSUP;
  }

  if (entry.attr.st_ino == FUSE_ROOT_ID) {
    fuse_log(
        FUSE_LOG_ERR,
        "%s occupies inode %ju, which the mount reserves for its own root\n",
        describe_fd(candidate.get()).c_str(),
        static_cast<uintmax_t>(FUSE_ROOT_ID));
    return EIO;
  }

  override_placeholder_size(entry.attr, candidate.get());

  const std::lock_guard table_guard(table_mutex_);
  Inode& node = inodes_[Key{.device = entry.attr.st_dev, .number = entry.attr.st_ino}];
  const std::lock_guard node_guard(node.mutex);

  if (!node.backing) {
    node.device = entry.attr.st_dev;
    node.number = entry.attr.st_ino;
    node.backing = std::move(candidate);
  }
  ++node.lookup_count;

  entry.ino = reinterpret_cast<fuse_ino_t>(&node);
  entry.generation = node.generation;
  return 0;
}

void Filesystem::forget_one(fuse_ino_t identifier, uint64_t count) {
  if (identifier == FUSE_ROOT_ID) {
    return;
  }

  const std::lock_guard table_guard(table_mutex_);
  Inode& node = *reinterpret_cast<Inode*>(identifier);
  Key key = {};
  {
    const std::lock_guard node_guard(node.mutex);
    if (count > node.lookup_count) {
      fuse_log(
          FUSE_LOG_ERR,
          "inode %ju was forgotten %ju times against a lookup count of %ju\n",
          static_cast<uintmax_t>(node.number),
          static_cast<uintmax_t>(count),
          static_cast<uintmax_t>(node.lookup_count));
      node.lookup_count = 0;
    } else {
      node.lookup_count -= count;
    }
    if (node.lookup_count != 0) {
      return;
    }
    key = Key{.device = node.device, .number = node.number};
  }
  inodes_.erase(key);
}

void Filesystem::release_one(Inode& node) {
  const std::lock_guard guard(node.mutex);
  if (node.open_count > 0) {
    --node.open_count;
  }
  if (node.open_count != 0 || node.backing_id == 0) {
    return;
  }
  if (::ioctl(fuse_session_fd(session_), FUSE_DEV_IOC_BACKING_CLOSE, &node.backing_id) < 0) {
    fuse_log(
        FUSE_LOG_WARNING,
        "the backing file %d of inode %ju stayed open after its last release: %s\n",
        node.backing_id,
        static_cast<uintmax_t>(node.number),
        std::strerror(errno));
  }
  node.backing_id = 0;
}

fuse_lowlevel_ops filesystem_operations() {
  fuse_lowlevel_ops operations = {};
  operations.init = op_init;
  operations.lookup = op_lookup;
  operations.forget = op_forget;
  operations.forget_multi = op_forget_multi;
  operations.getattr = op_getattr;
  operations.setattr = op_setattr;
  operations.readlink = op_readlink;
  operations.mknod = op_mknod;
  operations.mkdir = op_mkdir;
  operations.symlink = op_symlink;
  operations.link = op_link;
  operations.unlink = op_unlink;
  operations.rmdir = op_rmdir;
  operations.rename = op_rename;
  operations.open = op_open;
  operations.create = op_create;
  operations.release = op_release;
  operations.flush = op_flush;
  operations.fsync = op_fsync;
  operations.read = op_read;
  operations.write_buf = op_write_buf;
  operations.opendir = op_opendir;
  operations.readdir = op_readdir;
  operations.readdirplus = op_readdirplus;
  operations.releasedir = op_releasedir;
  operations.fsyncdir = op_fsyncdir;
  operations.statfs = op_statfs;
  operations.access = op_access;
  operations.flock = op_flock;
  operations.getxattr = op_getxattr;
  operations.listxattr = op_listxattr;
  operations.setxattr = op_setxattr;
  operations.removexattr = op_removexattr;
  return operations;
}

}  // namespace ncvfs
