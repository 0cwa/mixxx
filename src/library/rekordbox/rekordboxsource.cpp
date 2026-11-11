// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxsource.h"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <utility>

namespace mixxx::rekordbox {
namespace {
bool query(QSqlDatabase database,
        const QString& statement,
        const QVariantList& arguments,
        QString* error) {
    QSqlQuery query(database);
    if (!query.prepare(statement)) {
        *error = query.lastError().text();
        return false;
    }
    for (const auto& argument : arguments) {
        query.addBindValue(argument);
    }
    if (!query.exec()) {
        *error = query.lastError().text();
        return false;
    }
    return true;
}

bool deleteSourceRows(QSqlDatabase database, const QString& sourceId, QString* error) {
    return query(database,
                   QStringLiteral("DELETE FROM rekordbox_playlist_tracks WHERE "
                                  "playlist_id IN "
                                  "(SELECT id FROM rekordbox_playlists WHERE "
                                  "source_id=?)"),
                   {sourceId},
                   error) &&
            query(database,
                    QStringLiteral("DELETE FROM rekordbox_playlists WHERE "
                                   "source_id=?"),
                    {sourceId},
                    error) &&
            query(database,
                    QStringLiteral(
                            "DELETE FROM rekordbox_library WHERE source_id=?"),
                    {sourceId},
                    error);
}
} // namespace

bool matchesDiscoveredSource(const SourceSession& existing, const SourceDescriptor& discovered) {
    if (existing.source.kind != discovered.kind) {
        return false;
    }
    if (discovered.kind == SourceKind::Xml) {
        return existing.source.locator == discovered.locator;
    }
    if (!existing.source.volumeIdentity.isEmpty() || !discovered.volumeIdentity.isEmpty()) {
        return !discovered.volumeIdentity.isEmpty() &&
                existing.source.volumeIdentity == discovered.volumeIdentity;
    }
    return false;
}

bool SourceSessions::add(const SourceDescriptor& source, bool available) {
    if (source.id.isEmpty() || source.locator.isEmpty()) {
        return false;
    }
    auto it = m_sources.find(source.id);
    if (it != m_sources.end()) {
        if (it->source.kind != source.kind) {
            return false;
        }
        if (it->source.locator != source.locator) {
            relocate(source.id, source.locator);
        }
        it->source = source;
        it->available = available;
        return true;
    }
    SourceSession session;
    session.source = source;
    session.available = available;
    m_sources.insert(source.id, std::move(session));
    return true;
}

std::optional<SourceRequest> SourceSessions::begin(
        const QString& sourceId, std::shared_ptr<const void> accessLease, qint64 generation) {
    auto it = m_sources.find(sourceId);
    if (it == m_sources.end()) {
        return std::nullopt;
    }
    if (it->cancelled) {
        it->cancelled->store(true, std::memory_order_relaxed);
    }
    it->generation = generation > 0 ? generation : it->generation + 1;
    it->cancelled = std::make_shared<std::atomic_bool>(false);
    it->available = true;
    it->loading = true;
    it->diagnostics.clear();
    return SourceRequest{it->source, it->generation, it->cancelled, std::move(accessLease)};
}

bool SourceSessions::current(const SourceRequest& request) const {
    const auto it = m_sources.constFind(request.source.id);
    return it != m_sources.cend() && it->available &&
            it->generation == request.generation &&
            it->source.locator == request.source.locator &&
            it->cancelled == request.cancelled && !request.isCancelled();
}

bool SourceSessions::finish(const SourceResult& result) {
    if (!current(result.request)) {
        return false;
    }
    auto& state = m_sources[result.request.source.id];
    state.loading = false;
    state.lastOutcome = result.outcome;
    state.diagnostics = result.diagnostics;
    if ((result.outcome == ImportOutcome::Success || result.outcome == ImportOutcome::Warning) &&
            result.snapshot && result.snapshot->source.id == state.source.id &&
            result.snapshot->generation == state.generation) {
        state.snapshot = result.snapshot;
    } else if (result.outcome == ImportOutcome::Success ||
            result.outcome == ImportOutcome::Warning) {
        state.lastOutcome = ImportOutcome::Invalid;
        state.diagnostics.append(
                QStringLiteral("Decoded source identity or generation did not "
                               "match the request."));
    }
    return true;
}

bool SourceSessions::relocate(const QString& sourceId, const QString& locator) {
    auto it = m_sources.find(sourceId);
    if (it == m_sources.end() || locator.isEmpty()) {
        return false;
    }
    unavailable(sourceId);
    it->source.locator = locator;
    return true;
}

void SourceSessions::unavailable(const QString& sourceId) {
    auto it = m_sources.find(sourceId);
    if (it == m_sources.end()) {
        return;
    }
    if (it->cancelled) {
        it->cancelled->store(true, std::memory_order_relaxed);
    }
    ++it->generation;
    it->available = false;
    it->loading = false;
    it->lastOutcome = ImportOutcome::Unavailable;
}

void SourceSessions::remove(const QString& sourceId) {
    unavailable(sourceId);
    m_sources.remove(sourceId);
}

void SourceSessions::cancelAll() {
    for (auto it = m_sources.begin(); it != m_sources.end(); ++it) {
        if (it->cancelled) {
            it->cancelled->store(true, std::memory_order_relaxed);
        }
        ++it->generation;
        it->loading = false;
    }
}

const SourceSession* SourceSessions::get(const QString& sourceId) const {
    const auto it = m_sources.constFind(sourceId);
    return it == m_sources.cend() ? nullptr : &it.value();
}

QVector<SourceSession> SourceSessions::all() const {
    QVector<SourceSession> result;
    result.reserve(m_sources.size());
    for (const auto& state : m_sources) {
        result.push_back(state);
    }
    return result;
}

QVector<QString> SourceSessions::missingUsbSources(const QVector<QString>& presentIds) const {
    const QSet<QString> present(presentIds.cbegin(), presentIds.cend());
    QVector<QString> result;
    for (const auto& state : m_sources) {
        if (state.source.kind == SourceKind::Usb && state.available &&
                !present.contains(state.source.id)) {
            result.push_back(state.source.id);
        }
    }
    return result;
}

bool SourceSessions::loading() const {
    for (const auto& state : m_sources) {
        if (state.loading) {
            return true;
        }
    }
    return false;
}

QString externalPlaylistKey(const QString& sourceId, const QString& playlistId) {
    return QUuid::createUuidV5(QUuid(sourceId),
            (playlistId.isEmpty()
                            ? QByteArray("collection")
                            : QByteArray("playlist:") + playlistId.toUtf8()))
            .toString(QUuid::WithoutBraces);
}

bool createExternalSourceTables(QSqlDatabase database, QString* error) {
    return query(database,
                   QStringLiteral(
                           "CREATE TABLE IF NOT EXISTS rekordbox_library ("
                           "id INTEGER PRIMARY KEY AUTOINCREMENT, rb_id "
                           "INTEGER, "
                           "artist TEXT, title TEXT, album TEXT, year INTEGER, "
                           "genre TEXT, "
                           "tracknumber TEXT, location TEXT, comment TEXT, "
                           "duration REAL, "
                           "bitrate TEXT, bpm REAL, key TEXT, key_id INTEGER, "
                           "rating INTEGER, "
                           "analyze_path TEXT, device TEXT, color INTEGER, "
                           "coverart TEXT, xml_metadata TEXT, "
                           "source_id TEXT NOT NULL, source_track_id TEXT NOT "
                           "NULL, source_ordinal INTEGER, "
                           "UNIQUE(source_id,source_track_id))"),
                   {},
                   error) &&
            query(database,
                    QStringLiteral(
                            "CREATE TABLE IF NOT EXISTS rekordbox_playlists ("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT "
                            "UNIQUE, source_id TEXT NOT NULL, "
                            "source_playlist_id TEXT NOT NULL, parent_id TEXT, "
                            "label TEXT, is_folder INTEGER)"),
                    {},
                    error) &&
            query(database,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS "
                                   "rekordbox_playlist_tracks ("
                                   "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                   "playlist_id INTEGER, "
                                   "track_id INTEGER, position INTEGER)"),
                    {},
                    error) &&
            query(database,
                    QStringLiteral("CREATE INDEX IF NOT EXISTS "
                                   "rekordbox_playlist_source "
                                   "ON rekordbox_playlists(source_id)"),
                    {},
                    error) &&
            query(database,
                    QStringLiteral(
                            "CREATE INDEX IF NOT EXISTS "
                            "rekordbox_membership_playlist "
                            "ON "
                            "rekordbox_playlist_tracks(playlist_id,position)"),
                    {},
                    error);
}

bool publishExternalSource(QSqlDatabase database,
        const SourceSnapshot& snapshot,
        QString* error) {
    if (QUuid(snapshot.source.id).isNull() || snapshot.fingerprint.isEmpty()) {
        *error = QStringLiteral("Missing source identity or snapshot fingerprint.");
        return false;
    }
    QSet<QString> trackKeys;
    for (const auto& track : snapshot.tracks) {
        if (track.sourceTrackId.isEmpty() || trackKeys.contains(track.sourceTrackId)) {
            *error = QStringLiteral("Duplicate or missing source track identity.");
            return false;
        }
        trackKeys.insert(track.sourceTrackId);
    }
    QSet<QString> playlistKeys;
    for (const auto& playlist : snapshot.playlists) {
        if (playlist.id.isEmpty() || playlistKeys.contains(playlist.id)) {
            *error = QStringLiteral("Duplicate or missing playlist identity.");
            return false;
        }
        playlistKeys.insert(playlist.id);
        for (const auto& member : playlist.trackIds) {
            if (!trackKeys.contains(member)) {
                *error = QStringLiteral("Unresolved source playlist member.");
                return false;
            }
        }
    }
    for (const auto& playlist : snapshot.playlists) {
        if (!playlist.parentId.isEmpty() && !playlistKeys.contains(playlist.parentId)) {
            *error = QStringLiteral("Unresolved playlist parent.");
            return false;
        }
        QSet<QString> ancestors{playlist.id};
        QString parent = playlist.parentId;
        while (!parent.isEmpty()) {
            if (ancestors.contains(parent)) {
                *error = QStringLiteral("Cyclic playlist hierarchy.");
                return false;
            }
            ancestors.insert(parent);
            for (const auto& candidate : snapshot.playlists) {
                if (candidate.id == parent) {
                    parent = candidate.parentId;
                    break;
                }
            }
        }
    }
    if (!database.transaction()) {
        *error = database.lastError().text();
        return false;
    }
    const auto fail = [&]() {
        database.rollback();
        return false;
    };
    if (!deleteSourceRows(database, snapshot.source.id, error)) {
        return fail();
    }
    QHash<QString, qint64> trackRows;
    for (const auto& external : snapshot.tracks) {
        const auto& track = external.metadata;
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
                "INSERT INTO rekordbox_library "
                "(rb_id,artist,title,album,year,genre,tracknumber,location,comment,duration,"
                "bitrate,bpm,key,rating,analyze_path,device,color,coverart,xml_metadata,"
                "source_id,source_track_id,source_ordinal) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
        const QVariantList values{track.id,
                track.artist,
                track.title,
                track.album,
                track.year,
                track.genre,
                track.trackNumber,
                track.location,
                track.comment,
                track.durationSeconds,
                track.bitrate,
                track.bpm,
                track.key,
                track.rating,
                external.analyzePath,
                snapshot.source.id,
                external.color ? QVariant(*external.color) : QVariant(),
                track.artworkReference,
                snapshot.source.kind == SourceKind::Xml
                        ? QVariant(serializeTrackAnnotations(track))
                        : QVariant(),
                snapshot.source.id,
                external.sourceTrackId,
                track.sourceOrdinal};
        for (const auto& value : values) {
            insert.addBindValue(value);
        }
        if (!insert.exec()) {
            *error = insert.lastError().text();
            return fail();
        }
        trackRows.insert(external.sourceTrackId, insert.lastInsertId().toLongLong());
    }
    QVector<ExternalPlaylist> playlists;
    ExternalPlaylist collection;
    collection.name = snapshot.source.displayName;
    collection.folder = true;
    for (const auto& track : snapshot.tracks) {
        collection.trackIds.push_back(track.sourceTrackId);
    }
    playlists.push_back(std::move(collection));
    playlists += snapshot.playlists;
    for (const auto& playlist : playlists) {
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
                "INSERT INTO rekordbox_playlists "
                "(name,source_id,source_playlist_id,parent_id,label,is_folder) "
                "VALUES (?,?,?,?,?,?)"));
        insert.addBindValue(externalPlaylistKey(snapshot.source.id, playlist.id));
        insert.addBindValue(snapshot.source.id);
        insert.addBindValue(playlist.id.isEmpty() ? QStringLiteral("collection") : playlist.id);
        insert.addBindValue(playlist.parentId);
        insert.addBindValue(playlist.name);
        insert.addBindValue(playlist.folder);
        if (!insert.exec()) {
            *error = insert.lastError().text();
            return fail();
        }
        const qint64 playlistRow = insert.lastInsertId().toLongLong();
        int position = 1;
        for (const auto& member : playlist.trackIds) {
            if (!query(database, QStringLiteral("INSERT INTO rekordbox_playlist_tracks "
                                                "(playlist_id,track_id,position) VALUES (?,?,?)"),
                        {playlistRow, trackRows.value(member), position++},
                        error)) {
                return fail();
            }
        }
    }
    if (!database.commit()) {
        *error = database.lastError().text();
        return fail();
    }
    return true;
}

bool removeExternalSource(QSqlDatabase database, const QString& sourceId, QString* error) {
    if (!database.transaction()) {
        *error = database.lastError().text();
        return false;
    }
    if (!deleteSourceRows(database, sourceId, error)) {
        database.rollback();
        return false;
    }
    if (!database.commit()) {
        *error = database.lastError().text();
        database.rollback();
        return false;
    }
    return true;
}
} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
