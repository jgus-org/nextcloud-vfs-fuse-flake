#pragma once

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 17)

#include <fuse_lowlevel.h>
#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "discovery.hpp"
#include "hydration.hpp"
#include "unique_fd.hpp"

namespace ncvfs {

enum class PassthroughPolicy {
  Preferred,
  Required,
  Disabled,
};

struct Inode {
  unique_fd backing;
  dev_t device = 0;
  ino_t number = 0;
  uint64_t generation = 0;
  uint64_t lookup_count = 0;
  uint64_t open_count = 0;
  int backing_id = 0;
  std::mutex mutex;
};

class Filesystem {
 public:
  Filesystem(unique_fd store_root, dev_t store_device, std::string store_path, PassthroughPolicy passthrough_policy);

  void attach_session(fuse_session* session) { session_ = session; }

  Inode& inode(fuse_ino_t identifier);
  unique_fd borrow_backing(fuse_ino_t identifier);
  int lookup_entry(fuse_ino_t parent, const char* name, fuse_entry_param& entry);
  int entry_for_descriptor(int descriptor, fuse_entry_param& entry);
  void forget_one(fuse_ino_t identifier, uint64_t count);
  void release_one(Inode& node);
  int hydrate(int backing_fd) { return hydrator_.hydrate(backing_fd); }
  int populate(int backing_fd) { return discoverer_.populate(backing_fd); }
  std::expected<unique_fd, int> acquire_replacement_lease(int backing_fd) const;

  bool passthrough() const { return passthrough_.load(std::memory_order_relaxed); }
  bool passthrough_required() const { return passthrough_policy_ == PassthroughPolicy::Required; }
  void disable_passthrough() { passthrough_.store(false, std::memory_order_relaxed); }
  void fail_passthrough();
  bool failed() const { return failed_.load(std::memory_order_relaxed); }

 private:
  struct Key {
    dev_t device;
    ino_t number;

    bool operator==(const Key&) const = default;
  };

  struct KeyHash {
    size_t operator()(const Key& key) const {
      return std::hash<dev_t>{}(key.device) ^ (std::hash<ino_t>{}(key.number) << 1);
    }
  };

  int adopt_entry(unique_fd candidate, fuse_entry_param& entry);

  Inode root_;
  dev_t store_device_;
  fuse_session* session_ = nullptr;
  PassthroughPolicy passthrough_policy_;
  std::atomic<bool> passthrough_;
  std::atomic<bool> failed_ = false;
  Hydrator hydrator_;
  DirectoryDiscoverer discoverer_;
  std::string replacement_lock_path_;
  std::mutex table_mutex_;
  std::unordered_map<Key, Inode, KeyHash> inodes_;
};

fuse_lowlevel_ops filesystem_operations();

}  // namespace ncvfs
