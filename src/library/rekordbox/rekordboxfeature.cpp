#include "library/rekordbox/rekordboxfeature.h"

#include <mp3guessenc.h>

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QSet>
#include <QSettings>
#include <QStorageInfo>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QtDebug>
#include <algorithm>
#include <functional>
#include <utility>

#include "engine/engine.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/memorycuepromotionconfig.h"
#include "library/queryutil.h"
#include "library/rekordbox/rekordboxanlzimporter.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "library/rekordbox/rekordboxsourceparser.h"
#include "library/rekordbox/rekordboxxmlimporter.h"
#include "library/rekordbox/rekordboxxmlparser.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_rekordboxfeature.cpp"
#include "sources/soundsourceproxy.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/globaltrackcache.h"
#include "track/keyfactory.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "util/color/color.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/sandbox.h"
#include "waveform/waveform.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

namespace {

const QString kRekordboxLibraryTable = QStringLiteral("rekordbox_library");
const QString kRekordboxPlaylistsTable = QStringLiteral("rekordbox_playlists");
const QString kRekordboxPlaylistTracksTable = QStringLiteral("rekordbox_playlist_tracks");

const QString kRekordboxXmlImportLink = QStringLiteral("import-xml");
const QString kRekordboxXmlMetadata = QStringLiteral("xml_metadata");

void setMemoryCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int,
        const QString& label,
        mixxx::RgbColor::optional_t color) {
    CuePointer pCue;

    mixxx::CueType type = mixxx::CueType::Memory;
    if (endPosition.isValid()) {
        type = mixxx::CueType::Loop;
    }

    // ANLZ data can be read more than once while an external track is
    // requested. Match the complete imported record before creating a cue so
    // repeated reads are idempotent without removing user-created cues.
    const QList<CuePointer> cuePoints = track->getCuePoints();
    for (const CuePointer& trackCue : cuePoints) {
        if (trackCue->getType() == type &&
                trackCue->getPosition() == startPosition &&
                trackCue->getEndPosition() == endPosition &&
                trackCue->getLabel() == label &&
                (!color || trackCue->getColor() == *color)) {
            pCue = trackCue;
            break;
        }
    }

    if (pCue) {
        pCue->setStartAndEndPosition(startPosition, endPosition);
    } else {
        pCue = track->createAndAddCue(
                type,
                Cue::kNoHotCue,
                startPosition,
                endPosition);
    }
    pCue->setLabel(label);
    if (color) {
        pCue->setColor(*color);
    }
}

mixxx::RgbColor::optional_t xmlCueColor(const QString& value) {
    const QStringList components = value.split(QLatin1Char(','));
    if (components.size() != 3) {
        return mixxx::RgbColor::nullopt();
    }
    bool ok = false;
    const int red = components[0].toInt(&ok);
    if (!ok) {
        return mixxx::RgbColor::nullopt();
    }
    const int green = components[1].toInt(&ok);
    if (!ok) {
        return mixxx::RgbColor::nullopt();
    }
    const int blue = components[2].toInt(&ok);
    if (!ok || red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255) {
        return mixxx::RgbColor::nullopt();
    }
    return mixxx::RgbColor::optional(qRgb(red, green, blue));
}

void applyXmlMetadata(
        TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        const QByteArray& serializedMetadata) {
    if (!track || !sampleRate.isValid() || serializedMetadata.isEmpty()) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(serializedMetadata, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning() << "Skipping malformed Rekordbox XML annotation:" << parseError.errorString();
        return;
    }

    const QJsonObject metadata = document.object();
    if (metadata.value(QStringLiteral("schemaVersion")).toInt() >= 2) {
        qWarning() << "Schema-2 XML annotations require transactional source admission";
        return;
    }
    QVector<mixxx::audio::FramePos> beatPositions;
    const QJsonArray beatgrid = metadata.value(QStringLiteral("beatgrid")).toArray();
    if (!track->isBpmLocked()) {
        for (const QJsonValue& value : beatgrid) {
            const double seconds = value.toObject()
                                           .value(QStringLiteral("position"))
                                           .toDouble(-1.0);
            if (seconds >= 0.0) {
                beatPositions << mixxx::audio::FramePos(seconds * sampleRate.value());
            }
        }
    }

    if (!beatPositions.isEmpty()) {
        std::sort(beatPositions.begin(), beatPositions.end());
        beatPositions.erase(std::unique(beatPositions.begin(), beatPositions.end()),
                beatPositions.end());
        mixxx::BeatsPointer beats;
        if (beatPositions.size() >= 2) {
            beats = mixxx::Beats::fromBeatPositions(sampleRate, beatPositions);
        } else {
            const double bpm = beatgrid.first().toObject().value(QStringLiteral("bpm")).toDouble();
            if (bpm > 0.0) {
                beats = mixxx::Beats::fromConstTempo(
                        sampleRate, beatPositions.front(), mixxx::Bpm(bpm));
            }
        }
        if (beats && track->trySetBeats(beats)) {
            track->setBpmLocked(true);
        }
    }

    const QJsonArray cues = metadata.value(QStringLiteral("cues")).toArray();
    int importedCueIndex = 0;
    for (const QJsonValue& value : cues) {
        const QJsonObject item = value.toObject();
        const double start = item.value(QStringLiteral("start")).toDouble(-1.0);
        if (start < 0.0) {
            continue;
        }
        const double end = item.value(QStringLiteral("end")).toDouble(-1.0);
        const QString type = item.value(QStringLiteral("type")).toString();
        const bool isLoop = type.compare(QStringLiteral("loop"), Qt::CaseInsensitive) == 0 &&
                end > start;
        setMemoryCue(
                track,
                mixxx::audio::FramePos(start * sampleRate.value()),
                isLoop ? mixxx::audio::FramePos(end * sampleRate.value())
                       : mixxx::audio::kInvalidFramePos,
                importedCueIndex++,
                item.value(QStringLiteral("name")).toString(),
                xmlCueColor(item.value(QStringLiteral("color")).toString()));
    }
}

// Prepared by ChatGPT: source access helpers for human review.
struct SourceAccessLease {
    SecurityTokenPointer token;
};

std::shared_ptr<const SourceAccessLease> sourceAccess(
        const mixxx::rekordbox::SourceDescriptor& source, bool prompt, QString* error) {
    mixxx::FileInfo info(source.locator);
    auto token = source.kind == mixxx::rekordbox::SourceKind::Usb
            ? Sandbox::openSecurityTokenForDir(QDir(source.locator), true)
            : Sandbox::openSecurityToken(&info, true);
    bool accessible = source.kind == mixxx::rekordbox::SourceKind::Usb
            ? Sandbox::canAccessDir(QDir(source.locator))
            : Sandbox::canAccess(&info);
    if (!accessible && prompt && Sandbox::askForAccess(&info)) {
        token = source.kind == mixxx::rekordbox::SourceKind::Usb
                ? Sandbox::openSecurityTokenForDir(QDir(source.locator), true)
                : Sandbox::openSecurityToken(&info, true);
        accessible = source.kind == mixxx::rekordbox::SourceKind::Usb
                ? Sandbox::canAccessDir(QDir(source.locator))
                : Sandbox::canAccess(&info);
    }
    if (!accessible) {
        *error = QStringLiteral("Access to the selected source was denied.");
        return {};
    }
    return std::make_shared<const SourceAccessLease>(SourceAccessLease{std::move(token)});
}

QString volumeIdentity(const QStorageInfo& storage) {
#ifdef __LINUX__
    const QString device = QString::fromUtf8(storage.device());
    const auto entries = QDir(QStringLiteral("/dev/disk/by-uuid"))
                                 .entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (entry.isSymLink() && entry.symLinkTarget() == device) {
            return QStringLiteral("filesystem-uuid:") + entry.fileName();
        }
    }
#else
    Q_UNUSED(storage);
#endif
    return {};
}

bool sourceSnapshotCurrent(const mixxx::rekordbox::SourceSnapshot& snapshot) {
    const QFileInfo source(snapshot.sourcePath);
    if (snapshot.sourcePath.isEmpty() || !source.isFile() ||
            source.size() != snapshot.sourceFileSize ||
            source.lastModified() != snapshot.sourceModified) {
        return false;
    }
    return snapshot.source.kind != mixxx::rekordbox::SourceKind::Usb ||
            snapshot.source.volumeIdentity.isEmpty() ||
            volumeIdentity(QStorageInfo(snapshot.source.locator)) == snapshot.source.volumeIdentity;
}

qint64 analyzeTimingCorrection(const QString& location) {
    if (location.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive)) {
        switch (mp3guessenc_timing_shift_case(location.toStdString().c_str())) {
#ifdef __COREAUDIO__
        case EXIT_CODE_CASE_A:
            return 12;
        case EXIT_CODE_CASE_B:
            return 13;
        case EXIT_CODE_CASE_C:
            return 26;
        case EXIT_CODE_CASE_D:
            return 50;
#elif defined(__MAD__)
        case EXIT_CODE_CASE_A:
        case EXIT_CODE_CASE_D:
            return 26;
#elif defined(__FFMPEG__)
        case EXIT_CODE_CASE_D:
            return 26;
#endif
        default:
            break;
        }
    }
#ifdef __COREAUDIO__
    if (location.endsWith(QLatin1String(".m4a"), Qt::CaseInsensitive)) {
        return 48;
    }
#endif
    return 0;
}

QVector<mixxx::rekordbox::SourceDescriptor> findRekordboxSources() {
    QVector<mixxx::rekordbox::SourceDescriptor> result;
    QSet<QString> paths;
    for (const auto& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady() || paths.contains(storage.rootPath())) {
            continue;
        }
        paths.insert(storage.rootPath());
        mixxx::rekordbox::SourceDescriptor source;
        source.kind = mixxx::rekordbox::SourceKind::Usb;
        source.locator = storage.rootPath();
        source.displayName = storage.displayName().isEmpty()
                ? storage.rootPath()
                : storage.displayName();
        source.volumeIdentity = volumeIdentity(storage);
        QString error;
        const auto lease = sourceAccess(source, false, &error);
        if (lease && !mixxx::rekordbox::rekordboxPdbPath(source.locator).isEmpty()) {
            result.push_back(source);
        }
    }
    return result;
}

QString issueText(const QVector<mixxx::rekordbox::ImportIssue>& issues) {
    const QString fallback = QObject::tr(
            "The import could not be completed. Refresh the source and try again.");
    QStringList lines;
    for (const auto& issue : issues) {
        QString line = issue.message.isEmpty() ? fallback : issue.message;
        if (!issue.annotationId.isEmpty()) {
            line += QStringLiteral(" [%1]").arg(issue.annotationId);
        }
        if (!lines.contains(line)) {
            lines.append(line);
        }
    }
    return lines.isEmpty() ? fallback : lines.join(QLatin1Char('\n'));
}
// End of ChatGPT-prepared source access helpers.

} // anonymous namespace

namespace mixxx::rekordbox {

void applyXmlTrackAnnotations(
        TrackPointer track,
        audio::SampleRate sampleRate,
        const QByteArray& serializedMetadata) {
    applyXmlMetadata(std::move(track), sampleRate, serializedMetadata);
}

QList<MemoryCueLoop> normalizeMemoryCueLoops(const QList<MemoryCueLoop>& cues) {
    QList<MemoryCueLoop> normalized;
    for (const MemoryCueLoop& cue : cues) {
        if (cue.endPosition.isValid()) {
            bool replaced = false;
            for (MemoryCueLoop& existing : normalized) {
                if (existing.endPosition.isValid() &&
                        existing.startPosition == cue.startPosition &&
                        existing.endPosition == cue.endPosition) {
                    if (cue.fromExtendedSection && !existing.fromExtendedSection) {
                        existing = cue;
                    }
                    replaced = true;
                    break;
                }
            }
            if (replaced) {
                continue;
            }
        } else {
            bool duplicate = false;
            for (const MemoryCueLoop& existing : normalized) {
                if (!existing.endPosition.isValid() &&
                        existing.startPosition == cue.startPosition &&
                        existing.comment == cue.comment &&
                        existing.color == cue.color) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
        }
        normalized << cue;
    }

    std::stable_sort(normalized.begin(),
            normalized.end(),
            [](const MemoryCueLoop& a, const MemoryCueLoop& b) {
                if (a.startPosition != b.startPosition) {
                    return a.startPosition < b.startPosition;
                }
                if (a.endPosition.isValid() != b.endPosition.isValid()) {
                    return !a.endPosition.isValid();
                }
                if (a.endPosition != b.endPosition) {
                    return a.endPosition < b.endPosition;
                }
                if (a.comment != b.comment) {
                    return a.comment < b.comment;
                }
                if (a.color != b.color) {
                    return a.color.has_value();
                }
                return a.sourceOrder < b.sourceOrder;
            });
    return normalized;
}

QString analyzeImportSourceIdentity(
        const QString& trackLocation,
        const QString& analyzePath) {
    if (trackLocation.isEmpty() || analyzePath.isEmpty()) {
        return {};
    }
    return trackLocation + QLatin1Char('\n') + analyzePath;
}

bool shouldImportAnalyzeSource(
        const QString& sourceIdentity,
        const QSet<QString>& importedSources) {
    return !sourceIdentity.isEmpty() && !importedSources.contains(sourceIdentity);
}

void recordXmlTrackMapping(
        QHash<int, int>* trackIds,
        QVector<int>* importedTrackIds,
        int xmlTrackId,
        int databaseTrackId) {
    VERIFY_OR_DEBUG_ASSERT(trackIds && importedTrackIds && databaseTrackId >= 0) {
        return;
    }
    trackIds->insert(xmlTrackId, databaseTrackId);
    importedTrackIds->push_back(xmlTrackId);
}

} // namespace mixxx::rekordbox

RekordboxPlaylistModel::RekordboxPlaylistModel(QObject* parent,
        TrackCollectionManager* trackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource,
        UserSettingsPointer pConfig,
        std::shared_ptr<mixxx::rekordbox::SourceSessions> sources,
        std::shared_ptr<mixxx::rekordbox::ImportSourceRegistry> registry)
        : BaseExternalPlaylistModel(parent,
                  trackCollectionManager,
                  "mixxx.db.model.rekordbox.playlistmodel",
                  kRekordboxPlaylistsTable,
                  kRekordboxPlaylistTracksTable,
                  trackSource),
          m_pConfig(std::move(pConfig)),
          m_sources(std::move(sources)),
          m_registry(std::move(registry)) {
}

void RekordboxPlaylistModel::initSortColumnMapping() {
    // Add a bijective mapping between the SortColumnIds and column indices
    for (int i = 0; i < static_cast<int>(TrackModel::SortColumnId::IdMax); ++i) {
        m_columnIndexBySortColumnId[i] = -1;
    }

    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Artist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Title)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Album)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::AlbumArtist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Year)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_YEAR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Genre)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GENRE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Composer)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMPOSER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GROUPING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TrackNumber)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileType)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::NativeLocation)] =
            fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Comment)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMMENT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Duration)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DURATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::BitRate)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Bpm)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::ReplayGain)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_REPLAYGAIN);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::DateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TimesPlayed)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::LastPlayedAt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_LAST_PLAYED_AT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Rating)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Key)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Preview)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Color)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COLOR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::CoverArt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Position)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::PlaylistDateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_DATETIMEADDED);

    m_sortColumnIdByColumnIndex.clear();
    for (int i = static_cast<int>(TrackModel::SortColumnId::IdMin);
            i < static_cast<int>(TrackModel::SortColumnId::IdMax);
            ++i) {
        TrackModel::SortColumnId sortColumn = static_cast<TrackModel::SortColumnId>(i);
        m_sortColumnIdByColumnIndex.insert(
                m_columnIndexBySortColumnId[static_cast<int>(sortColumn)],
                sortColumn);
    }
}

TrackPointer RekordboxPlaylistModel::getTrack(const QModelIndex& index) const {
    // Prepared by ChatGPT: a modal review may trigger another GUI track request.
    // Let the outer request finish before allowing another canonical application.
    if (m_importInProgress) {
        return {};
    }
    QScopedValueRollback<bool> importing(m_importInProgress, true);
    // End of ChatGPT-prepared reentrancy guard.
    const QString sourceId = getFieldVariant(index, QStringLiteral("source_id")).toString();
    const auto* state = m_sources ? m_sources->get(sourceId) : nullptr;
    if (!state || !state->available || !state->snapshot ||
            state->snapshot->generation != state->generation) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox source"),
                tr("This source is unavailable or changed. Refresh it before "
                   "loading this track."));
        return {};
    }
    return getSourceTrack(index, state->snapshot);
}

// Prepared by ChatGPT: admit one source track atomically before canonical loading.
TrackPointer RekordboxPlaylistModel::getSourceTrack(const QModelIndex& index,
        const std::shared_ptr<const mixxx::rekordbox::SourceSnapshot>& decoded) const {
    using namespace mixxx::rekordbox;
    // Copy the snapshot before any modal dialog can refresh or remove its source.
    const auto snapshot = decoded;
    if (!m_registry || !snapshot ||
            (snapshot->source.kind == SourceKind::Xml && !snapshot->xml)) {
        return {};
    }
    const int trackIndex = getFieldVariant(index, QStringLiteral("source_ordinal")).toInt();
    const QString sourceTrackId =
            getFieldVariant(index, QStringLiteral("source_track_id"))
                    .toString();
    if (trackIndex < 0 || trackIndex >= snapshot->tracks.size() ||
            snapshot->tracks.at(trackIndex).sourceTrackId != sourceTrackId) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox source"),
                tr("This track view is stale. Refresh the source."));
        return {};
    }
    if (!sourceSnapshotCurrent(*snapshot)) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox source"),
                tr("The source file or device changed. Refresh the source "
                   "before loading this track."));
        return {};
    }
    const bool isXml = snapshot->source.kind == SourceKind::Xml;
    if (isXml) {
        const auto admission = validateXmlTrackImport(*snapshot->xml, trackIndex);
        if (admission.outcome != ImportOutcome::Success &&
                admission.outcome != ImportOutcome::Warning) {
            QMessageBox::warning(nullptr,
                    tr("Rekordbox track cannot be imported"),
                    issueText(admission.issues));
            return {};
        }
    }
    const ImportTicket ticket{snapshot->source.id, snapshot->generation};
    ImportPolicy policy;
    policy.revision = isXml ? QStringLiteral("xml-preserve-local-v2")
                            : QStringLiteral("usb-preserve-local-v1");
    const bool promotionRequested = mixxx::library::loadMainCuePromotionPolicy(m_pConfig)
                                            .promoteFirstMemoryCue;
    policy.revision += QStringLiteral(";main-cue=preserve;promotion-requested=%1")
                               .arg(promotionRequested ? 1 : 0);
    const QString location = snapshot->tracks.at(trackIndex).metadata.location;
    if (!QDir::isAbsolutePath(location)) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox media unavailable"),
                tr("The source media location is not a local absolute path: %1")
                        .arg(location));
        return {};
    }
    SourceDescriptor mediaSource{snapshot->source.id, SourceKind::Xml, location, {}, {}};
    QString error;
    const auto mediaLease = sourceAccess(mediaSource, true, &error);
    if (!mediaLease) {
        QMessageBox::warning(nullptr, tr("Rekordbox media unavailable"), error);
        return {};
    }
    const QFileInfo before(location);
    const QString canonical = before.canonicalFilePath();
    if (canonical.isEmpty() || !before.isFile()) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox media unavailable"),
                tr("The audio file cannot be found: %1").arg(location));
        return {};
    }
    const qint64 mediaSize = before.size();
    const QDateTime mediaModified = before.lastModified();
    struct InputState {
        QString path;
        bool exists;
        qint64 size;
        QDateTime modified;
    };
    QVector<InputState> analyzeInputs;
    std::shared_ptr<const AnlzFiles> analyzeFiles;
    const qint64 correction = isXml ? 0 : analyzeTimingCorrection(canonical);
    if (!isXml) {
        const auto& path = snapshot->tracks.at(trackIndex).analyzePath;
        for (const auto& input : QStringList{path,
                     path.left(path.size() - 3) + QStringLiteral("EXT")}) {
            const QFileInfo info(input);
            analyzeInputs.push_back({input, info.exists(), info.size(), info.lastModified()});
        }
        const auto decodedAnalyze = readAnlzFiles(path);
        if (!decodedAnalyze.snapshot) {
            QMessageBox::warning(nullptr,
                    tr("Rekordbox annotations unavailable"),
                    issueText(decodedAnalyze.issues));
            return {};
        }
        analyzeFiles = decodedAnalyze.snapshot;
    }
    const auto cancelled = [this, snapshot, mediaSize, mediaModified, canonical, analyzeInputs] {
        const auto* state = m_sources->get(snapshot->source.id);
        const QFileInfo current(canonical);
        if (!state || !state->available || state->generation != snapshot->generation ||
                !sourceSnapshotCurrent(*snapshot) || !current.isFile() ||
                current.size() != mediaSize || current.lastModified() != mediaModified) {
            return true;
        }
        return std::any_of(analyzeInputs.cbegin(), analyzeInputs.cend(), [](const auto& input) {
            const QFileInfo current(input.path);
            return current.exists() != input.exists || current.size() != input.size ||
                    current.lastModified() != input.modified;
        });
    };
    if (cancelled()) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox source"),
                tr("The source changed. Refresh to retry."));
        return {};
    }
    for (const bool accepted : {false, true}) {
        policy.acceptPreservedConflicts = accepted;
        const auto current = isXml ? currentXmlTrackRevision(*m_registry,
                                             *snapshot->xml,
                                             trackIndex,
                                             sourceTrackId,
                                             ticket,
                                             snapshot->fingerprint,
                                             policy)
                                   : currentAnlzTrackRevision(*m_registry,
                                             *snapshot,
                                             trackIndex,
                                             *analyzeFiles,
                                             policy,
                                             correction);
        if (current) {
            return m_pTrackCollectionManager->getTrackById(TrackId(QVariant::fromValue(*current)));
        }
    }
    XmlMediaFacts media;
    {
        const auto temporary = ::Track::newTemporary(canonical);
        SoundSourceProxy proxy(temporary);
        proxy.updateTrackFromSource(
                SoundSourceProxy::UpdateTrackFromSourceMode::Once,
                SyncTrackMetadataParams{});
        const auto synchronizedAt = temporary->getSourceSynchronizedAt().toUTC();
        if (!synchronizedAt.isValid()) {
            QMessageBox::warning(nullptr,
                    tr("Rekordbox media unavailable"),
                    tr("The audio metadata could not be inspected safely: %1")
                            .arg(location));
            return {};
        }
        const auto audio = proxy.openAudioSource();
        if (!audio) {
            QMessageBox::warning(nullptr,
                    tr("Rekordbox media unavailable"),
                    tr("The audio properties could not be read: %1")
                            .arg(location));
            return {};
        }
        media = {canonical,
                static_cast<int>(temporary->getSampleRate().value()),
                temporary->getChannels().value(),
                temporary->getDuration(),
                temporary->getBitrate(),
                mediaSize,
                temporary->getType(),
                synchronizedAt};
        audio->close();
    }
    struct CacheGuard {
        std::unique_ptr<GlobalTrackCacheLocker> locker;
    };
    auto guard = std::make_shared<CacheGuard>();
    guard->locker = std::make_unique<GlobalTrackCacheLocker>();
    XmlCanonicalAccess access;
    access.lease = guard;
    access.externalCollectionsAbsent = m_pTrackCollectionManager->externalCollections().isEmpty();
    access.check = [guard](qint64 id, const QString& path, QString* message) {
        if (!guard->locker ||
                guard->locker->lookupTrackById(TrackId(QVariant::fromValue(id))) ||
                guard->locker->lookupTrackByRef(TrackRef::fromFilePath(path))) {
            *message = QStringLiteral(
                    "This track is already loaded or cached. Unload it before "
                    "importing a changed source.");
            return false;
        }
        return true;
    };
    XmlPreparedTrack prepared;
    if (isXml) {
        prepared = prepareXmlTrackImport(m_registry->database(),
                snapshot->xml,
                trackIndex,
                sourceTrackId,
                ticket,
                snapshot->fingerprint,
                media,
                access);
    } else {
        auto analyzePrepared = prepareAnlzTrackImport(m_registry->database(),
                snapshot,
                trackIndex,
                analyzeFiles,
                media,
                access,
                correction,
                cancelled);
        prepared.outcome = analyzePrepared.outcome;
        prepared.issues = std::move(analyzePrepared.issues);
        prepared.snapshot = std::move(analyzePrepared.snapshot);
        prepared.target = std::move(analyzePrepared.target);
    }
    if (!prepared.snapshot) {
        guard->locker.reset();
        QMessageBox::warning(nullptr,
                tr("Rekordbox track cannot be imported"),
                issueText(prepared.issues));
        return {};
    }
    if (promotionRequested &&
            std::any_of(prepared.snapshot->fields.cbegin(),
                    prepared.snapshot->fields.cend(),
                    [](const auto& field) {
                        return !field.remove &&
                                field.targetField.startsWith(
                                        QLatin1String("cue:memory:")) &&
                                field.value.toObject()
                                        .value(QStringLiteral("type"))
                                        .toInt() ==
                                static_cast<int>(mixxx::CueType::Memory);
                    })) {
        prepared.snapshot->decodeOutcome = ImportOutcome::Warning;
        prepared.snapshot->decodeIssues.push_back(
                {QStringLiteral("unsupported_main_cue_promotion"),
                        tr("Automatic memory-cue promotion is not supported by "
                           "this import path. "
                           "The existing Main Cue will stay unchanged and "
                           "every memory cue will be retained separately."),
                        sourceTrackId,
                        QStringLiteral("main-cue-promotion")});
    }
    XmlCanonicalAdapter adapter(prepared.target, access);
    policy.acceptPreservedConflicts = true;
    const auto planned = m_registry->plan(*prepared.snapshot, policy, adapter);
    if (!planned.plan) {
        guard->locker.reset();
        QMessageBox::warning(nullptr,
                tr("Rekordbox track cannot be imported"),
                issueText(planned.issues));
        return {};
    }
    if (!planned.plan->losses.isEmpty()) {
        // The sealed plan retains the guard, but never holds the cache mutex while asking.
        guard->locker.reset();
        const auto answer = QMessageBox::warning(nullptr,
                tr("Review Rekordbox import"),
                tr("Some source values cannot be applied or conflict with "
                   "existing Mixxx values. "
                   "Existing local values will be preserved. Continue with "
                   "these limitations?\n\n%1")
                        .arg(issueText(planned.plan->losses)),
                QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Cancel);
        if (answer != QMessageBox::Ok) {
            return {};
        }
        guard->locker = std::make_unique<GlobalTrackCacheLocker>();
    }
    const auto applied = m_registry->apply(*planned.plan, &adapter, cancelled);
    guard->locker.reset();
    if (!applied.committed) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox track was not imported"),
                issueText(applied.issues));
        return {};
    }
    const TrackId trackId(QVariant::fromValue(prepared.target.trackId));
    auto& dao = m_pTrackCollectionManager->internalCollection()->getTrackDAO();
    dao.slotDatabaseTracksChanged({trackId});
    if (prepared.target.createTrack) {
        dao.slotDatabaseTracksAdded({trackId});
    }
    return m_pTrackCollectionManager->getTrackById(trackId);
}
// End of code prepared by ChatGPT for human review and submission.

bool RekordboxPlaylistModel::isColumnHiddenByDefault(int column) {
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE)) {
        return true;
    }
    return BaseSqlTableModel::isColumnHiddenByDefault(column);
}

bool RekordboxPlaylistModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            column == fieldIndex(kRekordboxXmlMetadata) ||
            column == fieldIndex(QStringLiteral("source_id")) ||
            column == fieldIndex(QStringLiteral("source_track_id")) ||
            column == fieldIndex(QStringLiteral("source_ordinal")) ||
            BaseExternalPlaylistModel::isColumnInternal(column);
}

// Prepared by ChatGPT: source-scoped loading and GUI publication.
RekordboxFeature::RekordboxFeature(
        Library* pLibrary, UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("rekordbox")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_sources(std::make_shared<mixxx::rekordbox::SourceSessions>()) {
    QString error;
    QSqlDatabase database = m_pTrackCollection->database();
    if (database.transaction()) {
        QSqlQuery query(database);
        const bool reset =
                query.exec(QStringLiteral(
                        "DROP TABLE IF EXISTS rekordbox_playlist_tracks")) &&
                query.exec(QStringLiteral(
                        "DROP TABLE IF EXISTS rekordbox_playlists")) &&
                query.exec(QStringLiteral(
                        "DROP TABLE IF EXISTS rekordbox_library")) &&
                mixxx::rekordbox::createExternalSourceTables(database, &error);
        if (!reset || !database.commit()) {
            database.rollback();
            qWarning() << "Rekordbox external view initialization failed:"
                       << error << database.lastError();
        }
    }
    m_registry = mixxx::rekordbox::ImportSourceRegistry::attach(database, &error);
    if (!m_registry) {
        qWarning() << "Rekordbox source registry unavailable:" << error;
    }
    const QStringList columns{LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_COLOR,
            LIBRARYTABLE_COVERART,
            REKORDBOX_ANALYZE_PATH,
            kRekordboxXmlMetadata,
            QStringLiteral("source_id"),
            QStringLiteral("source_track_id"),
            QStringLiteral("source_ordinal")};
    const QStringList searchColumns{LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT};
    m_trackSource = QSharedPointer<BaseTrackCache>::create(m_pTrackCollection,
            kRekordboxLibraryTable,
            QString(LIBRARYTABLE_ID),
            columns,
            searchColumns,
            false);
    m_pRekordboxPlaylistModel = make_parented<RekordboxPlaylistModel>(this,
            pLibrary->trackCollectionManager(),
            m_trackSource,
            m_pConfig,
            m_sources,
            m_registry);
    m_title = tr("Rekordbox");
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    const auto stored = QJsonDocument::fromJson(
            m_pConfig
                    ->getValueString(
                            ConfigKey("[Library]", "RekordboxSourcesV2"))
                    .toUtf8())
                                .array();
    for (const auto& value : stored) {
        const auto object = value.toObject();
        mixxx::rekordbox::SourceDescriptor source;
        source.id = object.value(QStringLiteral("id")).toString();
        source.kind = object.value(QStringLiteral("kind")).toString() ==
                        QLatin1String("xml")
                ? mixxx::rekordbox::SourceKind::Xml
                : mixxx::rekordbox::SourceKind::Usb;
        source.locator = object.value(QStringLiteral("locator")).toString();
        source.displayName = object.value(QStringLiteral("name")).toString();
        source.volumeIdentity = object.value(QStringLiteral("volume")).toString();
        if (!QUuid(source.id).isNull() && m_registry && m_registry->source(source.id, &error)) {
            m_sources->add(source, false);
        }
    }
    connect(&m_devicesFutureWatcher,
            &QFutureWatcher<QVector<mixxx::rekordbox::SourceDescriptor>>::finished,
            this,
            &RekordboxFeature::onRekordboxDevicesFound);
    rebuildSidebar();
}

RekordboxFeature::~RekordboxFeature() {
    m_sources->cancelAll();
    m_devicesFuture.waitForFinished();
    for (auto* watcher : std::as_const(m_sourceJobs)) {
        watcher->future().waitForFinished();
    }
}

void RekordboxFeature::bindLibraryWidget(WLibrary* libraryWidget, KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    auto view = make_parented<WLibraryTextBrowser>(libraryWidget);
    m_homeView = view.get();
    view->setHtml(formatRootViewHtml());
    view->setOpenLinks(false);
    connect(view, &WLibraryTextBrowser::anchorClicked, this, &RekordboxFeature::htmlLinkClicked);
    libraryWidget->registerView("REKORDBOXHOME", view);
}

QString RekordboxFeature::addSource(mixxx::rekordbox::SourceKind kind,
        const QString& locator,
        const QString& displayName,
        const QString& identity) {
    if (!m_registry) {
        return {};
    }
    const QString absolute = QDir::cleanPath(QFileInfo(locator).absoluteFilePath());
    const mixxx::rekordbox::SourceDescriptor discovered{{}, kind, absolute, displayName, identity};
    for (const auto& state : m_sources->all()) {
        if (mixxx::rekordbox::matchesDiscoveredSource(state, discovered)) {
            QString error;
            if (state.source.locator != absolute &&
                    !m_registry->relocate(state.source.id, absolute, &error)) {
                return {};
            }
            auto descriptor = state.source;
            descriptor.locator = absolute;
            descriptor.displayName = displayName;
            descriptor.volumeIdentity = identity;
            m_sources->add(descriptor, true);
            persistSources();
            return descriptor.id;
        }
    }
    QString error;
    const auto id =
            m_registry->createSource(kind == mixxx::rekordbox::SourceKind::Xml
                            ? QStringLiteral("rekordbox-xml")
                            : QStringLiteral("rekordbox-usb"),
                    absolute,
                    displayName,
                    &error);
    if (!id) {
        QMessageBox::warning(nullptr, tr("Rekordbox source"), error);
        return {};
    }
    m_sources->add({*id, kind, absolute, displayName, identity}, true);
    persistSources();
    rebuildSidebar();
    return *id;
}

void RekordboxFeature::persistSources() {
    QJsonArray sources;
    for (const auto& state : m_sources->all()) {
        const auto& source = state.source;
        sources.append(QJsonObject{{QStringLiteral("id"), source.id},
                {QStringLiteral("kind"),
                        source.kind == mixxx::rekordbox::SourceKind::Xml
                                ? QStringLiteral("xml")
                                : QStringLiteral("usb")},
                {QStringLiteral("locator"), source.locator},
                {QStringLiteral("name"), source.displayName},
                {QStringLiteral("volume"), source.volumeIdentity}});
    }
    m_pConfig->setValue(ConfigKey("[Library]", "RekordboxSourcesV2"),
            QString::fromUtf8(QJsonDocument(sources).toJson(QJsonDocument::Compact)));
}

void RekordboxFeature::htmlLinkClicked(const QUrl& link) {
    const QString action = link.path();
    if (action == QLatin1String("refresh")) {
        activate();
        return;
    }
    if (action == kRekordboxXmlImportLink || action == QLatin1String("add-usb")) {
        const bool xml = action == kRekordboxXmlImportLink;
        const QString path = xml
                ? QFileDialog::getOpenFileName(nullptr,
                          tr("Select Rekordbox XML collection"),
                          {},
                          tr("Rekordbox XML (*.xml)"))
                : QFileDialog::getExistingDirectory(
                          nullptr, tr("Select Rekordbox USB / SD root"));
        if (path.isEmpty()) {
            return;
        }
        const auto kind = xml ? mixxx::rekordbox::SourceKind::Xml
                              : mixxx::rekordbox::SourceKind::Usb;
        const QString id = addSource(kind,
                path,
                QFileInfo(path).fileName(),
                xml ? QString() : volumeIdentity(QStorageInfo(path)));
        if (!id.isEmpty()) {
            startSource(id);
        }
        return;
    }
    const QString id = action.section(QLatin1Char('/'), 1);
    const auto* state = m_sources->get(id);
    if (!state) {
        return;
    }
    if (action.startsWith(QLatin1String("source-refresh/"))) {
        startSource(id);
    } else if (action.startsWith(QLatin1String("source-remove/"))) {
        const QString label = state->source.displayName;
        if (QMessageBox::question(nullptr,
                    tr("Remove Rekordbox source"),
                    tr("Remove %1 from this browser? Tracks and audio in the "
                       "Mixxx Collection will be kept.")
                            .arg(label),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        m_sources->unavailable(id);
        QString error;
        if (!mixxx::rekordbox::removeExternalSource(m_pTrackCollection->database(), id, &error) ||
                !m_registry || !m_registry->removeSource(id, &error)) {
            QMessageBox::warning(nullptr, tr("Rekordbox source"), error);
        } else {
            m_sources->remove(id);
            persistSources();
        }
        rebuildSidebar();
        updateLoadingState();
    } else if (action.startsWith(QLatin1String("source-relocate/"))) {
        const auto descriptor = state->source;
        const QString path =
                descriptor.kind == mixxx::rekordbox::SourceKind::Xml
                ? QFileDialog::getOpenFileName(nullptr,
                          tr("Locate this Rekordbox XML source"),
                          {},
                          tr("Rekordbox XML (*.xml)"))
                : QFileDialog::getExistingDirectory(
                          nullptr, tr("Locate this Rekordbox USB / SD source"));
        if (path.isEmpty()) {
            return;
        }
        QString error;
        if (m_registry && m_registry->relocate(id, path, &error) && m_sources->relocate(id, path)) {
            persistSources();
            startSource(id);
        } else {
            QMessageBox::warning(nullptr, tr("Rekordbox source"), error);
        }
    }
}

void RekordboxFeature::startSource(const QString& sourceId) {
    const auto* existing = m_sources->get(sourceId);
    if (!existing || !m_registry) {
        return;
    }
    const auto descriptor = existing->source;
    const auto initialGeneration = existing->generation;
    if (descriptor.kind == mixxx::rekordbox::SourceKind::Usb &&
            !existing->available &&
            QMessageBox::question(nullptr,
                    tr("Reconnect Rekordbox source"),
                    tr("Confirm that the device at %2 is the saved source %1. "
                       "Reconnect it? "
                       "Use Add USB / SD source for a different device.")
                            .arg(descriptor.displayName, descriptor.locator),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    QString error;
    const auto lease = sourceAccess(descriptor, true, &error);
    const QString accessError = error;
    // Permission and reconnect dialogs run a nested event loop. A queued discovery
    // result must never pair a different locator with the lease acquired above.
    const auto* current = m_sources->get(sourceId);
    if (!current || current->generation != initialGeneration ||
            current->source.kind != descriptor.kind ||
            current->source.locator != descriptor.locator ||
            current->source.volumeIdentity != descriptor.volumeIdentity) {
        updateLoadingState();
        return;
    }
    const auto ticket = m_registry->requestGeneration(sourceId, &error);
    if (!ticket) {
        QMessageBox::warning(nullptr, tr("Rekordbox source"), error);
        return;
    }
    const auto request = m_sources->begin(sourceId, lease, ticket->generation);
    if (!request) {
        return;
    }
    if (!lease) {
        sourceFinished({*request, mixxx::rekordbox::ImportOutcome::Denied, {accessError}, {}});
        return;
    }
    auto* watcher = new QFutureWatcher<mixxx::rekordbox::SourceResult>(this);
    m_sourceJobs.append(watcher);
    connect(watcher,
            &QFutureWatcher<mixxx::rekordbox::SourceResult>::finished,
            this,
            [this, watcher, request = *request]() {
                mixxx::rekordbox::SourceResult result;
                result.request = request;
                try {
                    result = watcher->result();
                } catch (const std::exception& exception) {
                    result.diagnostics = {QString::fromUtf8(exception.what())};
                } catch (...) {
                    result.diagnostics = {
                            tr("Unexpected source loading failure.")};
                }
                m_sourceJobs.removeAll(watcher);
                sourceFinished(result);
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run(mixxx::rekordbox::decodeSource, *request));
    updateLoadingState();
    rebuildSidebar();
}

void RekordboxFeature::sourceFinished(const mixxx::rekordbox::SourceResult& decoded) {
    if (!m_sources->current(decoded.request)) {
        updateLoadingState();
        return;
    }
    auto result = decoded;
    if (result.snapshot &&
            (result.snapshot->source.id != result.request.source.id ||
                    result.snapshot->generation != result.request.generation)) {
        result.outcome = mixxx::rekordbox::ImportOutcome::Invalid;
        result.diagnostics = {tr("The decoded source did not match the current request.")};
        result.snapshot.reset();
    }
    if (result.snapshot &&
            (result.outcome == mixxx::rekordbox::ImportOutcome::Success ||
                    result.outcome ==
                            mixxx::rekordbox::ImportOutcome::Warning)) {
        QString error;
        if (!mixxx::rekordbox::publishExternalSource(
                    m_pTrackCollection->database(), *result.snapshot, &error)) {
            result.outcome = mixxx::rekordbox::ImportOutcome::Failed;
            result.diagnostics.append(error);
            result.snapshot.reset();
        }
    }
    m_sources->finish(result);
    rebuildSidebar();
    updateLoadingState();
    if (result.snapshot) {
        m_pRekordboxPlaylistModel->setPlaylist(
                mixxx::rekordbox::externalPlaylistKey(
                        result.request.source.id));
        emit showTrackModel(m_pRekordboxPlaylistModel);
    }
    if (!result.diagnostics.isEmpty()) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox source: %1")
                        .arg(result.request.source.displayName),
                result.diagnostics.join(QLatin1Char('\n')));
    }
}

std::unique_ptr<BaseSqlTableModel>
RekordboxFeature::createPlaylistModelForPlaylist(const QVariant& data) {
    const auto values = data.toList();
    if (values.size() < 2 || values[0].toString().isEmpty()) {
        return {};
    }
    auto model = std::make_unique<RekordboxPlaylistModel>(this,
            m_pLibrary->trackCollectionManager(),
            m_trackSource,
            m_pConfig,
            m_sources,
            m_registry);
    model->setPlaylist(values[0].toString());
    return model;
}

QVariant RekordboxFeature::title() {
    return m_title;
}
bool RekordboxFeature::isSupported() {
    return true;
}
TreeItemModel* RekordboxFeature::sidebarModel() const {
    return m_pSidebarModel;
}

QString RekordboxFeature::formatRootViewHtml() const {
    QString html =
            QStringLiteral("<h2>%1</h2><p>%2</p>")
                    .arg(tr("Rekordbox").toHtmlEscaped(),
                            tr("Browse XML collections and legacy USB / SD "
                               "exports. Source files are read-only. "
                               "Selecting or loading a track, or importing a playlist, can "
                               "add supported metadata and annotations to the "
                               "Mixxx Collection; "
                               "local edits are preserved. Memory cues remain "
                               "separate from the Main Cue. Native desktop "
                               "databases and OneLibrary are not supported.")
                                    .toHtmlEscaped());
    html += QStringLiteral(
            "<p><a style=\"color:#0496FF\" href=\"import-xml\">%1</a><br/>"
            "<a style=\"color:#0496FF\" href=\"add-usb\">%2</a><br/>"
            "<a style=\"color:#0496FF\" href=\"refresh\">%3</a></p>")
                    .arg(tr("Add XML collection").toHtmlEscaped(),
                            tr("Add USB / SD source").toHtmlEscaped(),
                            tr("Check attached USB / SD devices")
                                    .toHtmlEscaped());
    html += QStringLiteral("<ul>");
    auto states = m_sources->all();
    std::sort(states.begin(), states.end(), [](const auto& a, const auto& b) {
        return a.source.displayName.localeAwareCompare(b.source.displayName) < 0;
    });
    for (const auto& state : states) {
        html += QStringLiteral(
                "<li><b>%1</b> — %2<br/>%3<br/>"
                "<a href=\"source-refresh/%4\">%5</a> · "
                "<a href=\"source-relocate/%4\">%6</a> · "
                "<a href=\"source-remove/%4\">%7</a></li>")
                        .arg(state.source.displayName.toHtmlEscaped(),
                                state.loading ? tr("Loading").toHtmlEscaped()
                                        : state.available
                                        ? tr("Available").toHtmlEscaped()
                                        : tr("Not loaded / unavailable")
                                                  .toHtmlEscaped(),
                                state.source.locator.toHtmlEscaped(),
                                state.source.id.toHtmlEscaped(),
                                tr("Refresh").toHtmlEscaped(),
                                tr("Locate source").toHtmlEscaped(),
                                tr("Remove").toHtmlEscaped());
    }
    return html + QStringLiteral("</ul>");
}

void RekordboxFeature::rebuildSidebar() {
    clearLastRightClickedIndex();
    auto root = TreeItem::newRoot(this);
    auto states = m_sources->all();
    std::sort(states.begin(), states.end(), [](const auto& a, const auto& b) {
        const int compared = a.source.displayName.localeAwareCompare(b.source.displayName);
        return compared == 0 ? a.source.id < b.source.id : compared < 0;
    });
    for (const auto& state : states) {
        const QString rootKey = mixxx::rekordbox::externalPlaylistKey(state.source.id);
        QString label = state.source.displayName;
        if (state.loading) {
            label += tr(" (loading)");
        } else if (!state.available) {
            label += tr(" (unavailable)");
        }
        auto* item = root->appendChild(label, QVariantList{rootKey, state.source.id, QString()});
        if (!state.snapshot) {
            continue;
        }
        QHash<QString, QVector<const mixxx::rekordbox::ExternalPlaylist*>> children;
        for (const auto& playlist : state.snapshot->playlists) {
            children[playlist.parentId].append(&playlist);
        }
        std::function<void(TreeItem*, const QString&)> append;
        append = [&](TreeItem* parent, const QString& parentId) {
            for (const auto* playlist : children.value(parentId)) {
                auto* child = parent->appendChild(playlist->name,
                        QVariantList{mixxx::rekordbox::externalPlaylistKey(
                                             state.source.id, playlist->id),
                                state.source.id,
                                playlist->id});
                append(child, playlist->id);
            }
        };
        append(item, {});
    }
    m_pSidebarModel->setRootItem(std::move(root));
    if (m_homeView) {
        m_homeView->setHtml(formatRootViewHtml());
    }
}

void RekordboxFeature::updateLoadingState() {
    const bool loading = m_devicesFuture.isRunning() || m_sources->loading();
    m_title = loading ? tr("(loading) Rekordbox") : tr("Rekordbox");
    emit featureIsLoading(this, loading);
    if (!loading) {
        emit featureLoadingFinished(this);
    }
    if (m_homeView) {
        m_homeView->setHtml(formatRootViewHtml());
    }
}

void RekordboxFeature::refreshLibraryModels() {
    rebuildSidebar();
}

void RekordboxFeature::activate() {
    if (!m_devicesFuture.isRunning()) {
        m_devicesFuture = QtConcurrent::run(findRekordboxSources);
        m_devicesFutureWatcher.setFuture(m_devicesFuture);
    }
    updateLoadingState();
    emit enableCoverArtDisplay(true);
    emit switchToView("REKORDBOXHOME");
    emit disableSearch();
}

void RekordboxFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    const auto* item = m_pSidebarModel->getItem(index);
    if (!item) {
        return;
    }
    const auto data = item->getData().toList();
    if (data.size() < 3) {
        return;
    }
    const QString sourceId = data[1].toString();
    const auto* state = m_sources->get(sourceId);
    if (!state) {
        return;
    }
    if (!state->snapshot || !state->available || state->snapshot->generation != state->generation) {
        startSource(sourceId);
        return;
    }
    m_pRekordboxPlaylistModel->setPlaylist(data[0].toString());
    emit showTrackModel(m_pRekordboxPlaylistModel);
}

void RekordboxFeature::onRekordboxDevicesFound() {
    QVector<mixxx::rekordbox::SourceDescriptor> found;
    try {
        found = m_devicesFuture.result();
    } catch (...) {
        QMessageBox::warning(nullptr,
                tr("Rekordbox devices"),
                tr("Device discovery failed. Refresh to retry."));
        updateLoadingState();
        return;
    }
    QVector<QString> present;
    QHash<QString, int> identities;
    for (const auto& source : found) {
        if (!source.volumeIdentity.isEmpty()) {
            ++identities[source.volumeIdentity];
        }
    }
    for (const auto& source : found) {
        bool ambiguousSavedLocation = false;
        for (const auto& state : m_sources->all()) {
            if (state.source.kind == mixxx::rekordbox::SourceKind::Usb &&
                    state.source.locator == source.locator &&
                    (source.volumeIdentity.isEmpty() ||
                            identities.value(source.volumeIdentity) > 1)) {
                present.append(state.source.id);
                m_sources->unavailable(state.source.id);
                QString error;
                if (!mixxx::rekordbox::removeExternalSource(
                            m_pTrackCollection->database(), state.source.id, &error)) {
                    qWarning() << "Could not clear ambiguous source view:" << error;
                }
                ambiguousSavedLocation = true;
                break;
            }
        }
        if (ambiguousSavedLocation) {
            continue;
        }
        const QString identity = identities.value(source.volumeIdentity) == 1
                ? source.volumeIdentity
                : QString();
        const QString id = addSource(source.kind, source.locator, source.displayName, identity);
        if (!id.isEmpty()) {
            present.append(id);
        }
    }
    QString error;
    for (const auto& id : m_sources->missingUsbSources(present)) {
        m_sources->unavailable(id);
        if (!mixxx::rekordbox::removeExternalSource(m_pTrackCollection->database(), id, &error)) {
            qWarning() << "Could not clear unavailable source view:" << error;
        }
    }
    rebuildSidebar();
    updateLoadingState();
}
// End of code prepared by ChatGPT for human review and submission.
