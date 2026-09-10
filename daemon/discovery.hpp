#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ncvfs {

class DirectoryDiscoverer {
 public:
  explicit DirectoryDiscoverer(std::string store);

  int populate(int backing_fd);

 private:
  struct ActiveDiscovery {
    std::mutex mutex;
    std::condition_variable finished;
    bool complete = false;
    int result = 0;
  };

  int populate_once(const std::string& relative_path, int backing_fd);
  std::string relative_path(int backing_fd) const;

  std::string store_;
  std::mutex active_mutex_;
  std::unordered_map<std::string, std::shared_ptr<ActiveDiscovery>> active_;
};

}  // namespace ncvfs
