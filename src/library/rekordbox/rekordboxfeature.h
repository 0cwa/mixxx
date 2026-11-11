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
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringListModel>
#include <QVector>
#include <QtConcurrentRun>
#include <atomic>

#include "audio/frame.h"
#include "library/baseexternallibraryfeature.h"
#include "library/baseexternalplaylistmodel.h"
#include "library/baseexternaltrackmodel.h"
#include "library/memorycuepromotionconfig.h"
#include "library/rekordbox/rekordboxanlzimporter.h"
#include "library/rekordbox/rekordboxpdbdecoder.h"
#include "library/rekordbox/rekordboxxmlparser.h"
#include "library/treeitemmodel.h"
#include "util/color/rgbcolor.h"
#include "util/db/dbconnectionpool.h"
#include "util/parented_ptr.h"
#include "util/sandbox.h"

namespace mixxx::rekordbox {

struct MemoryCueLoop {
    audio::FramePos startPosition;
    audio::FramePos endPosition;
    QString comment;
    RgbColor::optional_t color;
    bool fromExtendedSection{false};
    int sourceOrder{0};
};

struct DeviceImportResult {
    QString devicePath;
    QString device;
    QStringList diagnostics;
    std::shared_ptr<const PdbData> data;
    QByteArray sourceFingerprint;
    bool applied{false};
};

DeviceImportResult importDeviceDatabase(mixxx::DbConnectionPoolPtr dbConnectionPool,
        const QString& devicePath,
        const QString& device,
        const SecurityTokenPointer& securityToken,
        const std::shared_ptr<std::atomic<bool>>& cancelled);

bool removeDeviceDatabase(QSqlDatabase& database, const QString& devicePath);

// CUES and CUES2 commonly contain the same loop. Deduplicate only loops with
// the same complete interval, and prefer the richer CUES2 record. Memory cues
// are never deduplicated by position because multiple cues may share a start.
QList<MemoryCueLoop> normalizeMemoryCueLoops(const QList<MemoryCueLoop>& cues);
bool memoryCueLoopLess(const MemoryCueLoop& a, const MemoryCueLoop& b);
QString resolveDeviceMediaPath(const QString& devicePath, const std::string& sourcePath);

/// Returns a stable identity for a track's external ANLZ source.
QString analyzeImportSourceIdentity(
        const QString& trackLocation,
        const QString& analyzePath);

/// Records the database identity used by XML playlist/order mappings.
void recordXmlTrackMapping(
        QHash<int, int>* trackIds,
        QVector<int>* importedTrackIds,
        int xmlTrackId,
        int databaseTrackId);

/// Applies XML timing annotations to an externally loaded Track. Beatgrids are
/// applied only while BPM is unlocked; cues and loops are always considered.
void applyXmlTrackAnnotations(
        TrackPointer track,
        audio::SampleRate sampleRate,
        const QByteArray& serializedMetadata);

} // namespace mixxx::rekordbox

class TrackCollectionManager;
class BaseExternalPlaylistModel;

class RekordboxPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT
  public:
    RekordboxPlaylistModel(QObject* parent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource,
            UserSettingsPointer pConfig);
    TrackPointer getTrack(const QModelIndex& index) const override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnInternal(int column) override;

  protected:
    void initSortColumnMapping() override;

  private:
    UserSettingsPointer m_pConfig;
    mutable QHash<QString, mixxx::rekordbox::AnlzImportState> m_analyzeImportStates;
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
    void onDeviceTracksFound();

  private:
    enum class PendingOperation { None,
        Discovery,
        DeviceImport,
        XmlImport };

    void finishOperation();
    QString formatRootViewHtml() const;
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<RekordboxPlaylistModel> m_pRekordboxPlaylistModel;

    QFutureWatcher<QList<TreeItem*>> m_devicesFutureWatcher;
    QFuture<QList<TreeItem*>> m_devicesFuture;
    QFutureWatcher<QString> m_tracksFutureWatcher;
    QFuture<QString> m_tracksFuture;
    QFutureWatcher<mixxx::rekordbox::DeviceImportResult> m_deviceImportWatcher;
    QFuture<mixxx::rekordbox::DeviceImportResult> m_deviceImportFuture;
    std::shared_ptr<std::atomic<bool>> m_cancelDeviceImport;
    bool m_refreshAfterDeviceImport{false};
    PendingOperation m_pendingOperation{PendingOperation::None};
    QString m_title;

    QSharedPointer<BaseTrackCache> m_trackSource;
};
