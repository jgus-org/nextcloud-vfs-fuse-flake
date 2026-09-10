#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <syslog.h>

#include <QCoreApplication>
#include <boost/program_options.hpp>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include "filesystem.hpp"
#include "store.hpp"
#include "unique_fd.hpp"

namespace {

namespace options = boost::program_options;

constexpr char mount_options[] = "fsname=nextcloud-vfs-fused,default_permissions";

class FuseArguments {
 public:
  FuseArguments() = default;
  FuseArguments(const FuseArguments&) = delete;
  FuseArguments& operator=(const FuseArguments&) = delete;
  ~FuseArguments() { fuse_opt_free_args(&arguments_); }

  bool add(const char* argument) { return fuse_opt_add_arg(&arguments_, argument) == 0; }

  fuse_args* get() { return &arguments_; }

 private:
  fuse_args arguments_ = FUSE_ARGS_INIT(0, nullptr);
};

class Session {
 public:
  Session(fuse_args* arguments, const fuse_lowlevel_ops& operations, void* userdata)
      : session_(fuse_session_new(arguments, &operations, sizeof operations, userdata)) {}
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session() {
    if (session_ != nullptr) {
      fuse_session_destroy(session_);
    }
  }

  fuse_session* get() const { return session_; }

  explicit operator bool() const { return session_ != nullptr; }

 private:
  fuse_session* session_ = nullptr;
};

class SignalHandlers {
 public:
  explicit SignalHandlers(fuse_session* session)
      : session_(session),
        installed_(fuse_set_signal_handlers(session) == 0) {
    ready_ = installed_ && fuse_set_fail_signal_handlers(session) == 0;
  }
  SignalHandlers(const SignalHandlers&) = delete;
  SignalHandlers& operator=(const SignalHandlers&) = delete;
  ~SignalHandlers() {
    if (installed_) {
      fuse_remove_signal_handlers(session_);
    }
  }

  explicit operator bool() const { return ready_; }

 private:
  fuse_session* session_;
  bool installed_ = false;
  bool ready_ = false;
};

class Mount {
 public:
  Mount(fuse_session* session, const std::string& point)
      : session_(session),
        mounted_(fuse_session_mount(session, point.c_str()) == 0) {}
  Mount(const Mount&) = delete;
  Mount& operator=(const Mount&) = delete;
  ~Mount() {
    if (mounted_) {
      fuse_session_unmount(session_);
    }
  }

  explicit operator bool() const { return mounted_; }

 private:
  fuse_session* session_;
  bool mounted_ = false;
};

class LoopConfig {
 public:
  LoopConfig() : config_(fuse_loop_cfg_create()) {}
  LoopConfig(const LoopConfig&) = delete;
  LoopConfig& operator=(const LoopConfig&) = delete;
  ~LoopConfig() { fuse_loop_cfg_destroy(config_); }

  fuse_loop_config* get() const { return config_; }

  explicit operator bool() const { return config_ != nullptr; }

 private:
  fuse_loop_config* config_ = nullptr;
};

void maximize_descriptor_limit() {
  rlimit limits = {};
  if (::getrlimit(RLIMIT_NOFILE, &limits) != 0) {
    fuse_log(FUSE_LOG_WARNING, "the open-descriptor limit could not be read: %s\n", std::strerror(errno));
    return;
  }
  limits.rlim_cur = limits.rlim_max;
  if (::setrlimit(RLIMIT_NOFILE, &limits) != 0) {
    fuse_log(FUSE_LOG_WARNING, "the open-descriptor limit could not be raised: %s\n", std::strerror(errno));
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  std::string store;
  std::string mount;
  std::string passthrough = "preferred";

  options::options_description description("nextcloud-vfs-fused options");
  auto add_option = description.add_options();
  add_option("help,h", "describe the options");
  add_option("store", options::value(&store)->required(), "backing store the sync client writes");
  add_option("mount", options::value(&mount)->required(), "directory the presented view appears at");
  add_option("passthrough", options::value(&passthrough), "kernel data path: required, preferred, or disabled");
  add_option("foreground,f", "stay attached to the terminal");
  add_option("debug,d", "log the FUSE protocol traffic, and stay attached to the terminal");

  options::variables_map given;
  try {
    options::store(options::parse_command_line(argc, argv, description), given);
    if (given.count("help") != 0) {
      std::cout << description << '\n';
      return 0;
    }
    options::notify(given);
  } catch (const options::error& failure) {
    std::cerr << failure.what() << '\n' << description << '\n';
    return 2;
  }

  const bool debug = given.count("debug") != 0;
  const bool foreground = debug || given.count("foreground") != 0;

  ncvfs::PassthroughPolicy passthrough_policy;
  if (passthrough == "required") {
    passthrough_policy = ncvfs::PassthroughPolicy::Required;
  } else if (passthrough == "preferred") {
    passthrough_policy = ncvfs::PassthroughPolicy::Preferred;
  } else if (passthrough == "disabled") {
    passthrough_policy = ncvfs::PassthroughPolicy::Disabled;
  } else {
    std::cerr << "invalid passthrough policy: " << passthrough << '\n';
    return 2;
  }

  const auto layout = ncvfs::resolve_layout(store, mount);
  if (!layout) {
    std::cerr << layout.error() << '\n';
    return 2;
  }

  struct stat store_attributes = {};
  if (::stat(layout->store.c_str(), &store_attributes) != 0) {
    std::cerr << "the store " << layout->store << " cannot be inspected: " << std::strerror(errno) << '\n';
    return 1;
  }

  ncvfs::unique_fd store_root(::open(layout->store.c_str(), O_PATH | O_DIRECTORY));
  if (!store_root) {
    std::cerr << "the store " << layout->store << " cannot be opened: " << std::strerror(errno) << '\n';
    return 1;
  }

  ncvfs::Filesystem filesystem(std::move(store_root), store_attributes.st_dev, layout->store, passthrough_policy);

  maximize_descriptor_limit();
  ::umask(0);

  FuseArguments arguments;
  if (!arguments.add(argv[0]) || !arguments.add("-o") || !arguments.add(mount_options) ||
      (debug && !arguments.add("-odebug"))) {
    std::cerr << "the FUSE argument vector could not be assembled\n";
    return 1;
  }

  const auto operations = ncvfs::filesystem_operations();
  const Session session(arguments.get(), operations, &filesystem);
  if (!session) {
    std::cerr << "the FUSE session could not be created\n";
    return 1;
  }
  filesystem.attach_session(session.get());

  const SignalHandlers handlers(session.get());
  if (!handlers) {
    std::cerr << "the FUSE signal handlers could not be installed\n";
    return 1;
  }

  const Mount mounted(session.get(), layout->mount);
  if (!mounted) {
    std::cerr << "the mount point " << layout->mount << " could not be mounted\n";
    return 1;
  }

  const LoopConfig loop;
  if (!loop) {
    std::cerr << "the FUSE loop configuration could not be created\n";
    return 1;
  }

  if (fuse_daemonize(foreground) != 0) {
    std::cerr << "the daemon could not detach from the terminal\n";
    return 1;
  }
  if (!foreground) {
    fuse_log_enable_syslog("nextcloud-vfs-fused", LOG_PID | LOG_CONS, LOG_DAEMON);
  }

  const int result = fuse_session_loop_mt(session.get(), loop.get());

  if (!foreground) {
    fuse_log_close_syslog();
  }
  return result == 0 && !filesystem.failed() ? 0 : 1;
}
