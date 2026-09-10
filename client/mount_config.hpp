#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QList>
#include <QStandardPaths>
#include <QString>
#include <optional>

namespace ncvfs {

struct MountMapping {
  QString store;
  QString mount;
};

inline QString normalized_directory(const QString& path) {
  const QFileInfo information(path);
  const auto canonical_path = information.canonicalFilePath();
  return QDir::cleanPath(canonical_path.isEmpty() ? information.absoluteFilePath() : canonical_path);
}

inline QString default_mount_config_path() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
         QStringLiteral("/nextcloud-vfs-fuse/mounts.json");
}

inline std::optional<QList<MountMapping>> load_mount_mappings(const QString& path, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("the mount configuration %1 cannot be opened: %2").arg(path, file.errorString());
    return std::nullopt;
  }

  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    error = QStringLiteral("the mount configuration %1 does not contain a JSON object").arg(path);
    return std::nullopt;
  }

  QList<MountMapping> mappings;
  const auto object = document.object();
  for (auto entry = object.constBegin(); entry != object.constEnd(); ++entry) {
    if (!entry.value().isString() || !QFileInfo(entry.key()).isAbsolute() ||
        !QFileInfo(entry.value().toString()).isAbsolute()) {
      error = QStringLiteral("the mount configuration %1 contains a non-absolute store or mount path").arg(path);
      return std::nullopt;
    }
    mappings.push_back({normalized_directory(entry.key()), normalized_directory(entry.value().toString())});
  }
  return mappings;
}

}  // namespace ncvfs
