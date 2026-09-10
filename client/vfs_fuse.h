/*
 * SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <QHash>
#include <QObject>

#include "common/plugin.h"
#include "common/vfs.h"

class QLocalServer;
class QLocalSocket;
class QJsonObject;

namespace OCC {

class HydrationJob;
struct FuseHydrationRequest;

class VfsFuse : public Vfs {
  Q_OBJECT

 public:
  explicit VfsFuse(QObject* parent = nullptr);
  ~VfsFuse() override;

  [[nodiscard]] Mode mode() const override;
  [[nodiscard]] QString fileSuffix() const override;
  [[nodiscard]] QString userFacingPath() const override;

  void stop() override;
  void unregisterFolder() override;

  [[nodiscard]] bool socketApiPinStateActionsShown() const override;
  [[nodiscard]] bool isHydrating() const override;
  void setDemandDrivenDiscoveryEnabled(bool enabled) override;
  [[nodiscard]] bool demandDrivenDiscoveryEnabled() const override;
  void initialDiscoveryFinished() override;

  OCC::Result<OCC::Vfs::ConvertToPlaceholderResult, QString>
  updateMetadata(const SyncFileItem& syncItem, const QString& filePath, const QString& replacesFile) override;
  Result<Vfs::ConvertToPlaceholderResult, QString> updatePlaceholderMarkInSync(
      const QString& filePath,
      const SyncFileItem& syncItem) override;
  [[nodiscard]] bool isPlaceHolderInSync(const QString& filePath) const override;

  Result<void, QString> createPlaceholder(const SyncFileItem& item) override;
  Result<void, QString> createPlaceholders(const QList<SyncFileItemPtr>& items) override;

  Result<void, QString> dehydratePlaceholder(const SyncFileItem& item) override;
  Result<ConvertToPlaceholderResult, QString> convertToPlaceholder(
      const QString& filename,
      const SyncFileItem& item,
      const QString& replacesFile,
      UpdateMetadataTypes updateType) override;

  bool needsMetadataUpdate(const SyncFileItem& item) override;
  bool isDehydratedPlaceholder(const QString& filePath) override;
  bool statTypeVirtualFile(csync_file_stat_t* stat, void* statData) override;

  bool setPinState(const QString& folderPath, PinState state) override;
  Optional<PinState> pinState(const QString& folderPath) override;
  AvailabilityResult availability(const QString& folderPath, const AvailabilityRecursivity recursiveCheck) override;

 public slots:
  void fileStatusChanged(const QString& systemFileName, OCC::SyncFileStatus fileStatus) override;

 protected:
  void startImpl(const VfsSetupParams& params) override;

 private:
  enum class TargetExpectation {
    Existing,
    MissingOrUnchanged,
  };

  void acceptHydrationConnections();
  void processHydrationInput(FuseHydrationRequest* request);
  void processControlRequest(FuseHydrationRequest* request, const QJsonObject& message);
  void finishControlRequest(FuseHydrationRequest* request, QJsonObject response);
  void beginHydration(FuseHydrationRequest* request, const QString& folderPath);
  void hydrationJobFinished(FuseHydrationRequest* request);
  void abortHydration(FuseHydrationRequest* request, const QString& error);
  void finalizeHydration(FuseHydrationRequest* request, bool commit, qint64 transferredBytes, const QString& error);
  Result<void, QString> commitHydration(FuseHydrationRequest* request, qint64 transferredBytes);
  Result<void, QString> restorePlaceholder(FuseHydrationRequest* request);
  bool sendHydrationMessage(FuseHydrationRequest* request, QJsonObject message);

  Result<void, QString>
  replaceWithPlaceholder(const QString& path, const SyncFileItem& item, TargetExpectation expectation);

  bool _extendedAttributesUsable = false;
  bool _demandDrivenDiscoveryEnabled = false;
  QString _userFacingPath;
  QLocalServer* _hydrationServer = nullptr;
  QHash<QLocalSocket*, FuseHydrationRequest*> _hydrationRequests;
};

class FuseVfsPluginFactory : public QObject, public DefaultPluginFactory<VfsFuse> {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "org.owncloud.PluginFactory" FILE "vfspluginmetadata.json")
  Q_INTERFACES(OCC::PluginFactory)
};

}  // namespace OCC
