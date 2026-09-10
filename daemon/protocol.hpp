#pragma once

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

namespace ncvfs::protocol {

inline constexpr int version = 1;
inline constexpr int connection_timeout_ms = 5000;
inline constexpr int watchdog_timeout_ms = 30000;
inline constexpr int heartbeat_interval_ms = 2000;

inline QString control_server_name(const QString& store) {
  auto identity = QFileInfo(store).canonicalFilePath();
  if (identity.isEmpty()) {
    identity = QFileInfo(store).absoluteFilePath();
  }
  const auto digest = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex();
  return QStringLiteral("nextcloud-vfs-fuse-") + QString::fromLatin1(digest);
}

inline QByteArray encode(QJsonObject message) {
  message.insert(QStringLiteral("version"), version);
  return QJsonDocument(std::move(message)).toJson(QJsonDocument::Compact) + '\n';
}

struct DecodedMessage {
  enum class State {
    Incomplete,
    Valid,
    Invalid,
  };

  State state = State::Incomplete;
  QJsonObject object;
  QString error;
};

inline DecodedMessage take_message(QByteArray& buffer) {
  const auto delimiter = buffer.indexOf('\n');
  if (delimiter < 0) {
    return {};
  }

  const auto encoded = buffer.left(delimiter);
  buffer.remove(0, delimiter + 1);
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(encoded, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return {
        .state = DecodedMessage::State::Invalid,
        .object = {},
        .error = QStringLiteral("invalid control protocol message: %1").arg(parse_error.errorString()),
    };
  }

  auto object = document.object();
  if (object.value(QStringLiteral("version")).toInt(-1) != version) {
    return {
        .state = DecodedMessage::State::Invalid,
        .object = {},
        .error = QStringLiteral("unsupported control protocol version"),
    };
  }
  return {
      .state = DecodedMessage::State::Valid,
      .object = std::move(object),
      .error = {},
  };
}

inline QString encoded_integer(qint64 value) { return QString::number(value); }

inline bool decode_integer(const QJsonValue& value, qint64& result) {
  bool valid = false;
  const auto decoded = value.toString().toLongLong(&valid);
  if (valid) {
    result = decoded;
  }
  return valid;
}

}  // namespace ncvfs::protocol
