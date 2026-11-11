#include "library/dao/cuedao.h"

#include <QSet>
#include <QThread>
#include <QVariant>
#include <QtDebug>

#include "engine/engine.h"
#include "library/queryutil.h"
#include "util/assert.h"
#include "util/color/rgbcolor.h"
#include "util/compatibility/qmutex.h"
#include "util/db/fwdsqlquery.h"
#include "util/db/sqltransaction.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger = mixxx::Logger("CueDAO");

/// Wrap a `QString` label in a `QVariant`. The label column is not nullable,
/// so this function also makes sure that the label an empty string, not null.
inline const QVariant labelToQVariant(const QString& label) {
    if (label.isNull()) {
        return QLatin1String(""); // null -> empty
    } else {
        return label;
    }
}

/// Empty labels are read as null strings
inline QString labelFromQVariant(const QVariant& value) {
    const auto label = value.toString();
    if (label.isEmpty()) {
        return QString(); // empty -> null
    } else {
        return label;
    }
}

CuePointer cueFromRow(const QSqlRecord& row) {
    const auto id = DbId(row.value(row.indexOf("id")));
    TrackId trackId(row.value(row.indexOf("track_id")));
    auto type = static_cast<mixxx::CueType>(row.value(row.indexOf("type")).toInt());
    const auto position =
            mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                    row.value(row.indexOf("position")).toDouble());
    double lengthFrames = row.value(row.indexOf("length")).toDouble() /
            mixxx::kEngineChannelOutputCount;
    int hotcue = row.value(row.indexOf("hotcue")).toInt();
    QString label = labelFromQVariant(row.value(row.indexOf("label")));
    mixxx::RgbColor::optional_t color = mixxx::RgbColor::fromQVariant(row.value(row.indexOf("color")));
    VERIFY_OR_DEBUG_ASSERT(color) {
        return CuePointer();
    }
    if (type == mixxx::CueType::Loop && lengthFrames == 0.0) {
        // These entries are likely added via issue #11283
        qWarning() << "Discard loop cue" << hotcue << "found in database with length of 0";
        return CuePointer();
    }
    CuePointer pCue(new Cue(id,
            type,
            position,
            lengthFrames,
            hotcue,
            label,
            *color));
    return pCue;
}

} // namespace

QList<CuePointer> CueDAO::getCuesForTrack(TrackId trackId) const {
    //qDebug() << "CueDAO::getCuesForTrack" << QThread::currentThread() << m_database.connectionName();
    QList<CuePointer> cues;

    FwdSqlQuery query(
            m_database,
            QStringLiteral("SELECT * FROM " CUE_TABLE " WHERE track_id=:id"));
    DEBUG_ASSERT(
            query.isPrepared() &&
            !query.hasError());
    query.bindValue(":id", trackId);
    if (!query.execPrepared()) {
        kLogger.warning()
                << "Failed to load cues of track"
                << trackId;
        DEBUG_ASSERT(!"failed query");
        return cues;
    }
    QMap<int, CuePointer> hotCuesByNumber;
    while (query.next()) {
        CuePointer pCue = cueFromRow(query.record());
        if (!pCue) {
            continue;
        }
        int hotCueNumber = pCue->getHotCue();
        if (hotCueNumber != Cue::kNoHotCue) {
            const auto pDuplicateCue = hotCuesByNumber.take(hotCueNumber);
            if (pDuplicateCue) {
                kLogger.warning()
                        << "Dropping hot cue"
                        << pDuplicateCue->getId()
                        << "with duplicate number"
                        << hotCueNumber;
                cues.removeOne(pDuplicateCue);
            }
            hotCuesByNumber.insert(hotCueNumber, pCue);
        }
        cues.push_back(pCue);
    }
    return cues;
}

bool CueDAO::deleteCuesForTrack(TrackId trackId) const {
    qDebug() << "CueDAO::deleteCuesForTrack" << QThread::currentThread() << m_database.connectionName();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM " CUE_TABLE " WHERE track_id=:track_id"));
    query.bindValue(":track_id", trackId.toVariant());
    if (query.exec()) {
        return true;
    } else {
        LOG_FAILED_QUERY(query);
    }
    return false;
}

bool CueDAO::deleteCuesForTracks(const QList<TrackId>& trackIds) const {
    qDebug() << "CueDAO::deleteCuesForTracks" << QThread::currentThread() << m_database.connectionName();

    QStringList idList;
    for (const auto& trackId: trackIds) {
        idList << trackId.toString();
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM " CUE_TABLE " WHERE track_id in (%1)")
                  .arg(idList.join(",")));
    if (query.exec()) {
        return true;
    } else {
        LOG_FAILED_QUERY(query);
    }
    return false;
}

// Prepared by ChatGPT for human review.
CueDAO::CueSnapshot CueDAO::snapshotCue(const Cue& cue) {
    const auto lock = lockMutex(&cue.m_mutex);
    return {cue.m_dbId,
            cue.m_type,
            cue.m_startPosition,
            cue.m_endPosition,
            cue.m_iHotCue,
            cue.m_label,
            cue.m_color,
            cue.m_bDirty};
}

bool CueDAO::finalizeCue(const PendingSave::SavedCue& saved) {
    const auto lock = lockMutex(&saved.cue->m_mutex);
    auto& cue = *saved.cue;
    const auto& snapshot = saved.snapshot;
    if (cue.m_dbId != snapshot.id && cue.m_dbId != saved.savedId) {
        cue.m_bDirty = true;
        return false;
    }
    cue.m_dbId = saved.savedId;
    const bool unchanged = cue.m_type == snapshot.type &&
            cue.m_startPosition == snapshot.start && cue.m_endPosition == snapshot.end &&
            cue.m_iHotCue == snapshot.hotCue && cue.m_label == snapshot.label &&
            cue.m_color == snapshot.color;
    cue.m_bDirty = !unchanged;
    return unchanged;
}

bool CueDAO::PendingSave::finalize() {
    bool unchanged = true;
    for (const auto& saved : m_savedCues) {
        unchanged = CueDAO::finalizeCue(saved) && unchanged;
    }
    m_savedCues.clear();
    return unchanged;
}

std::optional<DbId> CueDAO::writeCue(TrackId trackId, const Cue& cue) const {
    return writeCueSnapshot(trackId, snapshotCue(cue));
}

std::optional<DbId> CueDAO::writeCueSnapshot(
        TrackId trackId, const CueSnapshot& snapshot) const {
    if (!trackId.isValid()) {
        return std::nullopt;
    }
    QSqlQuery query(m_database);
    if (snapshot.id.isValid()) {
        query.prepare(QStringLiteral(
                "UPDATE " CUE_TABLE
                " SET "
                "type=:type,position=:position,length=:length,hotcue=:hotcue,"
                "label=:label,color=:color WHERE id=:id AND "
                "track_id=:track_id"));
        query.bindValue(":id", snapshot.id.toVariant());
    } else {
        query.prepare(QStringLiteral("INSERT INTO " CUE_TABLE
                                     " (track_id,type,position,length,hotcue,label,color) VALUES "
                                     "(:track_id,:type,:position,:length,:hotcue,:label,:color)"));
    }
    const auto length = snapshot.end.isValid()
            ? (snapshot.start.isValid() ? snapshot.end - snapshot.start : snapshot.end.value())
            : 0;
    query.bindValue(":track_id", trackId.toVariant());
    query.bindValue(":type", static_cast<int>(snapshot.type));
    query.bindValue(":position", snapshot.start.toEngineSamplePosMaybeInvalid());
    query.bindValue(":length", length * mixxx::kEngineChannelOutputCount);
    query.bindValue(":hotcue", snapshot.hotCue);
    query.bindValue(":label", labelToQVariant(snapshot.label));
    query.bindValue(":color", mixxx::RgbColor::toQVariant(snapshot.color));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return std::nullopt;
    }
    if (query.numRowsAffected() != 1) {
        kLogger.warning() << "Cue write did not affect exactly one row for track" << trackId;
        return std::nullopt;
    }
    const auto savedId = snapshot.id.isValid() ? snapshot.id : DbId(query.lastInsertId());
    if (!savedId.isValid()) {
        return std::nullopt;
    }
    return savedId;
}

std::optional<CueDAO::PendingSave> CueDAO::snapshotCues(
        const QList<CuePointer>& cueList) {
    PendingSave pending;
    pending.m_savedCues.reserve(cueList.size());
    QSet<const Cue*> pointers;
    QSet<DbId> existingIds;
    for (const auto& cue : cueList) {
        if (!cue || pointers.contains(cue.get())) {
            return std::nullopt;
        }
        pointers.insert(cue.get());
        auto snapshot = snapshotCue(*cue);
        if (snapshot.id.isValid() && existingIds.contains(snapshot.id)) {
            return std::nullopt;
        }
        existingIds.insert(snapshot.id);
        pending.m_savedCues.push_back({cue, std::move(snapshot), DbId()});
    }
    return pending;
}

bool CueDAO::writeTrackCues(TrackId trackId, PendingSave* pending) const {
    if (!trackId.isValid() || !pending) {
        return false;
    }
    QStringList cueIds;
    cueIds.reserve(pending->m_savedCues.size());
    for (auto& saved : pending->m_savedCues) {
        if (saved.snapshot.dirty || !saved.snapshot.id.isValid()) {
            const auto id = writeCueSnapshot(trackId, saved.snapshot);
            if (!id) {
                return false;
            }
            saved.savedId = *id;
        } else {
            QSqlQuery query(m_database);
            query.prepare(QStringLiteral("SELECT id FROM " CUE_TABLE
                                         " WHERE id=:id AND track_id=:track_id"));
            query.bindValue(":id", saved.snapshot.id.toVariant());
            query.bindValue(":track_id", trackId.toVariant());
            if (!query.exec() || !query.next()) {
                kLogger.warning() << "Clean cue is missing from track" << trackId;
                return false;
            }
            saved.savedId = saved.snapshot.id;
        }
        cueIds.append(saved.savedId.toString());
    }
    FwdSqlQuery query(m_database,
            QStringLiteral("DELETE FROM " CUE_TABLE " WHERE track_id=:track_id AND id NOT IN (%1)")
                    .arg(cueIds.join(QChar(','))));
    query.bindValue(":track_id", trackId);
    if (!query.isPrepared() || query.hasError() || !query.execPrepared()) {
        kLogger.warning() << "Failed to delete orphaned cues of track" << trackId;
        return false;
    }
    return true;
}

std::optional<CueDAO::PendingSave> CueDAO::prepareTrackCues(
        TrackId trackId, const QList<CuePointer>& cueList) const {
    auto pending = snapshotCues(cueList);
    if (!pending || !writeTrackCues(trackId, &*pending)) {
        return std::nullopt;
    }
    return pending;
}

bool CueDAO::saveTrackCues(TrackId trackId, const QList<CuePointer>& cueList) const {
    SqlTransaction transaction(m_database);
    if (!transaction) {
        return false;
    }
    auto pending = prepareTrackCues(trackId, cueList);
    if (!pending || !transaction.commit()) {
        return false;
    }
    pending->finalize();
    return true;
}
// End of ChatGPT-prepared code for human review.
