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
#include <fstream>

#include "audio/frame.h"
#include "library/baseexternallibraryfeature.h"
#include "library/baseexternalplaylistmodel.h"
#include "library/baseexternaltrackmodel.h"
#include "library/memorycuepromotionconfig.h"
#include "library/rekordbox/rekordboxsource.h"
#include "library/rekordbox/rekordboxxmlparser.h"
#include "library/treeitemmodel.h"
#include "util/color/rgbcolor.h"
#include "util/parented_ptr.h"

namespace mixxx::rekordbox {

struct MemoryCueLoop {
    audio::FramePos startPosition;
    audio::FramePos endPosition;
    QString comment;
    RgbColor::optional_t color;
    bool fromExtendedSection{false};
    int sourceOrder{0};
};

// CUES and CUES2 commonly contain the same loop. Deduplicate only loops with
// the same complete interval, and prefer the richer CUES2 record. Memory cues
// are never deduplicated by position because multiple cues may share a start.
QList<MemoryCueLoop> normalizeMemoryCueLoops(const QList<MemoryCueLoop>& cues);

/// Returns a stable identity for a track's external ANLZ source.
QString analyzeImportSourceIdentity(
        const QString& trackLocation,
        const QString& analyzePath);

/// Returns whether an ANLZ source has not been imported in the current model.
bool shouldImportAnalyzeSource(
        const QString& sourceIdentity,
        const QSet<QString>& importedSources);

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
class WLibraryTextBrowser;

class RekordboxPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT
  public:
    RekordboxPlaylistModel(QObject* parent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource,
            UserSettingsPointer pConfig,
            std::shared_ptr<mixxx::rekordbox::SourceSessions> sources = {},
            std::shared_ptr<mixxx::rekordbox::ImportSourceRegistry> registry = {});
    TrackPointer getTrack(const QModelIndex& index) const override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnInternal(int column) override;

  protected:
    void initSortColumnMapping() override;

  private:
    TrackPointer getSourceTrack(const QModelIndex& index,
            const std::shared_ptr<const mixxx::rekordbox::SourceSnapshot>& snapshot) const;
    UserSettingsPointer m_pConfig;
    std::shared_ptr<mixxx::rekordbox::SourceSessions> m_sources;
    std::shared_ptr<mixxx::rekordbox::ImportSourceRegistry> m_registry;
    mutable bool m_importInProgress = false;
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

  private slots:
    void htmlLinkClicked(const QUrl& link);

  private:
    QString formatRootViewHtml() const;
    void startSource(const QString& sourceId);
    void sourceFinished(const mixxx::rekordbox::SourceResult& result);
    void rebuildSidebar();
    void updateLoadingState();
    void persistSources();
    QString addSource(mixxx::rekordbox::SourceKind kind,
            const QString& locator,
            const QString& displayName,
            const QString& volumeIdentity = {});
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<RekordboxPlaylistModel> m_pRekordboxPlaylistModel;

    QFutureWatcher<QVector<mixxx::rekordbox::SourceDescriptor>> m_devicesFutureWatcher;
    QFuture<QVector<mixxx::rekordbox::SourceDescriptor>> m_devicesFuture;
    QList<QFutureWatcher<mixxx::rekordbox::SourceResult>*> m_sourceJobs;
    std::shared_ptr<mixxx::rekordbox::SourceSessions> m_sources;
    std::shared_ptr<mixxx::rekordbox::ImportSourceRegistry> m_registry;
    QString m_title;
    QPointer<WLibraryTextBrowser> m_homeView;

    QSharedPointer<BaseTrackCache> m_trackSource;
};
