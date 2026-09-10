#include "hydration.hpp"

#include <fcntl.h>
#include <fuse_log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QJsonObject>
#include <QLocalSocket>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include "proc_fd.hpp"
#include "protocol.hpp"
#include "store.hpp"
#include "unique_fd.hpp"

namespace ncvfs {

namespace {

using StringResult = std::expected<void, QString>;

class TransferTarget {
 public:
  explicit TransferTarget(unique_fd descriptor) : descriptor_(std::move(descriptor)) {}
  TransferTarget(const TransferTarget&) = delete;
  TransferTarget& operator=(const TransferTarget&) = delete;
  ~TransferTarget() {
    if (!committed_ && descriptor_ && ::ftruncate(descriptor_.get(), 0) != 0) {
      fuse_log(FUSE_LOG_WARNING, "the failed hydration retained partial backing bytes: %s\n", std::strerror(errno));
    }
  }

  int descriptor() const { return descriptor_.get(); }
  void commit() { committed_ = true; }

 private:
  unique_fd descriptor_;
  bool committed_ = false;
};

std::expected<unique_fd, QString> open_for_hydration(int backing_fd) {
  struct stat attributes = {};
  if (::fstat(backing_fd, &attributes) != 0) {
    return std::unexpected(
        QStringLiteral("the placeholder cannot be inspected: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
  }

  const ProcFdPath path(backing_fd);
  const mode_t permissions = attributes.st_mode & 07777;
  const bool needs_write_permission = (permissions & S_IWUSR) == 0;
  if (needs_write_permission && ::chmod(path.c_str(), permissions | S_IWUSR) != 0) {
    return std::unexpected(QStringLiteral("the placeholder permissions cannot be opened for hydration: %1")
                               .arg(QString::fromLocal8Bit(std::strerror(errno))));
  }

  const auto restore_permissions = [&]() -> StringResult {
    if (!needs_write_permission || ::chmod(path.c_str(), permissions) == 0) {
      return {};
    }
    return std::unexpected(QStringLiteral("the placeholder permissions cannot be restored: %1")
                               .arg(QString::fromLocal8Bit(std::strerror(errno))));
  };

  unique_fd writable(::open(path.c_str(), O_WRONLY | O_CLOEXEC));
  const auto open_failure = errno;
  const auto restored = restore_permissions();
  if (!restored) {
    return std::unexpected(restored.error());
  }
  if (!writable) {
    return std::unexpected(QStringLiteral("the placeholder cannot be opened for hydration: %1")
                               .arg(QString::fromLocal8Bit(std::strerror(open_failure))));
  }
  return writable;
}

StringResult send_message(QLocalSocket& socket, QJsonObject message) {
  const auto encoded = protocol::encode(std::move(message));
  if (socket.write(encoded) != encoded.size() || !socket.flush()) {
    return std::unexpected(QStringLiteral("the hydration control message could not be queued"));
  }
  while (socket.bytesToWrite() > 0) {
    if (!socket.waitForBytesWritten(protocol::connection_timeout_ms)) {
      return std::unexpected(
          QStringLiteral("the hydration control message could not be sent: %1").arg(socket.errorString()));
    }
  }
  return {};
}

std::expected<QJsonObject, QString> wait_for_message(QLocalSocket& socket, QByteArray& input, int timeout_ms) {
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
    if (!socket.waitForReadyRead(timeout_ms)) {
      return std::unexpected(
          QStringLiteral("the hydration control response was unavailable: %1").arg(socket.errorString()));
    }
    input += socket.readAll();
    if (input.size() > 64 * 1024) {
      return std::unexpected(QStringLiteral("the hydration control response exceeds 64 KiB"));
    }
  }
}

StringResult connect_to_server(QLocalSocket& socket, const QString& name) {
  socket.connectToServer(name);
  if (!socket.waitForConnected(protocol::connection_timeout_ms)) {
    return std::unexpected(QStringLiteral("the local socket %1 is unavailable: %2").arg(name, socket.errorString()));
  }
  return {};
}

StringResult write_all(int descriptor, const QByteArray& data) {
  qsizetype offset = 0;
  while (offset < data.size()) {
    const auto written = ::write(descriptor, data.constData() + offset, static_cast<size_t>(data.size() - offset));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(QStringLiteral("the hydration bytes cannot be written: %1")
                                 .arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    if (written == 0) {
      return std::unexpected(QStringLiteral("the hydration write made no progress"));
    }
    offset += static_cast<qsizetype>(written);
  }
  return {};
}

StringResult send_abort(QLocalSocket& control, const QString& error) {
  return send_message(
      control,
      {
          {QStringLiteral("type"), QStringLiteral("abort")},
          {QStringLiteral("error"), error},
      });
}

StringResult expect_final(QLocalSocket& control, QByteArray& input, bool expected_success) {
  const auto final = wait_for_message(control, input, protocol::watchdog_timeout_ms);
  if (!final) {
    return std::unexpected(final.error());
  }
  if (final->value(QStringLiteral("type")).toString() != QLatin1String("final")) {
    return std::unexpected(QStringLiteral("the hydration client returned an unexpected final response"));
  }
  const auto successful = final->value(QStringLiteral("ok")).toBool();
  if (successful != expected_success) {
    auto error = final->value(QStringLiteral("error")).toString();
    if (error.isEmpty()) {
      error = QStringLiteral("the hydration client rejected the transfer");
    }
    return std::unexpected(error);
  }
  return {};
}

StringResult transfer(
    QLocalSocket& control,
    const QString& request_id,
    qint64 expected_size,
    TransferTarget& target,
    qint64& transferred) {
  QLocalSocket cancellation;
  const auto cancellation_connection = connect_to_server(cancellation, request_id + QStringLiteral(":cancellation"));
  if (!cancellation_connection) {
    return std::unexpected(cancellation_connection.error());
  }

  QLocalSocket data;
  const auto data_connection = connect_to_server(data, request_id);
  if (!data_connection) {
    return std::unexpected(data_connection.error());
  }

  auto next_heartbeat = std::chrono::steady_clock::now();
  while (data.state() != QLocalSocket::UnconnectedState || data.bytesAvailable() > 0) {
    data.waitForReadyRead(200);
    const auto bytes = data.readAll();
    if (!bytes.isEmpty()) {
      const auto written = write_all(target.descriptor(), bytes);
      if (!written) {
        return std::unexpected(written.error());
      }
      transferred += bytes.size();
      if (transferred > expected_size) {
        return std::unexpected(QStringLiteral("the hydration transfer exceeds the journal size"));
      }
    }

    cancellation.waitForReadyRead(0);
    if (!cancellation.readAll().isEmpty()) {
      return std::unexpected(QStringLiteral("the hydration transfer was cancelled"));
    }
    if (control.state() == QLocalSocket::UnconnectedState) {
      return std::unexpected(QStringLiteral("the hydration control connection closed during transfer"));
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= next_heartbeat) {
      const auto heartbeat = send_message(
          control,
          {
              {QStringLiteral("type"), QStringLiteral("progress")},
              {QStringLiteral("bytes"), protocol::encoded_integer(transferred)},
          });
      if (!heartbeat) {
        return std::unexpected(heartbeat.error());
      }
      next_heartbeat = now + std::chrono::milliseconds(protocol::heartbeat_interval_ms);
    }
  }

  if (transferred != expected_size) {
    return std::unexpected(
        QStringLiteral("the hydration transfer ended after %1 of %2 bytes").arg(transferred).arg(expected_size));
  }
  return {};
}

}  // namespace

Hydrator::Hydrator(std::string store) : store_(std::move(store)) {}

std::string Hydrator::relative_path(int backing_fd) const {
  const auto path = describe_fd(backing_fd);
  if (!encloses(store_, path)) {
    return {};
  }
  const auto offset = store_ == "/" ? 1 : store_.size() + 1;
  return path.substr(offset);
}

int Hydrator::hydrate(int backing_fd) {
  const auto path = relative_path(backing_fd);
  if (path.empty()) {
    fuse_log(FUSE_LOG_ERR, "the placeholder path is outside the backing store\n");
    return EIO;
  }

  std::shared_ptr<ActiveHydration> active;
  bool leader = false;
  {
    const std::lock_guard guard(active_mutex_);
    const auto existing = active_.find(path);
    if (existing == active_.end()) {
      active = std::make_shared<ActiveHydration>();
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

  const int result = hydrate_once(path, backing_fd);
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

int Hydrator::hydrate_once(const std::string& relative_path, int backing_fd) {
  QLocalSocket control;
  const auto connected = connect_to_server(control, protocol::control_server_name(QString::fromStdString(store_)));
  if (!connected) {
    fuse_log(FUSE_LOG_ERR, "%s\n", connected.error().toLocal8Bit().constData());
    return EIO;
  }

  const auto requested = send_message(
      control,
      {
          {QStringLiteral("type"), QStringLiteral("hydrate")},
          {QStringLiteral("path"), QString::fromUtf8(relative_path)},
      });
  if (!requested) {
    fuse_log(FUSE_LOG_ERR, "%s\n", requested.error().toLocal8Bit().constData());
    return EIO;
  }

  QByteArray control_input;
  const auto ready = wait_for_message(control, control_input, protocol::watchdog_timeout_ms);
  if (!ready || ready->value(QStringLiteral("type")).toString() != QLatin1String("ready")) {
    const auto error =
        ready ? QStringLiteral("the hydration client returned an unexpected ready response") : ready.error();
    fuse_log(FUSE_LOG_ERR, "%s\n", error.toLocal8Bit().constData());
    return EIO;
  }

  const auto request_id = ready->value(QStringLiteral("requestId")).toString();
  qint64 expected_size = -1;
  if (request_id.isEmpty() || !protocol::decode_integer(ready->value(QStringLiteral("size")), expected_size) ||
      expected_size < 0) {
    const auto error = QStringLiteral("the hydration-ready response is missing its request identity or size");
    send_abort(control, error);
    fuse_log(FUSE_LOG_ERR, "%s\n", error.toLocal8Bit().constData());
    return EIO;
  }

  auto writable = open_for_hydration(backing_fd);
  if (!writable) {
    send_abort(control, writable.error());
    fuse_log(FUSE_LOG_ERR, "%s\n", writable.error().toLocal8Bit().constData());
    return EIO;
  }
  TransferTarget target(std::move(*writable));
  if (::ftruncate(target.descriptor(), 0) != 0) {
    const auto error = QStringLiteral("the placeholder cannot be cleared for hydration: %1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
    send_abort(control, error);
    fuse_log(FUSE_LOG_ERR, "%s\n", error.toLocal8Bit().constData());
    return EIO;
  }

  qint64 transferred = 0;
  const auto downloaded = transfer(control, request_id, expected_size, target, transferred);
  if (!downloaded) {
    send_abort(control, downloaded.error());
    expect_final(control, control_input, false);
    fuse_log(FUSE_LOG_ERR, "%s\n", downloaded.error().toLocal8Bit().constData());
    return EIO;
  }

  const auto complete = wait_for_message(control, control_input, protocol::watchdog_timeout_ms);
  if (!complete || complete->value(QStringLiteral("type")).toString() != QLatin1String("complete")) {
    const auto error =
        complete ? QStringLiteral("the hydration client returned an unexpected completion response") : complete.error();
    send_abort(control, error);
    fuse_log(FUSE_LOG_ERR, "%s\n", error.toLocal8Bit().constData());
    return EIO;
  }
  if (complete->value(QStringLiteral("status")).toInt(-1) != 0) {
    auto error = complete->value(QStringLiteral("error")).toString();
    if (error.isEmpty()) {
      error = QStringLiteral("the remote download failed");
    }
    expect_final(control, control_input, false);
    fuse_log(FUSE_LOG_ERR, "%s\n", error.toLocal8Bit().constData());
    return EIO;
  }

  if (::fdatasync(target.descriptor()) != 0) {
    const auto error = QStringLiteral("the hydrated file cannot be synchronized: %1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
    send_abort(control, error);
    expect_final(control, control_input, false);
    fuse_log(FUSE_LOG_ERR, "%s\n", error.toLocal8Bit().constData());
    return EIO;
  }

  const auto committed = send_message(
      control,
      {
          {QStringLiteral("type"), QStringLiteral("commit")},
          {QStringLiteral("bytes"), protocol::encoded_integer(transferred)},
      });
  if (!committed) {
    fuse_log(FUSE_LOG_ERR, "%s\n", committed.error().toLocal8Bit().constData());
    return EIO;
  }
  const auto finalized = expect_final(control, control_input, true);
  if (!finalized) {
    fuse_log(FUSE_LOG_ERR, "%s\n", finalized.error().toLocal8Bit().constData());
    return EIO;
  }

  target.commit();
  return 0;
}

}  // namespace ncvfs
