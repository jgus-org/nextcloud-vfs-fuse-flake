/*
 * SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "vfs_fuse.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <optional>
#include <utility>

#include "../cfapi/hydrationjob.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "filesystem.h"
#include "mount_config.hpp"
#include "protocol.hpp"
#include "replacement_lock.hpp"
#include "syncfileitem.h"
#include "unique_fd.hpp"

Q_LOGGING_CATEGORY(lcVfsFuse, "nextcloud.sync.vfs.fuse", QtInfoMsg)

namespace OCC {

enum class FuseHydrationPhase {
  AwaitingRequest,
  Downloading,
  AwaitingCommit,
};

struct FuseHydrationRequest : QObject {
  explicit FuseHydrationRequest(QLocalSocket* connection, QObject* parent)
      : QObject(parent),
        socket(connection),
        watchdog(new QTimer(this)) {
    watchdog->setSingleShot(true);
    watchdog->setInterval(ncvfs::protocol::watchdog_timeout_ms);
  }

  QLocalSocket* socket;
  QTimer* watchdog;
  QByteArray input;
  FuseHydrationPhase phase = FuseHydrationPhase::AwaitingRequest;
  HydrationJob* job = nullptr;
  SyncJournalFileRecord record;
  bool jobFinished = false;
  bool finalized = false;
  bool restoreOriginalRecord = false;
  QString abortReason;
};

namespace {

constexpr auto placeholderAttributeName = "user.nextcloud.vfs_fuse";
constexpr auto incompleteDirectoryAttributeName = "user.nextcloud.vfs_fuse.incomplete";
constexpr auto supportProbeAttributeName = "user.nextcloud.vfs_fuse.probe";
constexpr auto supportProbeAttributeValue = "probe";
constexpr auto demandDiscoveryInitializedKey = "vfs-fuse-demand-discovery-initialized";

QString configuredUserFacingPath(const QString& store) {
  const auto storePath = ncvfs::normalized_directory(store);
  const auto configPath = ncvfs::default_mount_config_path();
  if (QFileInfo::exists(configPath)) {
    QString error;
    const auto mappings = ncvfs::load_mount_mappings(configPath, error);
    if (!mappings) {
      qCWarning(lcVfsFuse) << error;
    } else {
      for (const auto& mapping : *mappings) {
        if (mapping.store == storePath) {
          return mapping.mount + QLatin1Char('/');
        }
      }
    }
  }
  return storePath + QLatin1Char('/');
}

bool setAttribute(const QByteArray& path, const char* name, const QByteArray& value) {
  return setxattr(path.constData(), name, value.constData(), static_cast<size_t>(value.size()), 0) == 0;
}

bool removeAttribute(const QByteArray& path, const char* name) { return removexattr(path.constData(), name) == 0; }

QString filesystemError(QString condition, int failure) {
  return QStringLiteral("%1: %2").arg(condition, QString::fromLocal8Bit(std::strerror(failure)));
}

Result<std::optional<qint64>, QString> readPlaceholderMarker(const QByteArray& path) {
  char value[64];
  const auto length = getxattr(path.constData(), placeholderAttributeName, value, sizeof value);
  if (length < 0) {
    const auto failure = errno;
    if (failure == ENODATA) {
      return std::optional<qint64>{};
    }
    return QStringLiteral("Could not read %1 from %2: %3")
        .arg(
            QString::fromLatin1(placeholderAttributeName),
            QString::fromUtf8(path),
            QString::fromLocal8Bit(std::strerror(failure)));
  }

  qint64 remoteSize = 0;
  const char* const end = value + length;
  const auto parsed = std::from_chars(value, end, remoteSize);
  if (parsed.ec != std::errc{} || parsed.ptr != end || remoteSize < 0) {
    return QStringLiteral("The %1 value on %2 is not a non-negative decimal size")
        .arg(QString::fromLatin1(placeholderAttributeName), QString::fromUtf8(path));
  }
  return std::optional<qint64>{remoteSize};
}

Result<void, QString> setPlaceholderMarker(const QString& path, qint64 remoteSize) {
  if (remoteSize < 0) {
    return QStringLiteral("The placeholder size for %1 is negative: %2").arg(path).arg(remoteSize);
  }

  const auto localPath = path.toUtf8();
  const auto encodedSize = QByteArray::number(remoteSize);
  const auto writeMarker = [&]() -> Result<void, QString> {
    if (setxattr(
            localPath.constData(),
            placeholderAttributeName,
            encodedSize.constData(),
            static_cast<size_t>(encodedSize.size()),
            0) == 0) {
      return {};
    }
    const auto failure = errno;
    return filesystemError(
        QStringLiteral("Could not write %1 on %2")
            .arg(QString::fromLatin1(placeholderAttributeName), QString::fromUtf8(localPath)),
        failure);
  };

  struct stat attributes = {};
  if (::lstat(localPath.constData(), &attributes) != 0) {
    const auto failure = errno;
    return filesystemError(QStringLiteral("Could not examine %1").arg(QString::fromUtf8(localPath)), failure);
  }
  const mode_t permissions = attributes.st_mode & 07777;
  if ((permissions & S_IWUSR) != 0) {
    return writeMarker();
  }

  if (::chmod(localPath.constData(), permissions | S_IWUSR) != 0) {
    const auto failure = errno;
    return filesystemError(
        QStringLiteral("Could not temporarily add owner-write permission to %1").arg(QString::fromUtf8(localPath)),
        failure);
  }
  const auto marked = writeMarker();
  if (::chmod(localPath.constData(), permissions) != 0) {
    const auto failure = errno;
    const auto restorationError = filesystemError(
        QStringLiteral("Could not restore permissions on %1").arg(QString::fromUtf8(localPath)),
        failure);
    return marked ? restorationError : QStringLiteral("%1; %2").arg(marked.error(), restorationError);
  }
  if (!marked) {
    return marked.error();
  }
  return {};
}

Result<void, QString> removePlaceholderMarker(const QByteArray& path) {
  const auto removeMarker = [&]() -> Result<void, QString> {
    if (removexattr(path.constData(), placeholderAttributeName) == 0) {
      return {};
    }
    const auto failure = errno;
    if (failure == ENODATA) {
      return {};
    }
    return filesystemError(
        QStringLiteral("Could not remove %1 from %2")
            .arg(QString::fromLatin1(placeholderAttributeName), QString::fromUtf8(path)),
        failure);
  };

  struct stat attributes = {};
  if (::lstat(path.constData(), &attributes) != 0) {
    const auto failure = errno;
    return filesystemError(QStringLiteral("Could not examine %1").arg(QString::fromUtf8(path)), failure);
  }
  const mode_t permissions = attributes.st_mode & 07777;
  if ((permissions & S_IWUSR) != 0) {
    return removeMarker();
  }

  if (::chmod(path.constData(), permissions | S_IWUSR) != 0) {
    const auto failure = errno;
    return filesystemError(
        QStringLiteral("Could not temporarily add owner-write permission to %1").arg(QString::fromUtf8(path)),
        failure);
  }
  const auto removed = removeMarker();
  if (::chmod(path.constData(), permissions) != 0) {
    const auto failure = errno;
    const auto restorationError =
        filesystemError(QStringLiteral("Could not restore permissions on %1").arg(QString::fromUtf8(path)), failure);
    return removed ? restorationError : QStringLiteral("%1; %2").arg(removed.error(), restorationError);
  }
  if (!removed) {
    return removed.error();
  }
  return {};
}

Result<void, QString> setIncompleteDirectoryMarker(const QString& path) {
  const auto localPath = path.toUtf8();
  const auto writeMarker = [&]() -> Result<void, QString> {
    if (setAttribute(localPath, incompleteDirectoryAttributeName, QByteArrayLiteral("1"))) {
      return {};
    }
    return filesystemError(
        QStringLiteral("Could not write %1 on %2")
            .arg(QString::fromLatin1(incompleteDirectoryAttributeName), QString::fromUtf8(localPath)),
        errno);
  };

  struct stat attributes = {};
  if (::lstat(localPath.constData(), &attributes) != 0) {
    return filesystemError(QStringLiteral("Could not examine %1").arg(QString::fromUtf8(localPath)), errno);
  }
  const mode_t permissions = attributes.st_mode & 07777;
  if ((permissions & S_IWUSR) != 0) {
    return writeMarker();
  }
  if (::chmod(localPath.constData(), permissions | S_IWUSR) != 0) {
    return filesystemError(
        QStringLiteral("Could not temporarily add owner-write permission to %1").arg(QString::fromUtf8(localPath)),
        errno);
  }
  const auto marked = writeMarker();
  if (::chmod(localPath.constData(), permissions) != 0) {
    const auto restorationError =
        filesystemError(QStringLiteral("Could not restore permissions on %1").arg(QString::fromUtf8(localPath)), errno);
    return marked ? restorationError : QStringLiteral("%1; %2").arg(marked.error(), restorationError);
  }
  return marked;
}

Result<void, QString> removeIncompleteDirectoryMarker(const QString& path) {
  const auto localPath = path.toUtf8();
  const auto removeMarker = [&]() -> Result<void, QString> {
    if (removeAttribute(localPath, incompleteDirectoryAttributeName) || errno == ENODATA) {
      return {};
    }
    return filesystemError(
        QStringLiteral("Could not remove %1 from %2")
            .arg(QString::fromLatin1(incompleteDirectoryAttributeName), QString::fromUtf8(localPath)),
        errno);
  };

  struct stat attributes = {};
  if (::lstat(localPath.constData(), &attributes) != 0) {
    return filesystemError(QStringLiteral("Could not examine %1").arg(QString::fromUtf8(localPath)), errno);
  }
  const mode_t permissions = attributes.st_mode & 07777;
  if ((permissions & S_IWUSR) != 0) {
    return removeMarker();
  }
  if (::chmod(localPath.constData(), permissions | S_IWUSR) != 0) {
    return filesystemError(
        QStringLiteral("Could not temporarily add owner-write permission to %1").arg(QString::fromUtf8(localPath)),
        errno);
  }
  const auto removed = removeMarker();
  if (::chmod(localPath.constData(), permissions) != 0) {
    const auto restorationError =
        filesystemError(QStringLiteral("Could not restore permissions on %1").arg(QString::fromUtf8(localPath)), errno);
    return removed ? restorationError : QStringLiteral("%1; %2").arg(removed.error(), restorationError);
  }
  return removed;
}

Result<bool, QString> directoryIsIncomplete(const QString& path) {
  const auto localPath = path.toUtf8();
  char value;
  if (getxattr(localPath.constData(), incompleteDirectoryAttributeName, &value, sizeof value) >= 0) {
    return true;
  }
  if (errno == ENODATA) {
    return false;
  }
  return filesystemError(
      QStringLiteral("Could not read %1 from %2")
          .arg(QString::fromLatin1(incompleteDirectoryAttributeName), QString::fromUtf8(localPath)),
      errno);
}

Result<void, QString> copyExtendedAttributes(const QByteArray& source, const QByteArray& destination) {
  QByteArray names;
  for (;;) {
    const auto required = llistxattr(source.constData(), nullptr, 0);
    if (required < 0) {
      const auto failure = errno;
      return filesystemError(
          QStringLiteral("Could not list extended attributes on %1").arg(QString::fromUtf8(source)),
          failure);
    }
    names.resize(static_cast<qsizetype>(required));
    if (names.isEmpty()) {
      return {};
    }
    const auto listed = llistxattr(source.constData(), names.data(), static_cast<size_t>(names.size()));
    if (listed >= 0) {
      names.resize(static_cast<qsizetype>(listed));
      break;
    }
    const auto failure = errno;
    if (failure != ERANGE) {
      return filesystemError(
          QStringLiteral("Could not list extended attributes on %1").arg(QString::fromUtf8(source)),
          failure);
    }
  }

  for (qsizetype offset = 0; offset < names.size();) {
    const char* const name = names.constData() + offset;
    offset += static_cast<qsizetype>(std::strlen(name)) + 1;
    if (std::strcmp(name, placeholderAttributeName) == 0 || std::strcmp(name, supportProbeAttributeName) == 0) {
      continue;
    }

    for (;;) {
      const auto required = lgetxattr(source.constData(), name, nullptr, 0);
      if (required < 0) {
        const auto failure = errno;
        if (failure == ENODATA) {
          break;
        }
        return filesystemError(
            QStringLiteral("Could not read extended attribute %1 from %2")
                .arg(QString::fromUtf8(name), QString::fromUtf8(source)),
            failure);
      }
      QByteArray value(static_cast<qsizetype>(required), '\0');
      const auto copied = lgetxattr(source.constData(), name, value.data(), static_cast<size_t>(value.size()));
      if (copied < 0) {
        const auto failure = errno;
        if (failure == ERANGE) {
          continue;
        }
        if (failure == ENODATA) {
          break;
        }
        return filesystemError(
            QStringLiteral("Could not read extended attribute %1 from %2")
                .arg(QString::fromUtf8(name), QString::fromUtf8(source)),
            failure);
      }
      value.resize(static_cast<qsizetype>(copied));
      if (lsetxattr(destination.constData(), name, value.constData(), static_cast<size_t>(value.size()), 0) != 0) {
        const auto failure = errno;
        return filesystemError(
            QStringLiteral("Could not write extended attribute %1 on %2")
                .arg(QString::fromUtf8(name), QString::fromUtf8(destination)),
            failure);
      }
      break;
    }
  }
  return {};
}

constexpr auto temporaryNameAttempts = 8;
constexpr auto hiddenPrefix = '.';

QByteArray truncatedToWholeCharacters(const QByteArray& name, qsizetype limit) {
  if (name.size() <= limit) {
    return name;
  }

  auto cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return name.left(cut);
}

QString temporaryPlaceholderPath(const QString& targetPath) {
  const auto separator = targetPath.lastIndexOf(QLatin1Char('/'));
  const auto directory = targetPath.left(separator + 1);
  const auto suffix =
      QStringLiteral(".~%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0')).toUtf8();
  const auto name = truncatedToWholeCharacters(
      targetPath.mid(separator + 1).toUtf8(),
      NAME_MAX - suffix.size() - sizeof(hiddenPrefix));
  return directory + QString::fromUtf8(QByteArray(1, hiddenPrefix) + name + suffix);
}

class PendingPlaceholder {
 public:
  explicit PendingPlaceholder(QString targetPath) : _targetPath(std::move(targetPath)) {
    for (auto attempt = 0; attempt < temporaryNameAttempts; ++attempt) {
      QFile candidate(temporaryPlaceholderPath(_targetPath));
      if (candidate.open(QFile::WriteOnly | QFile::NewOnly)) {
        candidate.close();
        _path = candidate.fileName();
        return;
      }
    }
  }

  PendingPlaceholder(const PendingPlaceholder&) = delete;
  PendingPlaceholder& operator=(const PendingPlaceholder&) = delete;

  ~PendingPlaceholder() {
    if (!_path.isEmpty()) {
      QFile::remove(_path);
    }
  }

  explicit operator bool() const { return !_path.isEmpty(); }

  const QString& path() const { return _path; }

  Result<void, QString> inheritTargetMetadata() {
    const auto targetPath = _targetPath.toUtf8();
    const auto pendingPath = _path.toUtf8();
    struct stat attributes = {};
    if (::lstat(targetPath.constData(), &attributes) != 0) {
      const auto failure = errno;
      if (failure == ENOENT) {
        return {};
      }
      return filesystemError(QStringLiteral("Could not examine %1").arg(QString::fromUtf8(targetPath)), failure);
    }
    struct stat pendingAttributes = {};
    if (::lstat(pendingPath.constData(), &pendingAttributes) != 0) {
      const auto failure = errno;
      return filesystemError(QStringLiteral("Could not examine %1").arg(QString::fromUtf8(pendingPath)), failure);
    }
    if ((pendingAttributes.st_uid != attributes.st_uid || pendingAttributes.st_gid != attributes.st_gid) &&
        ::chown(pendingPath.constData(), attributes.st_uid, attributes.st_gid) != 0) {
      const auto failure = errno;
      return filesystemError(
          QStringLiteral("Could not copy the ownership of %1 to %2")
              .arg(QString::fromUtf8(targetPath), QString::fromUtf8(pendingPath)),
          failure);
    }
    const auto copiedAttributes = copyExtendedAttributes(targetPath, pendingPath);
    if (!copiedAttributes) {
      return copiedAttributes.error();
    }
    if (::chmod(pendingPath.constData(), attributes.st_mode & 07777) != 0) {
      const auto failure = errno;
      return filesystemError(
          QStringLiteral("Could not copy the permissions of %1 to %2")
              .arg(QString::fromUtf8(targetPath), QString::fromUtf8(pendingPath)),
          failure);
    }
    return {};
  }

  Result<void, QString> replaceTarget(bool targetExisted) {
    const auto pendingPath = _path.toUtf8();
    const auto targetPath = _targetPath.toUtf8();
    const auto result =
        targetExisted
            ? ::rename(pendingPath.constData(), targetPath.constData())
            : ::renameat2(AT_FDCWD, pendingPath.constData(), AT_FDCWD, targetPath.constData(), RENAME_NOREPLACE);
    if (result != 0) {
      const auto failure = errno;
      return filesystemError(
          QStringLiteral("Could not install %2 at %1")
              .arg(QString::fromUtf8(targetPath), QString::fromUtf8(pendingPath)),
          failure);
    }
    _path.clear();
    return {};
  }

 private:
  QString _targetPath;
  QString _path;
};

bool isValidHydrationPath(const QString& path) {
  if (path.isEmpty() || QDir::isAbsolutePath(path) || path.endsWith(QLatin1Char('/')) ||
      QDir::cleanPath(path) != path) {
    return false;
  }
  const auto components = path.split(QLatin1Char('/'));
  return std::none_of(components.cbegin(), components.cend(), [](const QString& component) {
    return component.isEmpty() || component == QLatin1String(".") || component == QLatin1String("..");
  });
}

bool isValidControlPath(const QString& path) {
  if (path.isEmpty()) {
    return true;
  }
  if (QDir::isAbsolutePath(path) || path.endsWith(QLatin1Char('/')) || QDir::cleanPath(path) != path) {
    return false;
  }
  const auto components = path.split(QLatin1Char('/'));
  return std::none_of(components.cbegin(), components.cend(), [](const QString& component) {
    return component.isEmpty() || component == QLatin1String(".") || component == QLatin1String("..");
  });
}

QString pinStateName(PinState state) {
  switch (state) {
    case PinState::Inherited:
      return QStringLiteral("inherit");
    case PinState::AlwaysLocal:
      return QStringLiteral("always-local");
    case PinState::OnlineOnly:
      return QStringLiteral("online-only");
    case PinState::Unspecified:
      return QStringLiteral("automatic");
    case PinState::Excluded:
      return QStringLiteral("excluded");
  }
  return QStringLiteral("invalid");
}

std::optional<PinState> pinStateFromName(const QString& name) {
  if (name == QLatin1String("inherit")) {
    return PinState::Inherited;
  }
  if (name == QLatin1String("always-local")) {
    return PinState::AlwaysLocal;
  }
  if (name == QLatin1String("online-only")) {
    return PinState::OnlineOnly;
  }
  if (name == QLatin1String("automatic")) {
    return PinState::Unspecified;
  }
  return std::nullopt;
}

QString availabilityName(VfsItemAvailability availability) {
  switch (availability) {
    case VfsItemAvailability::AlwaysLocal:
      return QStringLiteral("always-local");
    case VfsItemAvailability::AllHydrated:
      return QStringLiteral("local");
    case VfsItemAvailability::Mixed:
      return QStringLiteral("mixed");
    case VfsItemAvailability::AllDehydrated:
      return QStringLiteral("dehydrated");
    case VfsItemAvailability::OnlineOnly:
      return QStringLiteral("online-only");
  }
  return QStringLiteral("invalid");
}

QString hydrationFailure(const HydrationJob& job) {
  if (!job.errorString().isEmpty()) {
    return job.errorString();
  }
  switch (job.status()) {
    case HydrationJob::Success:
      return {};
    case HydrationJob::Error:
      return QStringLiteral("the remote download failed");
    case HydrationJob::Cancelled:
      return QStringLiteral("the hydration request was cancelled");
  }
  return QStringLiteral("the hydration job returned an invalid status");
}

class ReplacementLease {
 public:
  ReplacementLease(const QString& store, const QString& target) {
    const auto encodedTarget = target.toUtf8();
    if (::lstat(encodedTarget.constData(), &attributes_) != 0) {
      const auto failure = errno;
      if (failure == ENOENT) {
        targetExisted_ = false;
        ready_ = true;
      } else {
        error_ = filesystemError(QStringLiteral("Could not examine %1").arg(target), failure);
      }
      return;
    }
    targetExisted_ = true;

    const auto encodedLockPath = QFile::encodeName(ncvfs::replacement_lock_path(store));
    descriptor_.reset(::open(encodedLockPath.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!descriptor_) {
      const auto failure = errno;
      error_ = filesystemError(QStringLiteral("Could not open the replacement lease for %1").arg(target), failure);
      return;
    }
    if (ncvfs::set_replacement_lock(descriptor_.get(), attributes_, F_WRLCK) != 0) {
      const auto failure = errno;
      error_ = failure == EAGAIN || failure == EACCES
                   ? QStringLiteral("%1 is open through the FUSE mount").arg(target)
                   : filesystemError(QStringLiteral("Could not reserve %1 for replacement").arg(target), failure);
      return;
    }

    struct stat current = {};
    if (::lstat(encodedTarget.constData(), &current) != 0 || current.st_dev != attributes_.st_dev ||
        current.st_ino != attributes_.st_ino) {
      error_ = QStringLiteral("%1 changed while it was reserved for replacement").arg(target);
      return;
    }
    ready_ = true;
  }

  explicit operator bool() const { return ready_; }
  const QString& error() const { return error_; }
  bool targetExisted() const { return targetExisted_; }

 private:
  ncvfs::unique_fd descriptor_;
  struct stat attributes_ = {};
  QString error_;
  bool ready_ = false;
  bool targetExisted_ = false;
};

}  // namespace

VfsFuse::VfsFuse(QObject* parent) : Vfs(parent) {}

VfsFuse::~VfsFuse() = default;

Vfs::Mode VfsFuse::mode() const { return Fuse; }

QString VfsFuse::fileSuffix() const { return QString(); }

QString VfsFuse::userFacingPath() const { return _userFacingPath; }

void VfsFuse::startImpl(const VfsSetupParams& params) {
  qCInfo(lcVfsFuse) << "Starting for sync root" << params.filesystemPath;

  _userFacingPath = configuredUserFacingPath(params.filesystemPath);

  const auto syncRoot = params.filesystemPath.toUtf8();
  _extendedAttributesUsable = setAttribute(syncRoot, supportProbeAttributeName, supportProbeAttributeValue) &&
                              removeAttribute(syncRoot, supportProbeAttributeName);
  if (!_extendedAttributesUsable) {
    qCCritical(lcVfsFuse) << "Extended attributes are unusable below" << params.filesystemPath
                          << "- placeholders cannot be distinguished from hydrated files";
  }

  if (_demandDrivenDiscoveryEnabled &&
      params.journal->keyValueStoreGetInt(QString::fromLatin1(demandDiscoveryInitializedKey), 0) == 0) {
    const auto marked = setIncompleteDirectoryMarker(params.filesystemPath);
    if (!marked) {
      qCCritical(lcVfsFuse) << marked.error();
    }
  }

  _hydrationServer = new QLocalServer(this);
  _hydrationServer->setSocketOptions(QLocalServer::UserAccessOption);
  const auto serverName = ncvfs::protocol::control_server_name(params.filesystemPath);
  if (!_hydrationServer->listen(serverName)) {
    QLocalSocket existingServer;
    existingServer.connectToServer(serverName);
    if (!existingServer.waitForConnected(100) && QLocalServer::removeServer(serverName)) {
      _hydrationServer->listen(serverName);
    }
    if (!_hydrationServer->isListening()) {
      qCCritical(lcVfsFuse) << "The hydration control server cannot listen on" << serverName << ':'
                            << _hydrationServer->errorString();
      return;
    }
  }
  connect(_hydrationServer, &QLocalServer::newConnection, this, &VfsFuse::acceptHydrationConnections);
}

void VfsFuse::stop() {
  const auto requests = _hydrationRequests.values();
  for (auto* request : requests) {
    abortHydration(request, tr("The virtual-files backend stopped"));
  }
  if (_hydrationServer) {
    _hydrationServer->close();
    _hydrationServer->deleteLater();
    _hydrationServer = nullptr;
  }
}

void VfsFuse::unregisterFolder() { stop(); }

bool VfsFuse::socketApiPinStateActionsShown() const { return true; }

bool VfsFuse::isHydrating() const {
  return std::any_of(_hydrationRequests.cbegin(), _hydrationRequests.cend(), [](const FuseHydrationRequest* request) {
    return request->job != nullptr && !request->finalized;
  });
}

void VfsFuse::setDemandDrivenDiscoveryEnabled(bool enabled) { _demandDrivenDiscoveryEnabled = enabled; }

bool VfsFuse::demandDrivenDiscoveryEnabled() const { return _demandDrivenDiscoveryEnabled; }

void VfsFuse::initialDiscoveryFinished() {
  if (!_demandDrivenDiscoveryEnabled) {
    return;
  }
  const auto completed = removeIncompleteDirectoryMarker(params().filesystemPath);
  if (!completed) {
    qCWarning(lcVfsFuse) << completed.error();
    return;
  }
  params().journal->keyValueStoreSet(QString::fromLatin1(demandDiscoveryInitializedKey), 1);
}

void VfsFuse::acceptHydrationConnections() {
  while (_hydrationServer && _hydrationServer->hasPendingConnections()) {
    auto* socket = _hydrationServer->nextPendingConnection();
    auto* request = new FuseHydrationRequest(socket, this);
    _hydrationRequests.insert(socket, request);
    connect(socket, &QLocalSocket::readyRead, this, [this, request] { processHydrationInput(request); });
    connect(socket, &QLocalSocket::disconnected, this, [this, request] {
      abortHydration(request, tr("The hydration control connection closed"));
    });
    connect(request->watchdog, &QTimer::timeout, this, [this, request] {
      abortHydration(request, tr("The hydration control connection stopped responding"));
    });
    request->watchdog->start();
  }
}

bool VfsFuse::sendHydrationMessage(FuseHydrationRequest* request, QJsonObject message) {
  if (!request->socket || request->socket->state() != QLocalSocket::ConnectedState) {
    return false;
  }
  const auto encoded = ncvfs::protocol::encode(std::move(message));
  if (request->socket->write(encoded) != encoded.size()) {
    return false;
  }
  return request->socket->flush();
}

void VfsFuse::processHydrationInput(FuseHydrationRequest* request) {
  if (request->finalized) {
    return;
  }
  request->input += request->socket->readAll();
  if (request->input.size() > 64 * 1024) {
    abortHydration(request, tr("The hydration control message exceeds 64 KiB"));
    return;
  }

  for (;;) {
    auto decoded = ncvfs::protocol::take_message(request->input);
    if (decoded.state == ncvfs::protocol::DecodedMessage::State::Incomplete) {
      return;
    }
    if (decoded.state == ncvfs::protocol::DecodedMessage::State::Invalid) {
      abortHydration(request, decoded.error);
      return;
    }

    request->watchdog->start();
    const auto type = decoded.object.value(QStringLiteral("type")).toString();
    if (request->phase == FuseHydrationPhase::AwaitingRequest) {
      if (type == QLatin1String("hydrate")) {
        beginHydration(request, decoded.object.value(QStringLiteral("path")).toString());
        if (request->finalized) {
          return;
        }
        continue;
      }
      processControlRequest(request, decoded.object);
      return;
    }

    if (type == QLatin1String("progress") && request->phase != FuseHydrationPhase::AwaitingRequest) {
      continue;
    }
    if (type == QLatin1String("abort")) {
      auto error = decoded.object.value(QStringLiteral("error")).toString();
      if (error.isEmpty()) {
        error = tr("The daemon aborted the hydration request");
      }
      abortHydration(request, error);
      return;
    }
    if (type == QLatin1String("commit") && request->phase == FuseHydrationPhase::AwaitingCommit) {
      qint64 transferredBytes = -1;
      if (!ncvfs::protocol::decode_integer(decoded.object.value(QStringLiteral("bytes")), transferredBytes)) {
        abortHydration(request, tr("The hydration commit has an invalid byte count"));
        return;
      }
      finalizeHydration(request, true, transferredBytes, {});
      return;
    }

    abortHydration(request, tr("The hydration control message is invalid in the current state"));
    return;
  }
}

void VfsFuse::processControlRequest(FuseHydrationRequest* request, const QJsonObject& message) {
  const auto type = message.value(QStringLiteral("type")).toString();
  const auto folderPath = message.value(QStringLiteral("path")).toString();
  QJsonObject response{
      {QStringLiteral("type"), QStringLiteral("%1-result").arg(type)},
      {QStringLiteral("path"), folderPath},
      {QStringLiteral("ok"), false},
  };

  const auto reject = [&](const QString& error) {
    response.insert(QStringLiteral("error"), error);
    finishControlRequest(request, std::move(response));
  };

  if (!isValidControlPath(folderPath)) {
    reject(tr("The control path is invalid"));
    return;
  }

  if (!folderPath.isEmpty()) {
    SyncJournalFileRecord record;
    if (!params().journal->getFileRecord(folderPath, &record) || !record.isValid()) {
      reject(tr("The control path is absent from the sync journal"));
      return;
    }
  }

  if (type == QLatin1String("vfs-status")) {
    const auto explicitPin = params().journal->internalPinStates().rawForPath(folderPath.toUtf8());
    const auto effectivePin = pinState(folderPath);
    const auto currentAvailability = availability(folderPath, AvailabilityRecursivity::RecursiveAvailability);
    if (!explicitPin || !effectivePin) {
      reject(tr("The pin state could not be read"));
      return;
    }
    if (!currentAvailability) {
      if (currentAvailability.error() != AvailabilityError::NoSuchItem) {
        reject(tr("The availability could not be read"));
        return;
      }
      response.insert(QStringLiteral("availability"), QStringLiteral("empty"));
    } else {
      response.insert(QStringLiteral("availability"), availabilityName(*currentAvailability));
    }
    response.insert(QStringLiteral("policy"), pinStateName(*explicitPin));
    response.insert(QStringLiteral("effectivePolicy"), pinStateName(*effectivePin));
    response.insert(QStringLiteral("ok"), true);
    finishControlRequest(request, std::move(response));
    return;
  }

  if (type == QLatin1String("vfs-set-pin-state")) {
    const auto stateName = message.value(QStringLiteral("state")).toString();
    const auto state = pinStateFromName(stateName);
    if (!state) {
      reject(tr("The requested pin state is invalid"));
      return;
    }
    if (!setPinState(folderPath, *state)) {
      reject(tr("The pin state could not be changed"));
      return;
    }
    response.insert(QStringLiteral("state"), stateName);
    response.insert(QStringLiteral("ok"), true);
    finishControlRequest(request, std::move(response));
    emit syncRequested(folderPath);
    return;
  }

  if (type == QLatin1String("discover-directory")) {
    const auto localPath = params().filesystemPath + folderPath;
    const auto incomplete = directoryIsIncomplete(localPath);
    if (!incomplete) {
      reject(incomplete.error());
      return;
    }
    if (!*incomplete) {
      reject(tr("The directory is not awaiting discovery"));
      return;
    }
    if (!folderPath.isEmpty()) {
      SyncJournalFileRecord record;
      if (!params().journal->getFileRecord(folderPath, &record) || !record.isValid() ||
          record._type != ItemTypeVirtualDirectory) {
        reject(tr("The directory is absent from the virtual-directory journal"));
        return;
      }
      params().journal->schedulePathForRemoteDiscovery(folderPath);
    }
    response.insert(QStringLiteral("type"), QStringLiteral("discovery-ready"));
    response.insert(QStringLiteral("ok"), true);
    finishControlRequest(request, std::move(response));
    emit syncRequested(folderPath);
    return;
  }

  reject(tr("The control request type is invalid"));
}

void VfsFuse::finishControlRequest(FuseHydrationRequest* request, QJsonObject response) {
  if (request->finalized) {
    return;
  }
  request->finalized = true;
  request->watchdog->stop();
  sendHydrationMessage(request, std::move(response));

  auto* socket = request->socket;
  _hydrationRequests.remove(socket);
  QObject::disconnect(socket, nullptr, this, nullptr);
  socket->disconnectFromServer();
  socket->deleteLater();
  request->deleteLater();
}

void VfsFuse::beginHydration(FuseHydrationRequest* request, const QString& folderPath) {
  if (!isValidHydrationPath(folderPath)) {
    abortHydration(request, tr("The hydration path is invalid"));
    return;
  }

  if (!params().journal->getFileRecord(folderPath, &request->record) || !request->record.isValid()) {
    abortHydration(request, tr("The hydration path is absent from the sync journal"));
    return;
  }
  if (!request->record.isVirtualFile()) {
    abortHydration(request, tr("The hydration path is not a virtual file"));
    return;
  }

  const auto path = params().filesystemPath + folderPath;
  const auto marker = readPlaceholderMarker(path.toUtf8());
  if (!marker || !marker->has_value() || marker->value() != request->record._fileSize) {
    abortHydration(request, tr("The hydration path is not a valid placeholder"));
    return;
  }

  const auto duplicate = std::any_of(
      _hydrationRequests.cbegin(),
      _hydrationRequests.cend(),
      [request, &folderPath](const FuseHydrationRequest* active) {
        return active != request && active->job != nullptr && !active->finalized && active->record.path() == folderPath;
      });
  if (duplicate) {
    abortHydration(request, tr("The hydration path already has an active request"));
    return;
  }

  const auto wasHydrating = isHydrating();
  request->phase = FuseHydrationPhase::Downloading;
  request->job = new HydrationJob(this);
  request->job->setAccount(params().account);
  request->job->setRemoteSyncRootPath(params().remotePath);
  request->job->setLocalPath(params().filesystemPath);
  request->job->setJournal(params().journal);
  request->job->setRequestId(QUuid::createUuid().toString(QUuid::WithoutBraces));
  request->job->setFolderPath(folderPath);
  request->job->setIsEncryptedFile(request->record.isE2eEncrypted());
  request->job->setE2eMangledName(request->record._e2eMangledName);
  request->job->setFileTotalSize(request->record._fileSize);
  connect(request->job, &HydrationJob::finished, this, [this, request] { hydrationJobFinished(request); });

  if (!wasHydrating) {
    emit beginHydrating();
  }
  request->job->start();
  if (request->finalized) {
    return;
  }

  QJsonObject ready{
      {QStringLiteral("type"), QStringLiteral("ready")},
      {QStringLiteral("requestId"), request->job->requestId()},
      {QStringLiteral("size"), ncvfs::protocol::encoded_integer(request->record._fileSize)},
      {QStringLiteral("mtime"), ncvfs::protocol::encoded_integer(request->record._modtime)},
  };
  if (!sendHydrationMessage(request, std::move(ready))) {
    abortHydration(request, tr("The hydration-ready response could not be sent"));
  }
}

void VfsFuse::hydrationJobFinished(FuseHydrationRequest* request) {
  if (request->finalized || request->jobFinished) {
    return;
  }
  request->jobFinished = true;
  request->phase = FuseHydrationPhase::AwaitingCommit;
  request->watchdog->start();

  QJsonObject complete{
      {QStringLiteral("type"), QStringLiteral("complete")},
      {QStringLiteral("status"), static_cast<int>(request->job->status())},
      {QStringLiteral("errorCode"), request->job->errorCode()},
      {QStringLiteral("statusCode"), request->job->statusCode()},
      {QStringLiteral("error"), request->job->errorString()},
  };
  if (!sendHydrationMessage(request, std::move(complete))) {
    abortHydration(request, tr("The hydration-complete response could not be sent"));
    return;
  }
  if (request->job->status() != HydrationJob::Success) {
    const auto error = request->abortReason.isEmpty() ? hydrationFailure(*request->job) : request->abortReason;
    finalizeHydration(request, false, 0, error);
  }
}

void VfsFuse::abortHydration(FuseHydrationRequest* request, const QString& error) {
  if (request->finalized) {
    return;
  }
  request->abortReason = error;
  if (request->job && !request->jobFinished) {
    request->job->cancel();
    if (request->jobFinished || request->finalized) {
      return;
    }
  }
  finalizeHydration(request, false, 0, error);
}

Result<void, QString> VfsFuse::commitHydration(FuseHydrationRequest* request, qint64 transferredBytes) {
  if (!request->jobFinished || request->job->status() != HydrationJob::Success) {
    return tr("The remote download did not finish successfully");
  }
  if (transferredBytes != request->record._fileSize) {
    return tr("The hydration transferred %1 bytes for a %2-byte file")
        .arg(transferredBytes)
        .arg(request->record._fileSize);
  }

  SyncJournalFileRecord currentRecord;
  if (!params().journal->getFileRecord(request->record.path(), &currentRecord) || !currentRecord.isValid() ||
      !currentRecord.isVirtualFile() || currentRecord._fileId != request->record._fileId ||
      currentRecord._etag != request->record._etag || currentRecord._fileSize != request->record._fileSize ||
      currentRecord._modtime != request->record._modtime) {
    return tr("The sync journal changed during hydration");
  }

  const auto path = params().filesystemPath + request->record.path();
  const auto marker = readPlaceholderMarker(path.toUtf8());
  if (!marker || !marker->has_value() || marker->value() != request->record._fileSize) {
    return tr("The hydrated file lost its placeholder marker before commit");
  }
  if (FileSystem::getSize(path) != transferredBytes) {
    return tr("The hydrated file's backing size does not match the transferred byte count");
  }
  if (!FileSystem::setModTime(path, request->record._modtime)) {
    return tr("The hydrated file's modification time could not be restored");
  }
  request->restoreOriginalRecord = true;
  const auto unmarked = removePlaceholderMarker(path.toUtf8());
  if (!unmarked) {
    return unmarked.error();
  }

  request->job->finalize(this);
  SyncJournalFileRecord committed;
  if (!params().journal->getFileRecord(request->record.path(), &committed) || !committed.isValid() ||
      committed._type != ItemTypeFile || committed._fileSize != transferredBytes) {
    return tr("The hydrated file was not committed to the sync journal");
  }
  const auto pin = pinState(request->record.path());
  if (pin && *pin == PinState::OnlineOnly && !setPinState(request->record.path(), PinState::Unspecified)) {
    return tr("The hydrated file's pin state could not be updated");
  }
  return {};
}

Result<void, QString> VfsFuse::restorePlaceholder(FuseHydrationRequest* request) {
  if (!request->record.isValid()) {
    return {};
  }

  auto record = request->record;
  if (!request->restoreOriginalRecord) {
    if (!params().journal->getFileRecord(request->record.path(), &record) || !record.isValid() ||
        !record.isVirtualFile()) {
      return {};
    }
    const auto path = params().filesystemPath + record.path();
    if (FileSystem::fileExists(path)) {
      const auto marker = readPlaceholderMarker(path.toUtf8());
      if (marker && !marker->has_value()) {
        return {};
      }
    }
  }

  const auto path = params().filesystemPath + record.path();
  if (FileSystem::fileExists(path)) {
    const auto item = SyncFileItem::fromSyncJournalFileRecord(record);
    const auto restored = replaceWithPlaceholder(path, *item, TargetExpectation::Existing);
    if (!restored) {
      return restored.error();
    }
  }
  if (request->restoreOriginalRecord) {
    record._type = ItemTypeVirtualFile;
    const auto journalResult = params().journal->setFileRecord(record);
    if (!journalResult) {
      return journalResult.error();
    }
  }
  return {};
}

void VfsFuse::finalizeHydration(
    FuseHydrationRequest* request,
    bool commit,
    qint64 transferredBytes,
    const QString& error) {
  if (request->finalized) {
    return;
  }
  request->finalized = true;
  request->watchdog->stop();
  const auto hadJob = request->job != nullptr;

  auto finalError = error;
  auto result = commit ? commitHydration(request, transferredBytes) : Result<void, QString>{};
  if (!result || !commit) {
    const auto restored = restorePlaceholder(request);
    if (!restored) {
      finalError = finalError.isEmpty() ? restored.error() : QStringLiteral("%1; %2").arg(finalError, restored.error());
    }
  }
  if (!result) {
    finalError = finalError.isEmpty() ? result.error() : QStringLiteral("%1; %2").arg(finalError, result.error());
  }

  const auto committed = commit && result;
  QJsonObject final{
      {QStringLiteral("type"), QStringLiteral("final")},
      {QStringLiteral("ok"), static_cast<bool>(committed)},
      {QStringLiteral("error"), finalError},
  };
  sendHydrationMessage(request, std::move(final));

  auto* socket = request->socket;
  _hydrationRequests.remove(socket);
  QObject::disconnect(socket, nullptr, this, nullptr);
  socket->disconnectFromServer();
  socket->deleteLater();
  if (request->job) {
    request->job->deleteLater();
  }
  request->deleteLater();

  if (hadJob && !isHydrating()) {
    emit doneHydrating();
  }
}

OCC::Result<OCC::Vfs::ConvertToPlaceholderResult, QString>
VfsFuse::updateMetadata(const SyncFileItem& syncItem, const QString& filePath, const QString& replacesFile) {
  Q_UNUSED(replacesFile)

  if (syncItem._modtime <= 0) {
    return {tr("Error updating metadata due to invalid modification time")};
  }

  if (!FileSystem::fileExists(filePath)) {
    return {tr("Cannot update the metadata of %1 because it is no longer present").arg(filePath)};
  }

  if (!_extendedAttributesUsable) {
    return {tr("Extended attributes are unusable below %1").arg(_setupParams.filesystemPath)};
  }

  const auto markerResult = setPlaceholderMarker(filePath, syncItem._size);
  if (!markerResult) {
    return markerResult.error();
  }

  qCDebug(lcVfsFuse) << "setModTime" << filePath << syncItem._modtime;
  if (!FileSystem::setModTime(filePath, syncItem._modtime)) {
    return {tr("Failed to set the modification time of %1").arg(filePath)};
  }
  return {OCC::Vfs::ConvertToPlaceholderResult::Ok};
}

Result<Vfs::ConvertToPlaceholderResult, QString> VfsFuse::updatePlaceholderMarkInSync(
    const QString& filePath,
    const SyncFileItem& syncItem) {
  if (!_extendedAttributesUsable) {
    return {tr("Extended attributes are unusable below %1").arg(_setupParams.filesystemPath)};
  }

  if (syncItem._type == ItemTypeVirtualDirectory) {
    const auto markerResult = setIncompleteDirectoryMarker(filePath);
    if (!markerResult) {
      return markerResult.error();
    }
  } else if (syncItem._type == ItemTypeDirectory) {
    const auto markerResult = removeIncompleteDirectoryMarker(filePath);
    if (!markerResult) {
      return markerResult.error();
    }
  } else if (syncItem._type == ItemTypeVirtualFile || syncItem._type == ItemTypeVirtualFileDownload) {
    const auto markerResult = setPlaceholderMarker(filePath, syncItem._size);
    if (!markerResult) {
      return markerResult.error();
    }
  } else {
    const auto markerResult = removePlaceholderMarker(filePath.toUtf8());
    if (!markerResult) {
      return markerResult.error();
    }
  }

  return {ConvertToPlaceholderResult::Ok};
}

bool VfsFuse::isPlaceHolderInSync(const QString& filePath) const {
  const auto& syncRoot = params().filesystemPath;
  if (!filePath.startsWith(syncRoot)) {
    return true;
  }

  SyncJournalFileRecord record;
  if (!params().journal->getFileRecord(filePath.mid(syncRoot.length()), &record) || !record.isValid()) {
    return true;
  }

  if (record.isDirectory()) {
    const auto incomplete = directoryIsIncomplete(filePath);
    if (!incomplete) {
      qCWarning(lcVfsFuse) << incomplete.error();
      return false;
    }
    return *incomplete == (record._type == ItemTypeVirtualDirectory);
  }

  const auto marker = readPlaceholderMarker(filePath.toUtf8());
  if (!marker) {
    qCWarning(lcVfsFuse) << marker.error();
    return false;
  }
  if (record.isVirtualFile()) {
    return marker->has_value() && marker->value() == record._fileSize;
  }
  return !marker->has_value();
}

Result<void, QString>
VfsFuse::replaceWithPlaceholder(const QString& path, const SyncFileItem& item, TargetExpectation expectation) {
  if (item._modtime <= 0) {
    return {tr("Error updating metadata due to invalid modification time")};
  }

  if (!_extendedAttributesUsable) {
    return tr("Extended attributes are unusable below %1").arg(_setupParams.filesystemPath);
  }

  ReplacementLease lease(params().filesystemPath, path);
  if (!lease) {
    return lease.error();
  }
  if (expectation == TargetExpectation::Existing && !lease.targetExisted()) {
    return tr("Cannot replace %1 because it is no longer present").arg(path);
  }
  if (expectation == TargetExpectation::MissingOrUnchanged && lease.targetExisted()) {
    const auto marker = readPlaceholderMarker(path.toUtf8());
    if (!marker) {
      return marker.error();
    }
    if (!marker->has_value() && !FileSystem::verifyFileUnchanged(path, item._size, item._modtime)) {
      return tr("Cannot create a placeholder because the local file changed");
    }
  }

  PendingPlaceholder pending(path);
  if (!pending) {
    return tr("Failed to create a placeholder beside %1").arg(path);
  }

  const auto markerResult = setPlaceholderMarker(pending.path(), item._size);
  if (!markerResult) {
    return markerResult.error();
  }

  qCDebug(lcVfsFuse) << "setModTime" << path << item._modtime;
  if (!FileSystem::setModTime(pending.path(), item._modtime)) {
    return tr("Failed to set the modification time of %1").arg(path);
  }

  if (lease.targetExisted()) {
    const auto metadataResult = pending.inheritTargetMetadata();
    if (!metadataResult) {
      return metadataResult.error();
    }
  }

  const auto replacementResult = pending.replaceTarget(lease.targetExisted());
  if (!replacementResult) {
    return replacementResult.error();
  }

  return {};
}

Result<void, QString> VfsFuse::createPlaceholder(const SyncFileItem& item) {
  const auto path = QString(_setupParams.filesystemPath + item._file);
  return replaceWithPlaceholder(path, item, TargetExpectation::MissingOrUnchanged);
}

Result<void, QString> VfsFuse::createPlaceholders(const QList<SyncFileItemPtr>& items) {
  auto result = Result<void, QString>{};

  for (const auto& oneItem : items) {
    const auto itemResult = createPlaceholder(*oneItem);
    if (!itemResult) {
      result = itemResult;
      break;
    }
  }

  return result;
}

Result<void, QString> VfsFuse::dehydratePlaceholder(const SyncFileItem& item) {
  if (item._size < 0) {
    return tr("The placeholder size for %1 is negative: %2").arg(item._file).arg(item._size);
  }

  const auto pin = pinState(item._file);
  if (pin && *pin == PinState::AlwaysLocal) {
    return tr("Cannot dehydrate %1 because it is pinned for offline use").arg(item._file);
  }

  const auto path = QString(_setupParams.filesystemPath + item._file);
  const auto placeholderResult = replaceWithPlaceholder(path, item, TargetExpectation::Existing);
  if (!placeholderResult) {
    return placeholderResult;
  }

  return {};
}

Result<Vfs::ConvertToPlaceholderResult, QString>
VfsFuse::convertToPlaceholder(const QString& filename, const SyncFileItem& item, const QString&, UpdateMetadataTypes) {
  if (item._type == ItemTypeVirtualDirectory) {
    const auto marked = setIncompleteDirectoryMarker(filename);
    if (!marked) {
      return marked.error();
    }
  } else if (item._type == ItemTypeDirectory) {
    const auto completed = removeIncompleteDirectoryMarker(filename);
    if (!completed) {
      return completed.error();
    }
  }
  return {ConvertToPlaceholderResult::Ok};
}

bool VfsFuse::needsMetadataUpdate(const SyncFileItem&) { return false; }

bool VfsFuse::isDehydratedPlaceholder(const QString& filePath) {
  if (!FileSystem::fileExists(filePath)) {
    return false;
  }
  const auto marker = readPlaceholderMarker(filePath.toUtf8());
  if (!marker) {
    qCWarning(lcVfsFuse) << marker.error();
    return true;
  }
  return marker->has_value();
}

bool VfsFuse::statTypeVirtualFile(csync_file_stat_t* stat, void* statData) {
  if (stat->type == ItemTypeDirectory) {
    return false;
  }

  const auto parentPath = static_cast<QByteArray*>(statData);
  Q_ASSERT(!parentPath->endsWith('/'));
  Q_ASSERT(!stat->path.startsWith('/'));

  const auto path = QByteArray(*parentPath + '/' + stat->path);
  const auto absolutePath = QString::fromUtf8(path);
  Q_ASSERT(absolutePath.startsWith(params().filesystemPath));

  const auto relativePath = absolutePath.mid(params().filesystemPath.length());
  const auto pin = pinState(relativePath);
  const auto marker = readPlaceholderMarker(path);
  const auto markerReadFailed = !marker;
  if (markerReadFailed) {
    qCWarning(lcVfsFuse) << marker.error();
  }

  SyncJournalFileRecord record;
  const auto hasVirtualRecord =
      params().journal->getFileRecord(relativePath, &record) && record.isValid() && record.isVirtualFile();
  const auto recoverableMissingMarker = marker && !marker->has_value() && hasVirtualRecord && stat->size == 0;
  const auto markerNeedsRepair = hasVirtualRecord && (markerReadFailed || recoverableMissingMarker ||
                                                      (marker->has_value() && marker->value() != record._fileSize));

  if (markerNeedsRepair) {
    const auto repairResult = setPlaceholderMarker(absolutePath, record._fileSize);
    if (!repairResult) {
      qCCritical(lcVfsFuse) << repairResult.error();
    }
  }

  if ((marker && marker->has_value()) || recoverableMissingMarker || (markerReadFailed && hasVirtualRecord)) {
    const auto shouldDownload = pin && (*pin == PinState::AlwaysLocal);
    stat->type = shouldDownload ? ItemTypeVirtualFileDownload : ItemTypeVirtualFile;
    return true;
  }

  const auto shouldDehydrate = pin && (*pin == PinState::OnlineOnly);
  if (shouldDehydrate) {
    stat->type = ItemTypeVirtualFileDehydration;
    return true;
  }

  return false;
}

bool VfsFuse::setPinState(const QString& folderPath, PinState state) {
  qCDebug(lcVfsFuse) << "setPinState" << folderPath << state;
  if (!setPinStateInDb(folderPath, state)) {
    return false;
  }
  if (_demandDrivenDiscoveryEnabled && state == PinState::AlwaysLocal) {
    params().journal->markVirtualFileForDownloadRecursively(folderPath.toUtf8());
  }
  return true;
}

Optional<PinState> VfsFuse::pinState(const QString& folderPath) { return pinStateInDb(folderPath); }

Vfs::AvailabilityResult VfsFuse::availability(const QString& folderPath, const AvailabilityRecursivity recursiveCheck) {
  Q_UNUSED(recursiveCheck)
  return availabilityInDb(folderPath);
}

void VfsFuse::fileStatusChanged(const QString&, SyncFileStatus) {}

}  // namespace OCC
