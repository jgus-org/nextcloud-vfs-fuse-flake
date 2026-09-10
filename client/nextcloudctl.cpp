#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTextStream>
#include <algorithm>
#include <optional>

#include "../libsync/vfs/fuse/mount_config.hpp"
#include "../libsync/vfs/fuse/protocol.hpp"

namespace {

struct Target {
  QString input;
  QString store;
  QString relativePath;
};

struct Response {
  QJsonObject object;
  QString error;

  explicit operator bool() const { return error.isEmpty(); }
};

QString absolutePath(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }

bool containsPath(const QString& root, const QString& path) {
  return path == root || path.startsWith(root == QLatin1String("/") ? root : root + QLatin1Char('/'));
}

QString relativePath(const QString& root, const QString& path) {
  if (path == root) {
    return {};
  }
  return path.mid(root.size() + (root == QLatin1String("/") ? 0 : 1));
}

std::optional<Target> resolveTarget(
    const QString& input,
    const QString& explicitStore,
    const QList<ncvfs::MountMapping>& mappings,
    QString& error) {
  if (!explicitStore.isEmpty()) {
    const auto store = ncvfs::normalized_directory(explicitStore);
    if (!QFileInfo(explicitStore).isAbsolute()) {
      error = QStringLiteral("the store path is not absolute");
      return std::nullopt;
    }
    if (!QFileInfo(input).isAbsolute()) {
      const auto relative = QDir::cleanPath(input);
      if (relative == QLatin1String(".") || relative == QLatin1String("..") ||
          relative.startsWith(QLatin1String("../"))) {
        error = QStringLiteral("the relative control path is invalid");
        return std::nullopt;
      }
      return Target{input, store, relative};
    }

    const auto target = absolutePath(input);
    if (containsPath(store, target)) {
      return Target{input, store, relativePath(store, target)};
    }
    for (const auto& mapping : mappings) {
      if (mapping.store == store && containsPath(mapping.mount, target)) {
        return Target{input, store, relativePath(mapping.mount, target)};
      }
    }
    error = QStringLiteral("the path is outside the selected store and its configured mount");
    return std::nullopt;
  }

  const auto target = absolutePath(input);
  for (const auto& mapping : mappings) {
    if (containsPath(mapping.mount, target)) {
      return Target{input, mapping.store, relativePath(mapping.mount, target)};
    }
  }
  error = QStringLiteral("the path is outside every configured Nextcloud mount");
  return std::nullopt;
}

Response request(const Target& target, QJsonObject message) {
  QLocalSocket socket;
  socket.connectToServer(ncvfs::protocol::control_server_name(target.store));
  if (!socket.waitForConnected(ncvfs::protocol::connection_timeout_ms)) {
    return {{}, socket.errorString()};
  }

  message.insert(QStringLiteral("path"), target.relativePath);
  const auto encoded = ncvfs::protocol::encode(std::move(message));
  if (socket.write(encoded) != encoded.size() || !socket.flush()) {
    return {{}, socket.errorString()};
  }
  while (socket.bytesToWrite() > 0) {
    if (!socket.waitForBytesWritten(ncvfs::protocol::connection_timeout_ms)) {
      return {{}, socket.errorString()};
    }
  }

  QByteArray input;
  for (;;) {
    auto decoded = ncvfs::protocol::take_message(input);
    if (decoded.state == ncvfs::protocol::DecodedMessage::State::Valid) {
      if (!decoded.object.value(QStringLiteral("ok")).toBool()) {
        return {
            decoded.object,
            decoded.object.value(QStringLiteral("error")).toString(QStringLiteral("request failed"))};
      }
      return {decoded.object, {}};
    }
    if (decoded.state == ncvfs::protocol::DecodedMessage::State::Invalid) {
      return {{}, decoded.error};
    }
    if (socket.bytesAvailable() > 0) {
      input += socket.readAll();
      continue;
    }
    if (!socket.waitForReadyRead(ncvfs::protocol::connection_timeout_ms)) {
      if (socket.bytesAvailable() > 0) {
        input += socket.readAll();
        continue;
      }
      return {{}, socket.errorString()};
    }
    input += socket.readAll();
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("nextcloudctl"));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("Control Nextcloud virtual-file availability"));
  parser.addHelpOption();
  parser.addOption(
      {QStringList{QStringLiteral("s"), QStringLiteral("store")},
       QStringLiteral("Select a backing store explicitly"),
       QStringLiteral("path")});
  parser.addOption(
      {QStringList{QStringLiteral("c"), QStringLiteral("config")},
       QStringLiteral("Read mount mappings from this file"),
       QStringLiteral("path"),
       ncvfs::default_mount_config_path()});
  parser.addOption({QStringLiteral("json"), QStringLiteral("Write structured JSON output")});
  parser.addPositionalArgument(
      QStringLiteral("command"),
      QStringLiteral("status, keep-local, free-space, automatic, or inherit"));
  parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("One or more paths"), QStringLiteral("PATH..."));
  parser.process(application);

  const auto arguments = parser.positionalArguments();
  if (arguments.size() < 2) {
    parser.showHelp(2);
  }
  const auto command = arguments.first();
  const auto paths = arguments.sliced(1);

  QJsonObject requestTemplate;
  if (command == QLatin1String("status")) {
    requestTemplate.insert(QStringLiteral("type"), QStringLiteral("vfs-status"));
  } else {
    QString state;
    if (command == QLatin1String("keep-local")) {
      state = QStringLiteral("always-local");
    } else if (command == QLatin1String("free-space")) {
      state = QStringLiteral("online-only");
    } else if (command == QLatin1String("automatic")) {
      state = QStringLiteral("automatic");
    } else if (command == QLatin1String("inherit")) {
      state = QStringLiteral("inherit");
    } else {
      QTextStream(stderr) << "unknown command: " << command << '\n';
      return 2;
    }
    requestTemplate.insert(QStringLiteral("type"), QStringLiteral("vfs-set-pin-state"));
    requestTemplate.insert(QStringLiteral("state"), state);
  }

  QString mappingError;
  auto mappings = ncvfs::load_mount_mappings(parser.value(QStringLiteral("config")), mappingError);
  if (!mappings && parser.isSet(QStringLiteral("store")) && !parser.isSet(QStringLiteral("config")) &&
      !QFileInfo::exists(ncvfs::default_mount_config_path())) {
    mappings = QList<ncvfs::MountMapping>{};
  }
  if (!mappings) {
    QTextStream(stderr) << mappingError << '\n';
    return 2;
  }
  if (mappings->isEmpty() && !parser.isSet(QStringLiteral("store"))) {
    QTextStream(stderr) << "the mount configuration is empty and no store was selected\n";
    return 2;
  }

  std::sort(mappings->begin(), mappings->end(), [](const auto& left, const auto& right) {
    return left.mount.size() > right.mount.size();
  });

  QJsonArray results;
  auto failed = false;
  for (const auto& path : paths) {
    QString targetError;
    const auto target = resolveTarget(path, parser.value(QStringLiteral("store")), *mappings, targetError);
    if (!target) {
      QTextStream(stderr) << path << ": " << targetError << '\n';
      failed = true;
      continue;
    }

    auto result = request(*target, requestTemplate);
    if (!result) {
      QTextStream(stderr) << path << ": " << result.error << '\n';
      failed = true;
      continue;
    }
    result.object.insert(QStringLiteral("inputPath"), path);
    results.append(result.object);

    if (!parser.isSet(QStringLiteral("json")) && command == QLatin1String("status")) {
      QTextStream(stdout) << path << '\t' << result.object.value(QStringLiteral("policy")).toString() << '\t'
                          << result.object.value(QStringLiteral("effectivePolicy")).toString() << '\t'
                          << result.object.value(QStringLiteral("availability")).toString() << '\n';
    }
  }

  if (parser.isSet(QStringLiteral("json"))) {
    QTextStream(stdout) << QJsonDocument(results).toJson(QJsonDocument::Compact) << '\n';
  }
  return failed ? 1 : 0;
}
