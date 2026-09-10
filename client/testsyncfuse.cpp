/*
 * SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: CC0-1.0
 */

#include <fcntl.h>
#include <syncengine.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <thread>
#include <utility>

#include "common/vfs.h"
#include "syncenginetestutils.h"
#include "vfs/fuse/mount_config.hpp"
#include "vfs/fuse/protocol.hpp"
#include "vfs/fuse/replacement_lock.hpp"
#include "vfs/fuse/unique_fd.hpp"

using namespace OCC;

namespace {

constexpr auto placeholderMarkerName = "user.nextcloud.vfs_fuse";
constexpr auto incompleteDirectoryMarkerName = "user.nextcloud.vfs_fuse.incomplete";
constexpr auto preservedAttributeName = "user.nextcloud.vfs_fuse.test";
constexpr qint64 noPlaceholderMarkerSize = -1;

bool hasFusePlaceholderMarker(const QString& path) {
  const auto localPath = path.toUtf8();
  return getxattr(localPath.constData(), placeholderMarkerName, nullptr, 0) >= 0;
}

bool hasIncompleteDirectoryMarker(const QString& path) {
  const auto localPath = path.toUtf8();
  return getxattr(localPath.constData(), incompleteDirectoryMarkerName, nullptr, 0) >= 0;
}

ino_t fileInode(const QString& path) {
  struct stat attributes = {};
  return stat(path.toUtf8().constData(), &attributes) == 0 ? attributes.st_ino : 0;
}

qint64 fusePlaceholderMarkerSize(const QString& path) {
  constexpr auto bufferSize = 256;
  const auto localPath = path.toUtf8();
  QByteArray value(bufferSize, '\0');
  const auto count = getxattr(localPath.constData(), placeholderMarkerName, value.data(), bufferSize);
  if (count < 0) {
    return noPlaceholderMarkerSize;
  }
  value.resize(static_cast<qsizetype>(count));

  auto parsed = false;
  const auto size = value.toLongLong(&parsed);
  return parsed ? size : noPlaceholderMarkerSize;
}

QByteArray extendedAttribute(const QString& path, const char* name) {
  const auto localPath = path.toUtf8();
  const auto size = getxattr(localPath.constData(), name, nullptr, 0);
  if (size < 0) {
    return {};
  }
  QByteArray value(static_cast<qsizetype>(size), '\0');
  const auto read = getxattr(localPath.constData(), name, value.data(), static_cast<size_t>(value.size()));
  if (read < 0) {
    return {};
  }
  value.resize(static_cast<qsizetype>(read));
  return value;
}

bool sendProtocolMessage(QLocalSocket& socket, QJsonObject message, QString& error) {
  const auto encoded = ncvfs::protocol::encode(std::move(message));
  if (socket.write(encoded) != encoded.size() || !socket.flush()) {
    error = socket.errorString();
    return false;
  }
  while (socket.bytesToWrite() > 0) {
    if (!socket.waitForBytesWritten(ncvfs::protocol::connection_timeout_ms)) {
      error = socket.errorString();
      return false;
    }
  }
  return true;
}

QJsonObject waitForProtocolMessage(QLocalSocket& socket, QByteArray& input, QString& error) {
  for (;;) {
    auto decoded = ncvfs::protocol::take_message(input);
    if (decoded.state == ncvfs::protocol::DecodedMessage::State::Valid) {
      return decoded.object;
    }
    if (decoded.state == ncvfs::protocol::DecodedMessage::State::Invalid) {
      error = decoded.error;
      return {};
    }
    if (socket.bytesAvailable() > 0) {
      input += socket.readAll();
      continue;
    }
    if (!socket.waitForReadyRead(ncvfs::protocol::watchdog_timeout_ms)) {
      error = socket.errorString();
      return {};
    }
    input += socket.readAll();
  }
}

struct HydrationPeerResult {
  bool committed = false;
  QByteArray bytes;
  QString error;
};

struct ControlPeerResult {
  QJsonObject response;
  QString error;
};

ControlPeerResult runControlPeer(const QString& store, QJsonObject request) {
  ControlPeerResult result;
  QLocalSocket control;
  control.connectToServer(ncvfs::protocol::control_server_name(store));
  if (!control.waitForConnected(ncvfs::protocol::connection_timeout_ms)) {
    result.error = control.errorString();
    return result;
  }
  if (!sendProtocolMessage(control, std::move(request), result.error)) {
    return result;
  }
  QByteArray input;
  result.response = waitForProtocolMessage(control, input, result.error);
  return result;
}

HydrationPeerResult runHydrationPeer(
    const QString& store,
    const QString& folderPath,
    qint64 reportedByteAdjustment = 0,
    std::function<void()> beforeCommit = {}) {
  HydrationPeerResult result;
  QLocalSocket control;
  control.connectToServer(ncvfs::protocol::control_server_name(store));
  if (!control.waitForConnected(ncvfs::protocol::connection_timeout_ms)) {
    result.error = control.errorString();
    return result;
  }
  if (!sendProtocolMessage(
          control,
          {
              {QStringLiteral("type"), QStringLiteral("hydrate")},
              {QStringLiteral("path"), folderPath},
          },
          result.error)) {
    return result;
  }

  QByteArray controlInput;
  const auto ready = waitForProtocolMessage(control, controlInput, result.error);
  if (ready.value(QStringLiteral("type")).toString() != QLatin1String("ready")) {
    if (result.error.isEmpty()) {
      result.error = QStringLiteral("missing ready response");
    }
    return result;
  }

  QLocalSocket cancellation;
  cancellation.connectToServer(ready.value(QStringLiteral("requestId")).toString() + QStringLiteral(":cancellation"));
  if (!cancellation.waitForConnected(ncvfs::protocol::connection_timeout_ms)) {
    result.error = cancellation.errorString();
    return result;
  }
  QLocalSocket data;
  data.connectToServer(ready.value(QStringLiteral("requestId")).toString());
  if (!data.waitForConnected(ncvfs::protocol::connection_timeout_ms)) {
    result.error = data.errorString();
    return result;
  }

  QFile target(store + folderPath);
  if (!target.open(QFile::WriteOnly | QFile::Truncate)) {
    result.error = target.errorString();
    return result;
  }
  while (data.state() != QLocalSocket::UnconnectedState || data.bytesAvailable() > 0) {
    data.waitForReadyRead(1000);
    const auto bytes = data.readAll();
    if (!bytes.isEmpty()) {
      if (target.write(bytes) != bytes.size()) {
        result.error = target.errorString();
        return result;
      }
      result.bytes += bytes;
    }
  }
  target.flush();

  const auto complete = waitForProtocolMessage(control, controlInput, result.error);
  if (complete.value(QStringLiteral("type")).toString() != QLatin1String("complete") ||
      complete.value(QStringLiteral("status")).toInt(-1) != 0) {
    if (result.error.isEmpty()) {
      result.error = complete.value(QStringLiteral("error")).toString(QStringLiteral("download failed"));
    }
    return result;
  }

  if (beforeCommit) {
    beforeCommit();
  }

  if (!sendProtocolMessage(
          control,
          {
              {QStringLiteral("type"), QStringLiteral("commit")},
              {QStringLiteral("bytes"), ncvfs::protocol::encoded_integer(result.bytes.size() + reportedByteAdjustment)},
          },
          result.error)) {
    return result;
  }
  const auto final = waitForProtocolMessage(control, controlInput, result.error);
  if (final.value(QStringLiteral("type")).toString() != QLatin1String("final")) {
    if (result.error.isEmpty()) {
      result.error = QStringLiteral("missing final response");
    }
    return result;
  }
  result.committed = final.value(QStringLiteral("ok")).toBool();
  if (!result.committed && result.error.isEmpty()) {
    result.error = final.value(QStringLiteral("error")).toString();
  }
  return result;
}

QString disconnectHydrationPeer(const QString& store, const QString& folderPath) {
  QLocalSocket control;
  control.connectToServer(ncvfs::protocol::control_server_name(store));
  if (!control.waitForConnected(ncvfs::protocol::connection_timeout_ms)) {
    return control.errorString();
  }
  QString error;
  if (!sendProtocolMessage(
          control,
          {
              {QStringLiteral("type"), QStringLiteral("hydrate")},
              {QStringLiteral("path"), folderPath},
          },
          error)) {
    return error;
  }
  QByteArray input;
  const auto ready = waitForProtocolMessage(control, input, error);
  if (ready.value(QStringLiteral("type")).toString() != QLatin1String("ready")) {
    return error.isEmpty() ? QStringLiteral("missing ready response") : error;
  }
  control.abort();
  return {};
}

}  // namespace

#define FUSEVERIFY_VIRTUAL(folder, path)                                                                    \
  QVERIFY(QFileInfo((folder).localPath() + (path)).exists());                                               \
  QVERIFY(dbRecord((folder), (path)).isValid());                                                            \
  QCOMPARE(QFileInfo((folder).localPath() + (path)).size(), 0);                                             \
  QVERIFY(hasFusePlaceholderMarker((folder).localPath() + (path)));                                         \
  QCOMPARE(fusePlaceholderMarkerSize((folder).localPath() + (path)), dbRecord((folder), (path))._fileSize); \
  QCOMPARE(dbRecord((folder), (path))._type, ItemTypeVirtualFile);

#define FUSEVERIFY_NONVIRTUAL(folder, path)                          \
  QVERIFY(QFileInfo((folder).localPath() + (path)).exists());        \
  QVERIFY(!hasFusePlaceholderMarker((folder).localPath() + (path))); \
  QVERIFY(dbRecord((folder), (path)).isValid());                     \
  QCOMPARE(dbRecord((folder), (path))._type, ItemTypeFile);

#define FUSEVERIFY_GONE(folder, path)                          \
  QVERIFY(!QFileInfo((folder).localPath() + (path)).exists()); \
  QVERIFY(!dbRecord((folder), (path)).isValid());

bool itemInstruction(const ItemCompletedSpy& spy, const QString& path, const SyncInstructions instr) {
  auto item = spy.findItem(path);
  return item->_instruction == instr;
}

SyncJournalFileRecord dbRecord(FakeFolder& folder, const QString& path) {
  SyncJournalFileRecord record;
  [[maybe_unused]] const auto result = folder.syncJournal().getFileRecord(path, &record);
  return record;
}

void triggerDownload(FakeFolder& folder, const QByteArray& path) {
  auto& journal = folder.syncJournal();
  SyncJournalFileRecord record;
  if (!journal.getFileRecord(path, &record) || !record.isValid()) {
    return;
  }
  record._type = ItemTypeVirtualFileDownload;
  QVERIFY(journal.setFileRecord(record));
  journal.schedulePathForRemoteDiscovery(record._path);
}

void markForDehydration(FakeFolder& folder, const QByteArray& path) {
  auto& journal = folder.syncJournal();
  SyncJournalFileRecord record;
  if (!journal.getFileRecord(path, &record) || !record.isValid()) {
    return;
  }
  record._type = ItemTypeVirtualFileDehydration;
  QVERIFY(journal.setFileRecord(record));
  journal.schedulePathForRemoteDiscovery(record._path);
}

QSharedPointer<Vfs> setupVfs(FakeFolder& folder, bool demandDrivenDiscovery = false) {
  auto fuseVfs = QSharedPointer<Vfs>(createVfsFromPlugin(Vfs::Fuse).release());
  if (!fuseVfs) {
    return fuseVfs;
  }

  fuseVfs->setDemandDrivenDiscoveryEnabled(demandDrivenDiscovery);

  QObject::connect(
      &folder.syncEngine().syncFileStatusTracker(),
      &SyncFileStatusTracker::fileStatusChanged,
      fuseVfs.data(),
      &Vfs::fileStatusChanged);
  folder.switchToVfs(fuseVfs);

  folder.syncJournal().internalPinStates().setForPath(QByteArray(), PinState::Unspecified);

  return fuseVfs;
}

class TestSyncFuse : public QObject {
  Q_OBJECT

 private slots:
  void testUserFacingPath() {
    FakeFolder fakeFolder{FileInfo()};
    QTemporaryDir mountPoint;
    const auto mappings = QJsonObject{{QDir::cleanPath(fakeFolder.localPath()), mountPoint.path()}};
    const auto configPath = ncvfs::default_mount_config_path();
    QVERIFY(QDir().mkpath(QFileInfo(configPath).path()));
    QFile configFile(configPath);
    QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto encodedMappings = QJsonDocument(mappings).toJson(QJsonDocument::Compact);
    QCOMPARE(configFile.write(encodedMappings), encodedMappings.size());
    configFile.close();

    const auto vfs = setupVfs(fakeFolder);
    const auto userFacingPath = vfs ? vfs->userFacingPath() : QString{};
    QVERIFY(configFile.remove());

    QVERIFY(vfs);
    QCOMPARE(QDir::cleanPath(userFacingPath), QDir::cleanPath(mountPoint.path()));
  }

  void testControlProtocol() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);
    QSignalSpy syncRequests(vfs.data(), &Vfs::syncRequested);

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");

    const auto control = [&](QJsonObject request) {
      ControlPeerResult result;
      QEventLoop loop;
      std::thread peer([&] {
        result = runControlPeer(fakeFolder.localPath(), std::move(request));
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
      });
      loop.exec();
      peer.join();
      return result;
    };

    auto status = control({
        {QStringLiteral("type"), QStringLiteral("vfs-status")},
        {QStringLiteral("path"), QStringLiteral("A/a1")},
    });
    QVERIFY2(status.error.isEmpty(), qPrintable(status.error));
    QVERIFY(status.response.value(QStringLiteral("ok")).toBool());
    QCOMPARE(status.response.value(QStringLiteral("policy")).toString(), QStringLiteral("inherit"));
    QCOMPARE(status.response.value(QStringLiteral("effectivePolicy")).toString(), QStringLiteral("automatic"));
    QCOMPARE(status.response.value(QStringLiteral("availability")).toString(), QStringLiteral("dehydrated"));

    auto keepLocal = control({
        {QStringLiteral("type"), QStringLiteral("vfs-set-pin-state")},
        {QStringLiteral("path"), QStringLiteral("A/a1")},
        {QStringLiteral("state"), QStringLiteral("always-local")},
    });
    QVERIFY2(keepLocal.error.isEmpty(), qPrintable(keepLocal.error));
    QVERIFY(keepLocal.response.value(QStringLiteral("ok")).toBool());
    QCOMPARE(syncRequests.count(), 1);
    QCOMPARE(syncRequests.first().first().toString(), QStringLiteral("A/a1"));
    QCOMPARE(*vfs->pinState(QStringLiteral("A/a1")), PinState::AlwaysLocal);

    auto freeSpace = control({
        {QStringLiteral("type"), QStringLiteral("vfs-set-pin-state")},
        {QStringLiteral("path"), QStringLiteral("A/a1")},
        {QStringLiteral("state"), QStringLiteral("online-only")},
    });
    QVERIFY2(freeSpace.error.isEmpty(), qPrintable(freeSpace.error));
    QVERIFY(freeSpace.response.value(QStringLiteral("ok")).toBool());
    QCOMPARE(syncRequests.count(), 2);
    QCOMPARE(*vfs->pinState(QStringLiteral("A/a1")), PinState::OnlineOnly);
  }

  void testDemandDrivenDirectoryDiscovery() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder, true);
    QVERIFY(vfs);
    QSignalSpy syncRequests(vfs.data(), &Vfs::syncRequested);

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().mkdir("A/B");
    fakeFolder.remoteModifier().mkdir("A/B/C");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    fakeFolder.remoteModifier().insert("A/B/b1", 64);
    fakeFolder.remoteModifier().insert("A/B/C/c1", 64);
    QVERIFY(fakeFolder.syncOnce());

    QCOMPARE(dbRecord(fakeFolder, "A")._type, ItemTypeVirtualDirectory);
    QVERIFY(hasIncompleteDirectoryMarker(fakeFolder.localPath() + "A"));
    QVERIFY(!QFileInfo::exists(fakeFolder.localPath() + "A/B"));
    QVERIFY(!dbRecord(fakeFolder, "A/B").isValid());

    fakeFolder.remoteModifier().insert("A/B/b2", 64);
    QVERIFY(fakeFolder.syncOnce());
    QCOMPARE(dbRecord(fakeFolder, "A")._type, ItemTypeVirtualDirectory);
    QVERIFY(hasIncompleteDirectoryMarker(fakeFolder.localPath() + "A"));
    QVERIFY(!QFileInfo::exists(fakeFolder.localPath() + "A/B"));

    ControlPeerResult discovery;
    QEventLoop loop;
    std::thread peer([&] {
      discovery = runControlPeer(
          fakeFolder.localPath(),
          {
              {QStringLiteral("type"), QStringLiteral("discover-directory")},
              {QStringLiteral("path"), QStringLiteral("A")},
          });
      QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    });
    loop.exec();
    peer.join();

    QVERIFY2(discovery.error.isEmpty(), qPrintable(discovery.error));
    QVERIFY(discovery.response.value(QStringLiteral("ok")).toBool());
    QCOMPARE(discovery.response.value(QStringLiteral("type")).toString(), QStringLiteral("discovery-ready"));
    QCOMPARE(syncRequests.count(), 1);
    QCOMPARE(syncRequests.first().first().toString(), QStringLiteral("A"));

    QVERIFY(fakeFolder.syncOnce());
    QCOMPARE(dbRecord(fakeFolder, "A")._type, ItemTypeDirectory);
    QVERIFY(!hasIncompleteDirectoryMarker(fakeFolder.localPath() + "A"));
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/B")._type, ItemTypeVirtualDirectory);
    QVERIFY(hasIncompleteDirectoryMarker(fakeFolder.localPath() + "A/B"));
    QVERIFY(!QFileInfo::exists(fakeFolder.localPath() + "A/B/C"));
    QVERIFY(!dbRecord(fakeFolder, "A/B/b1").isValid());
    QVERIFY(!dbRecord(fakeFolder, "A/B/b2").isValid());
  }

  void testAlwaysLocalExpandsDemandDrivenDirectories() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder, true);
    QVERIFY(vfs);

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().mkdir("A/B");
    fakeFolder.remoteModifier().insert("A/B/file", 64);
    QVERIFY(fakeFolder.syncOnce());
    QCOMPARE(dbRecord(fakeFolder, "A")._type, ItemTypeVirtualDirectory);

    QVERIFY(vfs->setPinState(QString(), PinState::AlwaysLocal));
    QVERIFY(fakeFolder.syncOnce());
    QCOMPARE(dbRecord(fakeFolder, "A")._type, ItemTypeDirectory);
    QCOMPARE(dbRecord(fakeFolder, "A/B")._type, ItemTypeDirectory);
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "A/B/file");
  }

  void testVirtualFileLifecycle_data() {
    QTest::addColumn<bool>("doLocalDiscovery");

    QTest::newRow("full local discovery") << true;
    QTest::newRow("skip local discovery") << false;
  }

  void testVirtualFileLifecycle() {
    QFETCH(bool, doLocalDiscovery);

    FakeFolder fakeFolder{FileInfo()};
    QVERIFY(setupVfs(fakeFolder));
    QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    ItemCompletedSpy completeSpy(fakeFolder);

    auto cleanup = [&]() {
      completeSpy.clear();
      if (!doLocalDiscovery)
        fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem);
    };
    cleanup();

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    auto someDate = QDateTime(QDate(1984, 07, 30), QTime(1, 3, 2));
    fakeFolder.remoteModifier().setModTime("A/a1", someDate);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 64);
    QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
    QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
    QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_NEW));
    cleanup();

    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 64);
    QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
    QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
    QVERIFY(completeSpy.isEmpty());
    cleanup();

    fakeFolder.syncJournal().forceRemoteDiscoveryNextSync();
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 64);
    QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
    QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
    QVERIFY(completeSpy.isEmpty());
    cleanup();

    QCOMPARE(fusePlaceholderMarkerSize(fakeFolder.localPath() + "A/a1"), 64);
    fakeFolder.remoteModifier().appendByte("A/a1");
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 65);
    QCOMPARE(fusePlaceholderMarkerSize(fakeFolder.localPath() + "A/a1"), 65);
    QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
    QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
    QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_UPDATE_METADATA));
    cleanup();

    if (!doLocalDiscovery)
      fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem, {"A"});
    fakeFolder.localModifier().remove("A/a1");
    QVERIFY(fakeFolder.syncOnce());
    QVERIFY(!fakeFolder.currentLocalState().find("A/a1"));
    QVERIFY(!fakeFolder.currentRemoteState().find("A/a1"));
    QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_REMOVE));
    QVERIFY(!dbRecord(fakeFolder, "A/a1").isValid());
    cleanup();

    fakeFolder.remoteModifier().insert("A/a1", 65);
    fakeFolder.remoteModifier().setModTime("A/a1", someDate);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 65);
    QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").lastModified(), someDate);
    QVERIFY(fakeFolder.currentRemoteState().find("A/a1"));
    QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_NEW));
    cleanup();

    fakeFolder.remoteModifier().rename("A/a1", "A/a1m");
    QVERIFY(fakeFolder.syncOnce());
    QVERIFY(!QFileInfo(fakeFolder.localPath() + "A/a1").exists());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1m");
    QCOMPARE(dbRecord(fakeFolder, "A/a1m")._fileSize, 65);
    QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1m").lastModified(), someDate);
    QVERIFY(!fakeFolder.currentRemoteState().find("A/a1"));
    QVERIFY(fakeFolder.currentRemoteState().find("A/a1m"));
    QVERIFY(
        itemInstruction(completeSpy, "A/a1m", CSYNC_INSTRUCTION_RENAME) ||
        (itemInstruction(completeSpy, "A/a1m", CSYNC_INSTRUCTION_NEW) &&
         itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_REMOVE)));
    QVERIFY(!dbRecord(fakeFolder, "A/a1").isValid());
    cleanup();

    fakeFolder.remoteModifier().remove("A/a1m");
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_GONE(fakeFolder, "A/a1m");
    QVERIFY(!fakeFolder.currentRemoteState().find("A/a1m"));
    QVERIFY(itemInstruction(completeSpy, "A/a1m", CSYNC_INSTRUCTION_REMOVE));
    QVERIFY(!dbRecord(fakeFolder, "A/a1").isValid());
    cleanup();
  }

  void testHydrateAfterRemoteChange() {
    FakeFolder fakeFolder{FileInfo()};
    QVERIFY(setupVfs(fakeFolder));
    QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    ItemCompletedSpy completeSpy(fakeFolder);

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    fakeFolder.remoteModifier().insert("A/a2", 64);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a2");
    completeSpy.clear();

    triggerDownload(fakeFolder, "A/a1");
    triggerDownload(fakeFolder, "A/a2");
    fakeFolder.remoteModifier().appendByte("A/a2");
    QVERIFY(fakeFolder.syncOnce());

    QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_SYNC));
    QCOMPARE(completeSpy.findItem("A/a1")->_type, ItemTypeVirtualFileDownload);
    QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_SYNC));
    QCOMPARE(completeSpy.findItem("A/a2")->_type, ItemTypeVirtualFileDownload);

    FUSEVERIFY_NONVIRTUAL(fakeFolder, "A/a1");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "A/a2");
    QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a1").size(), 64);
    QCOMPARE(QFileInfo(fakeFolder.localPath() + "A/a2").size(), 65);
    QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
  }

  void testOnDemandHydration() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);
    fakeFolder.syncJournal().internalPinStates().setForPath("A", PinState::OnlineOnly);

    const auto modtime = QDateTime(QDate(1984, 7, 30), QTime(1, 3, 2));
    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64, 'H');
    fakeFolder.remoteModifier().setModTime("A/a1", modtime);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");

    HydrationPeerResult peerResult;
    QEventLoop loop;
    std::thread peer([&] {
      peerResult = runHydrationPeer(fakeFolder.localPath(), QStringLiteral("A/a1"));
      QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    });
    loop.exec();
    peer.join();

    QVERIFY2(peerResult.committed, qPrintable(peerResult.error));
    QCOMPARE(peerResult.bytes, QByteArray(64, 'H'));
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "A/a1");
    QFile hydrated(fakeFolder.localPath() + "A/a1");
    QVERIFY(hydrated.open(QFile::ReadOnly));
    QCOMPARE(hydrated.readAll(), QByteArray(64, 'H'));
    QCOMPARE(QFileInfo(hydrated).lastModified(), modtime);
    QVERIFY(!vfs->isHydrating());
    QCOMPARE(*fakeFolder.syncJournal().internalPinStates().rawForPath("A/a1"), PinState::Unspecified);

    ItemCompletedSpy completeSpy(fakeFolder);
    QVERIFY(fakeFolder.syncOnce());
    QVERIFY(completeSpy.isEmpty());
  }

  void testHydrationDisconnectRestoresPlaceholder() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");

    QString peerError;
    QEventLoop loop;
    std::thread peer([&] {
      peerError = disconnectHydrationPeer(fakeFolder.localPath(), QStringLiteral("A/a1"));
      QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    });
    loop.exec();
    peer.join();

    QVERIFY2(peerError.isEmpty(), qPrintable(peerError));
    QTRY_VERIFY_WITH_TIMEOUT(!vfs->isHydrating(), 5000);
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 64);
  }

  void testHydrationRejectsIncorrectCommitSize() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");

    HydrationPeerResult peerResult;
    QEventLoop loop;
    std::thread peer([&] {
      peerResult = runHydrationPeer(fakeFolder.localPath(), QStringLiteral("A/a1"), -1);
      QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    });
    loop.exec();
    peer.join();

    QVERIFY(!peerResult.committed);
    QVERIFY(peerResult.error.contains(QStringLiteral("transferred")));
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 64);
  }

  void testHydrationRollbackPreservesDeletion() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");

    const auto path = fakeFolder.localPath() + "A/a1";
    HydrationPeerResult peerResult;
    bool removed = false;
    QEventLoop loop;
    std::thread peer([&] {
      peerResult =
          runHydrationPeer(fakeFolder.localPath(), QStringLiteral("A/a1"), -1, [&] { removed = QFile::remove(path); });
      QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    });
    loop.exec();
    peer.join();

    QVERIFY(removed);
    QVERIFY(!peerResult.committed);
    QVERIFY(peerResult.error.contains(QStringLiteral("transferred")));
    QVERIFY(!QFileInfo::exists(path));
    const auto retainedRecord = dbRecord(fakeFolder, "A/a1");
    QVERIFY(retainedRecord.isValid());
    QCOMPARE(retainedRecord._type, ItemTypeVirtualFile);

    ItemCompletedSpy completeSpy(fakeFolder);
    QVERIFY(fakeFolder.syncOnce());
    QVERIFY(!fakeFolder.currentRemoteState().find("A/a1"));
    FUSEVERIFY_GONE(fakeFolder, "A/a1");
    QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_REMOVE));
  }

  void testRemoteChangeOfReadOnlyPlaceholder() {
    FakeFolder fakeFolder{FileInfo()};
    QVERIFY(setupVfs(fakeFolder));

    const auto readOnlyOnServer = RemotePermissions::fromServerString(QStringLiteral("GRDNVCK"));
    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    fakeFolder.remoteModifier().find("A/a1")->permissions = readOnlyOnServer;
    QVERIFY(fakeFolder.syncOnce());

    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 64);
    QCOMPARE(fusePlaceholderMarkerSize(fakeFolder.localPath() + "A/a1"), 64);
    QVERIFY(!QFileInfo(fakeFolder.localPath() + "A/a1").permission(QFile::WriteOwner));

    fakeFolder.remoteModifier().appendByte("A/a1");
    fakeFolder.remoteModifier().find("A/a1")->permissions = readOnlyOnServer;
    QVERIFY(fakeFolder.syncOnce());

    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(dbRecord(fakeFolder, "A/a1")._fileSize, 65);
    QCOMPARE(fusePlaceholderMarkerSize(fakeFolder.localPath() + "A/a1"), 65);
    QVERIFY(!QFileInfo(fakeFolder.localPath() + "A/a1").permission(QFile::WriteOwner));
  }

  void testLostAndStaleMarkersAreRepaired() {
    FakeFolder fakeFolder{FileInfo()};
    QVERIFY(setupVfs(fakeFolder));

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert("A/a1", 64);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");

    const auto path = fakeFolder.localPath() + "A/a1";
    const auto localPath = path.toUtf8();
    QVERIFY(removexattr(localPath.constData(), placeholderMarkerName) == 0);
    fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem, {"A"});
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(fakeFolder.currentRemoteState().find("A/a1")->size, 64);

    const auto staleSize = QByteArrayLiteral("1");
    QVERIFY(
        setxattr(
            localPath.constData(),
            placeholderMarkerName,
            staleSize.constData(),
            static_cast<size_t>(staleSize.size()),
            0) == 0);
    fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem, {"A"});
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(fakeFolder.currentRemoteState().find("A/a1")->size, 64);

    const auto malformedSize = QByteArrayLiteral("not-a-size");
    QVERIFY(
        setxattr(
            localPath.constData(),
            placeholderMarkerName,
            malformedSize.constData(),
            static_cast<size_t>(malformedSize.size()),
            0) == 0);
    fakeFolder.syncEngine().setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem, {"A"});
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QCOMPARE(fakeFolder.currentRemoteState().find("A/a1")->size, 64);
  }

  void testDehydrationPreservesFilesystemMetadata() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    const auto directory = fakeFolder.localPath() + "A";
    const auto path = directory + "/a1";
    QVERIFY(QDir().mkpath(directory));
    QFile file(path);
    QVERIFY(file.open(QFile::WriteOnly));
    QCOMPARE(file.write(QByteArray(64, 'H')), 64);
    file.close();

    const mode_t preservedMode = S_ISUID | S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
    QVERIFY(::chmod(path.toUtf8().constData(), preservedMode) == 0);
    const auto preservedValue = QByteArrayLiteral("preserved");
    QVERIFY(
        setxattr(
            path.toUtf8().constData(),
            preservedAttributeName,
            preservedValue.constData(),
            static_cast<size_t>(preservedValue.size()),
            0) == 0);

    SyncFileItem item;
    item._file = QStringLiteral("A/a1");
    item._size = 64;
    item._modtime = QDateTime::currentSecsSinceEpoch();
    const auto result = vfs->dehydratePlaceholder(item);
    if (!result) {
      QFAIL(qPrintable(result.error()));
    }

    QCOMPARE(QFileInfo(path).size(), 0);
    QCOMPARE(fusePlaceholderMarkerSize(path), 64);
    QCOMPARE(extendedAttribute(path, preservedAttributeName), preservedValue);
    struct stat attributes = {};
    QVERIFY(::stat(path.toUtf8().constData(), &attributes) == 0);
    QCOMPARE(attributes.st_mode & 07777, preservedMode);
  }

  void testDehydrationRejectsNegativeSize() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    const auto directory = fakeFolder.localPath() + "A";
    const auto path = directory + "/a1";
    QVERIFY(QDir().mkpath(directory));
    QFile file(path);
    QVERIFY(file.open(QFile::WriteOnly));
    QCOMPARE(file.write("hydrated"), 8);
    file.close();
    const auto directoryEntries = QDir(directory).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);

    SyncFileItem item;
    item._file = QStringLiteral("A/a1");
    item._size = -1;
    item._modtime = QDateTime::currentSecsSinceEpoch();
    const auto result = vfs->dehydratePlaceholder(item);
    QVERIFY(!result);
    QVERIFY(result.error().contains(QStringLiteral("negative")));

    QFile preserved(path);
    QVERIFY(preserved.open(QFile::ReadOnly));
    QCOMPARE(preserved.readAll(), QByteArrayLiteral("hydrated"));
    QVERIFY(!hasFusePlaceholderMarker(path));
    QCOMPARE(QDir(directory).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot), directoryEntries);
  }

  void testPlaceholderCreationPreservesLocalFile() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    const auto directory = fakeFolder.localPath() + "A";
    const auto path = directory + "/a1";
    QVERIFY(QDir().mkpath(directory));
    QFile file(path);
    QVERIFY(file.open(QFile::WriteOnly));
    QCOMPARE(file.write("local"), 5);
    file.close();

    SyncFileItem item;
    item._file = QStringLiteral("A/a1");
    item._size = 64;
    item._modtime = QDateTime::currentSecsSinceEpoch();
    const auto result = vfs->createPlaceholder(item);
    QVERIFY(!result);
    QVERIFY(result.error().contains(QStringLiteral("local file changed")));

    QFile preserved(path);
    QVERIFY(preserved.open(QFile::ReadOnly));
    QCOMPARE(preserved.readAll(), QByteArrayLiteral("local"));
    QVERIFY(!hasFusePlaceholderMarker(path));
  }

  void testDehydrationDoesNotRecreateMissingFile() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    SyncFileItem item;
    item._file = QStringLiteral("A/a1");
    item._size = 64;
    item._modtime = QDateTime::currentSecsSinceEpoch();
    const auto result = vfs->dehydratePlaceholder(item);
    QVERIFY(!result);
    QVERIFY(result.error().contains(QStringLiteral("no longer present")));
    QVERIFY(!QFileInfo::exists(fakeFolder.localPath() + "A/a1"));
  }

  void testDehydrationPreservesAlwaysLocalPin() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    const auto directory = fakeFolder.localPath() + "A";
    const auto path = directory + "/a1";
    QVERIFY(QDir().mkpath(directory));
    QFile file(path);
    QVERIFY(file.open(QFile::WriteOnly));
    QCOMPARE(file.write("hydrated"), 8);
    file.close();
    QVERIFY(vfs->setPinState(QStringLiteral("A/a1"), PinState::AlwaysLocal));

    SyncFileItem item;
    item._file = QStringLiteral("A/a1");
    item._size = 64;
    item._modtime = QDateTime::currentSecsSinceEpoch();
    const auto result = vfs->dehydratePlaceholder(item);
    QVERIFY(!result);
    QVERIFY(result.error().contains(QStringLiteral("pinned for offline use")));

    QFile preserved(path);
    QVERIFY(preserved.open(QFile::ReadOnly));
    QCOMPARE(preserved.readAll(), QByteArrayLiteral("hydrated"));
    QVERIFY(!hasFusePlaceholderMarker(path));
    QCOMPARE(*vfs->pinState(QStringLiteral("A/a1")), PinState::AlwaysLocal);
  }

  void testDehydrationRefusesSharedReplacementLease() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    const auto directory = fakeFolder.localPath() + "A";
    const auto path = directory + "/a1";
    QVERIFY(QDir().mkpath(directory));
    QFile file(path);
    QVERIFY(file.open(QFile::WriteOnly));
    QCOMPARE(file.write(QByteArray(64, 'H')), 64);
    file.close();

    struct stat attributes = {};
    QVERIFY(::lstat(path.toUtf8().constData(), &attributes) == 0);
    ncvfs::unique_fd replacementLease(
        ::open(
            QFile::encodeName(ncvfs::replacement_lock_path(fakeFolder.localPath())).constData(),
            O_RDWR | O_CREAT | O_CLOEXEC,
            0600));
    QVERIFY(replacementLease);
    QVERIFY(ncvfs::set_replacement_lock(replacementLease.get(), attributes, F_RDLCK) == 0);

    SyncFileItem item;
    item._file = QStringLiteral("A/a1");
    item._size = 64;
    item._modtime = QDateTime::currentSecsSinceEpoch();
    const auto openResult = vfs->dehydratePlaceholder(item);
    QVERIFY(!openResult);
    QVERIFY(openResult.error().contains(QStringLiteral("open through the FUSE mount")));
    QVERIFY(!hasFusePlaceholderMarker(path));

    replacementLease.reset();
    const auto closedResult = vfs->dehydratePlaceholder(item);
    if (!closedResult) {
      QFAIL(qPrintable(closedResult.error()));
    }
    QCOMPARE(fusePlaceholderMarkerSize(path), 64);
  }

  void testHydratedOfflineWritesUploadAndConflict() {
    FakeFolder fakeFolder{FileInfo()};
    const auto vfs = setupVfs(fakeFolder);
    QVERIFY(vfs);

    fakeFolder.remoteModifier().mkdir("C");
    fakeFolder.remoteModifier().insert("C/c1", 24);
    fakeFolder.remoteModifier().insert("C/c2", 24);
    QVERIFY(fakeFolder.syncOnce());
    FUSEVERIFY_VIRTUAL(fakeFolder, "C/c1");
    FUSEVERIFY_VIRTUAL(fakeFolder, "C/c2");

    const auto hydrate = [&](const QString& folderPath) {
      HydrationPeerResult result;
      QEventLoop loop;
      std::thread peer([&] {
        result = runHydrationPeer(fakeFolder.localPath(), folderPath);
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
      });
      loop.exec();
      peer.join();
      return result;
    };

    const auto c1Hydration = hydrate(QStringLiteral("C/c1"));
    QVERIFY2(c1Hydration.committed, qPrintable(c1Hydration.error));
    QCOMPARE(c1Hydration.bytes.size(), 24);
    const auto c2Hydration = hydrate(QStringLiteral("C/c2"));
    QVERIFY2(c2Hydration.committed, qPrintable(c2Hydration.error));
    QCOMPARE(c2Hydration.bytes.size(), 24);
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "C/c1");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "C/c2");

    fakeFolder.localModifier().appendByte("C/c1");
    fakeFolder.localModifier().appendByte("C/c2");
    fakeFolder.remoteModifier().appendByte("C/c2");
    fakeFolder.remoteModifier().appendByte("C/c2");

    ItemCompletedSpy completeSpy(fakeFolder);
    QVERIFY(fakeFolder.syncOnce());
    QCOMPARE(fakeFolder.currentRemoteState().find("C/c1")->size, 25);
    QVERIFY(itemInstruction(completeSpy, "C/c1", CSYNC_INSTRUCTION_SYNC));
    QCOMPARE(fakeFolder.currentRemoteState().find("C/c2")->size, 26);
    QVERIFY(itemInstruction(completeSpy, "C/c2", CSYNC_INSTRUCTION_CONFLICT));
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "C/c1");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "C/c2");
  }

  void testSyncDehydration() {
    FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
    QVERIFY(setupVfs(fakeFolder));

    QVERIFY(fakeFolder.syncOnce());
    QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

    ItemCompletedSpy completeSpy(fakeFolder);
    auto cleanup = [&]() { completeSpy.clear(); };
    cleanup();

    const auto hydratedInode = fileInode(fakeFolder.localPath() + "A/a1");
    QVERIFY(hydratedInode != 0);
    markForDehydration(fakeFolder, "A/a1");

    markForDehydration(fakeFolder, "A/a2");
    fakeFolder.remoteModifier().appendByte("A/a2");

    markForDehydration(fakeFolder, "B/b1");
    fakeFolder.remoteModifier().remove("B/b1");

    markForDehydration(fakeFolder, "B/b2");
    fakeFolder.remoteModifier().rename("B/b2", "B/b3");

    markForDehydration(fakeFolder, "C/c1");
    fakeFolder.localModifier().appendByte("C/c1");

    markForDehydration(fakeFolder, "C/c2");
    fakeFolder.localModifier().appendByte("C/c2");
    fakeFolder.remoteModifier().appendByte("C/c2");
    fakeFolder.remoteModifier().appendByte("C/c2");

    QVERIFY(fakeFolder.syncOnce());

    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    QVERIFY(fileInode(fakeFolder.localPath() + "A/a1") != hydratedInode);
    QVERIFY(itemInstruction(completeSpy, "A/a1", CSYNC_INSTRUCTION_SYNC));
    QCOMPARE(completeSpy.findItem("A/a1")->_type, ItemTypeVirtualFileDehydration);
    QCOMPARE(completeSpy.findItem("A/a1")->_file, QStringLiteral("A/a1"));

    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a2");
    QVERIFY(itemInstruction(completeSpy, "A/a2", CSYNC_INSTRUCTION_SYNC));
    QCOMPARE(completeSpy.findItem("A/a2")->_type, ItemTypeVirtualFileDehydration);

    FUSEVERIFY_GONE(fakeFolder, "B/b1");
    QVERIFY(!fakeFolder.currentRemoteState().find("B/b1"));
    QVERIFY(itemInstruction(completeSpy, "B/b1", CSYNC_INSTRUCTION_REMOVE));

    QVERIFY(!QFileInfo(fakeFolder.localPath() + "B/b2").exists());
    QVERIFY(!fakeFolder.currentRemoteState().find("B/b2"));
    FUSEVERIFY_VIRTUAL(fakeFolder, "B/b3");
    QVERIFY(itemInstruction(completeSpy, "B/b2", CSYNC_INSTRUCTION_REMOVE));
    QVERIFY(itemInstruction(completeSpy, "B/b3", CSYNC_INSTRUCTION_NEW));

    QCOMPARE(fakeFolder.currentRemoteState().find("C/c1")->size, 25);
    QVERIFY(itemInstruction(completeSpy, "C/c1", CSYNC_INSTRUCTION_SYNC));

    QCOMPARE(fakeFolder.currentRemoteState().find("C/c2")->size, 26);
    QVERIFY(itemInstruction(completeSpy, "C/c2", CSYNC_INSTRUCTION_CONFLICT));
    cleanup();

    auto expectedRemoteState = fakeFolder.currentRemoteState();
    QVERIFY(fakeFolder.syncOnce());
    QCOMPARE(fakeFolder.currentRemoteState(), expectedRemoteState);

    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a1");
    FUSEVERIFY_VIRTUAL(fakeFolder, "A/a2");
    FUSEVERIFY_GONE(fakeFolder, "B/b1");
    QVERIFY(!QFileInfo(fakeFolder.localPath() + "B/b2").exists());
    FUSEVERIFY_VIRTUAL(fakeFolder, "B/b3");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "C/c1");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "C/c2");
  }

  void testLongNonAsciiNameBecomesPlaceholder() {
    FakeFolder fakeFolder{FileInfo()};
    QVERIFY(setupVfs(fakeFolder));

    const QString longCyrillicPath = QStringLiteral("A/") + QString(125, QChar(0x0431));
    QCOMPARE(longCyrillicPath.toUtf8().size(), 252);

    fakeFolder.remoteModifier().mkdir("A");
    fakeFolder.remoteModifier().insert(longCyrillicPath, 64);
    QVERIFY(fakeFolder.syncOnce());

    FUSEVERIFY_VIRTUAL(fakeFolder, longCyrillicPath);
    QCOMPARE(dbRecord(fakeFolder, longCyrillicPath)._fileSize, 64);
    QVERIFY(fakeFolder.currentRemoteState().find(longCyrillicPath));
  }

  void testNewVirtuals() {
    FakeFolder fakeFolder{FileInfo()};
    QVERIFY(setupVfs(fakeFolder));
    QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

    auto setPin = [&](const QByteArray& path, PinState state) {
      fakeFolder.syncJournal().internalPinStates().setForPath(path, state);
    };

    fakeFolder.remoteModifier().mkdir("local");
    fakeFolder.remoteModifier().mkdir("online");
    fakeFolder.remoteModifier().mkdir("unspec");
    QVERIFY(fakeFolder.syncOnce());
    QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

    setPin("local", PinState::AlwaysLocal);
    setPin("online", PinState::OnlineOnly);
    setPin("unspec", PinState::Unspecified);

    fakeFolder.remoteModifier().insert("file1");
    fakeFolder.remoteModifier().insert("online/file1");
    fakeFolder.remoteModifier().insert("local/file1");
    fakeFolder.remoteModifier().insert("unspec/file1");
    QVERIFY(fakeFolder.syncOnce());

    FUSEVERIFY_VIRTUAL(fakeFolder, "file1");
    FUSEVERIFY_VIRTUAL(fakeFolder, "online/file1");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "local/file1");
    FUSEVERIFY_VIRTUAL(fakeFolder, "unspec/file1");

    setPin(QByteArray(), PinState::AlwaysLocal);

    fakeFolder.remoteModifier().insert("file2");
    fakeFolder.remoteModifier().insert("online/file2");
    fakeFolder.remoteModifier().insert("local/file2");
    fakeFolder.remoteModifier().insert("unspec/file2");
    QVERIFY(fakeFolder.syncOnce());

    FUSEVERIFY_NONVIRTUAL(fakeFolder, "file2");
    FUSEVERIFY_VIRTUAL(fakeFolder, "online/file2");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "local/file2");
    FUSEVERIFY_VIRTUAL(fakeFolder, "unspec/file2");

    FUSEVERIFY_NONVIRTUAL(fakeFolder, "file1");

    FUSEVERIFY_VIRTUAL(fakeFolder, "online/file1");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "local/file1");
    FUSEVERIFY_VIRTUAL(fakeFolder, "unspec/file1");

    setPin(QByteArray(), PinState::OnlineOnly);

    fakeFolder.remoteModifier().insert("file3");
    fakeFolder.remoteModifier().insert("online/file3");
    fakeFolder.remoteModifier().insert("local/file3");
    fakeFolder.remoteModifier().insert("unspec/file3");
    QVERIFY(fakeFolder.syncOnce());

    FUSEVERIFY_VIRTUAL(fakeFolder, "file3");
    FUSEVERIFY_VIRTUAL(fakeFolder, "online/file3");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "local/file3");
    FUSEVERIFY_VIRTUAL(fakeFolder, "unspec/file3");

    FUSEVERIFY_VIRTUAL(fakeFolder, "file1");

    FUSEVERIFY_VIRTUAL(fakeFolder, "online/file1");
    FUSEVERIFY_NONVIRTUAL(fakeFolder, "local/file1");
    FUSEVERIFY_VIRTUAL(fakeFolder, "unspec/file1");
  }
};

QTEST_GUILESS_MAIN(TestSyncFuse)
#include "testsyncfuse.moc"
