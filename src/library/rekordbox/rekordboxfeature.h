// This feature reads tracks, playlists and folders from removable Recordbox
// prepared devices (USB drives, etc), by parsing the binary *.PDB files
// stored on each removable device. It does not read the locally stored
// Rekordbox database (Collection).

// It draws heavily from the hard work completed here:

//      https://github.com/Deep-Symmetry/crate-digger

// And uses the C++ Kaitai Struct binary parsing libraries:

//      http://kaitai.io
//      https://github.com/kaitai-io/kaitai_struct
//      https://github.com/kaitai-io/kaitai_struct_cpp_stl_runtime

// The *.PDB C++ files:

//      rekordbox_pdb.h
//      rekordbox_pdb.cpp

// Were generated from the following structure definition file:

//      https://github.com/Deep-Symmetry/crate-digger/blob/master/src/main/kaitai/rekordbox_pdb.ksy

#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QList>
#include <QStringListModel>
#include <QtConcurrentRun>
#include <fstream>
#include <optional>

#include "library/baseexternallibraryfeature.h"
#include "library/baseexternalplaylistmodel.h"
#include "library/baseexternaltrackmodel.h"
#include "library/treeitemmodel.h"
#include "util/parented_ptr.h"

class TrackCollectionManager;
class BaseExternalPlaylistModel;

struct RekordboxDeviceInfo {
    QString label;
    QString path;
};

struct RekordboxPlaylistNode {
    QString label;
    QString path;
    bool isFolder{false};
    QList<RekordboxPlaylistNode> children;
};

struct RekordboxDeviceImportResult {
    RekordboxDeviceInfo device;
    QString devicePlaylist;
    QList<RekordboxPlaylistNode> playlistNodes;
};

class RekordboxPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT
  public:
    RekordboxPlaylistModel(QObject* parent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);
    TrackPointer getTrack(const QModelIndex& index) const override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnInternal(int column) override;

  protected:
    void initSortColumnMapping() override;
};

class RekordboxFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    RekordboxFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~RekordboxFeature() override;

    QVariant title() override;
    static bool isSupported();
    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void refreshLibraryModels();
    void onRekordboxDevicesFound();
    void onTracksFound();

  private slots:
    void htmlLinkClicked(const QUrl& link);

  private:
    QString formatRootViewHtml() const;
    void requestDeviceRefresh();
    void startDeviceRefresh();
    void refreshDevicesAfterImport();
    void restorePendingDeviceMarker();
    TreeItem* findDeviceItem(const RekordboxDeviceInfo& device) const;
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<RekordboxPlaylistModel> m_pRekordboxPlaylistModel;

    QFutureWatcher<QList<RekordboxDeviceInfo>> m_devicesFutureWatcher;
    QFuture<QList<RekordboxDeviceInfo>> m_devicesFuture;
    QFutureWatcher<RekordboxDeviceImportResult> m_tracksFutureWatcher;
    QFuture<RekordboxDeviceImportResult> m_tracksFuture;
    std::optional<RekordboxDeviceInfo> m_pendingDevice;
    bool m_deviceRefreshPending{false};
    bool m_deviceScanActive{false};
    QString m_title;

    QSharedPointer<BaseTrackCache> m_trackSource;
};
