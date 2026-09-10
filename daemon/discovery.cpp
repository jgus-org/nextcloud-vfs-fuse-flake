#include "discovery.hpp"

#include <fuse_log.h>

#include <QJsonObject>
#include <QLocalSocket>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "placeholder.hpp"
#include "proc_fd.hpp"
#include "protocol.hpp"
#include "store.hpp"

namespace ncvfs {

namespace {

using StringResult = std::expected<void, QString>;

StringResult send_message(QLocalSocket& socket, QJsonObject message) {
  const auto encoded = protocol::encode(std::move(message));
  if (socket.write(encoded) != encoded.size() || !socket.flush()) {
    return std::unexpected(QStringLiteral("the directory discovery request could not be queued"));
  }
  while (socket.bytesToWrite() > 0) {
    if (!socket.waitForBytesWritten(protocol::connection_timeout_ms)) {
      return std::unexpected(
          QStringLiteral("the directory discovery request could not be sent: %1").arg(socket.errorString()));
    }
  }
  return {};
}

std::expected<QJsonObject, QString> wait_for_message(QLocalSocket& socket, QByteArray& input) {
  for (;;) {
    auto decoded = protocol::take_message(input);
    if (decoded.state == protocol::DecodedMessage::State::Valid) {
      return decoded.object;
    }
    if (decoded.state == protocol::DecodedMessage::State::Invalid) {
      return std::unexpected(decoded.error);
    }
    if (socket.bytesAvailable() > 0) {
      input += socket.readAll();
      continue;
    }
    if (!socket.waitForReadyRead(protocol::watchdog_timeout_ms)) {
      return std::unexpected(
          QStringLiteral("the directory discovery response was unavailable: %1").arg(socket.errorString()));
    }
    input += socket.readAll();
    if (input.size() > 64 * 1024) {
      return std::unexpected(QStringLiteral("the directory discovery response exceeds 64 KiB"));
    }
  }
}

StringResult connect_to_server(QLocalSocket& socket, const QString& name) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(protocol::watchdog_timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    socket.connectToServer(name);
    if (socket.waitForConnected(protocol::connection_timeout_ms)) {
      return {};
    }
    socket.abort();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return std::unexpected(QStringLiteral("the local socket %1 is unavailable: %2").arg(name, socket.errorString()));
}

}  // namespace

DirectoryDiscoverer::DirectoryDiscoverer(std::string store) : store_(std::move(store)) {}

std::string DirectoryDiscoverer::relative_path(int backing_fd) const {
  const auto path = describe_fd(backing_fd);
  if (path == store_) {
    return {};
  }
  if (!encloses(store_, path)) {
    return {};
  }
  const auto offset = store_ == "/" ? 1 : store_.size() + 1;
  return path.substr(offset);
}

int DirectoryDiscoverer::populate(int backing_fd) {
  const auto incomplete = directory_is_incomplete(backing_fd);
  if (!incomplete) {
    fuse_log(FUSE_LOG_ERR, "the directory discovery marker cannot be read: %s\n", std::strerror(incomplete.error()));
    return EIO;
  }
  if (!*incomplete) {
    return 0;
  }

  const auto path = relative_path(backing_fd);
  if (path.empty() && describe_fd(backing_fd) != store_) {
    fuse_log(FUSE_LOG_ERR, "the directory discovery path is outside the backing store\n");
    return EIO;
  }

  std::shared_ptr<ActiveDiscovery> active;
  bool leader = false;
  {
    const std::lock_guard guard(active_mutex_);
    const auto existing = active_.find(path);
    if (existing == active_.end()) {
      active = std::make_shared<ActiveDiscovery>();
      active_.emplace(path, active);
      leader = true;
    } else {
      active = existing->second;
    }
  }

  if (!leader) {
    std::unique_lock lock(active->mutex);
    active->finished.wait(lock, [&] { return active->complete; });
    return active->result;
  }

  const int result = populate_once(path, backing_fd);
  {
    const std::lock_guard guard(active->mutex);
    active->result = result;
    active->complete = true;
  }
  active->finished.notify_all();
  {
    const std::lock_guard guard(active_mutex_);
    active_.erase(path);
  }
  return result;
}

int DirectoryDiscoverer::populate_once(const std::string& relative_path, int backing_fd) {
  QLocalSocket control;
  const auto connected = connect_to_server(control, protocol::control_server_name(QString::fromStdString(store_)));
  if (!connected) {
    fuse_log(FUSE_LOG_ERR, "%s\n", connected.error().toLocal8Bit().constData());
    return EIO;
  }

  const auto requested = send_message(
      control,
      {
          {QStringLiteral("type"), QStringLiteral("discover-directory")},
          {QStringLiteral("path"), QString::fromUtf8(relative_path)},
      });
  if (!requested) {
    fuse_log(FUSE_LOG_ERR, "%s\n", requested.error().toLocal8Bit().constData());
    return EIO;
  }

  QByteArray input;
  const auto response = wait_for_message(control, input);
  if (!response || response->value(QStringLiteral("type")).toString() != QLatin1String("discovery-ready") ||
      !response->value(QStringLiteral("ok")).toBool()) {
    const auto error = response ? response->value(QStringLiteral("error")).toString() : response.error();
    fuse_log(FUSE_LOG_ERR, "%s\n", error.toLocal8Bit().constData());
    return EIO;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(protocol::watchdog_timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto incomplete = directory_is_incomplete(backing_fd);
    if (!incomplete) {
      fuse_log(FUSE_LOG_ERR, "the directory discovery marker cannot be read: %s\n", std::strerror(incomplete.error()));
      return EIO;
    }
    if (!*incomplete) {
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }

  fuse_log(FUSE_LOG_ERR, "directory discovery timed out for %s\n", relative_path.c_str());
  return ETIMEDOUT;
}

}  // namespace ncvfs
