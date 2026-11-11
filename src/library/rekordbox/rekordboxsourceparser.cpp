// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxsourceparser.h"

#include <rekordbox_pdb.h>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace mixxx::rekordbox {
namespace {
constexpr qint64 kMaximumSourceBytes = 256 * 1024 * 1024;

QString pdbText(rekordbox_pdb_t::device_sql_string_t* value) {
    if (!value || !value->body()) {
        return {};
    }
    QString text;
    if (const auto* body =
                    dynamic_cast<rekordbox_pdb_t::device_sql_short_ascii_t*>(
                            value->body())) {
        text = QString::fromStdString(body->text());
    } else if (const auto* body =
                       dynamic_cast<rekordbox_pdb_t::device_sql_long_ascii_t*>(
                               value->body())) {
        text = QString::fromStdString(body->text());
    } else if (const auto* body = dynamic_cast<
                       rekordbox_pdb_t::device_sql_long_utf16le_t*>(
                       value->body())) {
        const auto raw = body->text();
        if (raw.size() % 2 != 0) {
            throw std::runtime_error("Invalid UTF-16 PDB string length");
        }
        QVector<char16_t> utf16;
        utf16.reserve(static_cast<int>(raw.size() / 2));
        for (size_t i = 0; i < raw.size(); i += 2) {
            utf16.append(static_cast<char16_t>(qFromLittleEndian<quint16>(raw.data() + i)));
        }
        text = QString::fromUtf16(utf16.constData(), utf16.size());
    }
    return text.remove(QChar::Null);
}

QString volumePath(const QString& root, QString relative) {
    while (relative.startsWith(QLatin1Char('/'))) {
        relative.remove(0, 1);
    }
    if (relative.isEmpty()) {
        return {};
    }
    const QString base = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    const QString path = QDir::cleanPath(QDir(base).absoluteFilePath(relative));
    const QString prefix = base.endsWith(QLatin1Char('/')) ? base : base + QLatin1Char('/');
    if (!path.startsWith(prefix)) {
        throw std::runtime_error("PDB path escapes the selected device root");
    }
    return path;
}

std::optional<quint32> pdbColor(uint32_t value) {
    static constexpr quint32 colors[] = {0,
            0xF870F8,
            0xF87090,
            0xF8A030,
            0xF8E331,
            0x1EE000,
            0x16C0F8,
            0x0150F8,
            0x9808F8};
    if (value == 0 || value >= std::size(colors)) {
        return std::nullopt;
    }
    return colors[value];
}

bool cancelled(const SourceRequest& request, SourceResult* result) {
    if (!request.isCancelled()) {
        return false;
    }
    result->outcome = ImportOutcome::Cancelled;
    result->snapshot.reset();
    result->diagnostics = {QStringLiteral("Source loading was cancelled.")};
    return true;
}

SourceResult decodeXmlSource(const SourceRequest& request, const QByteArray& bytes) {
    SourceResult result;
    result.request = request;
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    const auto decoded = decodeXml(&buffer);
    for (const auto& issue : decoded.diagnostics) {
        result.diagnostics.append(QStringLiteral("%1 (%2:%3)")
                        .arg(issue.message)
                        .arg(issue.line)
                        .arg(issue.column));
    }
    if (!decoded.snapshot) {
        result.outcome = decoded.outcome == XmlOutcome::Unsupported
                ? ImportOutcome::Unsupported
                : ImportOutcome::Invalid;
        return result;
    }
    auto snapshot = std::make_shared<SourceSnapshot>();
    snapshot->source = request.source;
    snapshot->generation = request.generation;
    snapshot->accessLease = request.accessLease;
    snapshot->fingerprint = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                    .toHex());
    snapshot->xml = decoded.snapshot;
    QHash<int, int> idCounts;
    QHash<QString, int> locationCounts;
    for (const auto& track : decoded.snapshot->tracks) {
        if (track.hasValidId) {
            ++idCounts[track.id];
        }
        if (!track.location.isEmpty()) {
            ++locationCounts[track.location];
        }
    }
    QVector<QString> sourceKeys;
    sourceKeys.reserve(decoded.snapshot->tracks.size());
    for (int i = 0; i < decoded.snapshot->tracks.size(); ++i) {
        if (cancelled(request, &result)) {
            return result;
        }
        ExternalTrack track;
        track.metadata = decoded.snapshot->tracks.at(i);
        if (track.metadata.hasValidId && idCounts.value(track.metadata.id) == 1) {
            track.sourceTrackId = QStringLiteral("id:%1").arg(track.metadata.id);
        } else if (!track.metadata.hasValidId && !track.metadata.location.isEmpty() &&
                locationCounts.value(track.metadata.location) == 1) {
            track.sourceTrackId = QStringLiteral("location:") +
                    QString::fromLatin1(QCryptographicHash::hash(
                            track.metadata.location.toUtf8(),
                            QCryptographicHash::Sha256)
                                    .toHex());
        } else {
            track.sourceTrackId = QStringLiteral("record:%1").arg(i);
        }
        snapshot->tracks.push_back(std::move(track));
        sourceKeys.push_back(snapshot->tracks.back().sourceTrackId);
    }
    std::function<void(const Playlist&, const QString&)> append;
    append = [&](const Playlist& playlist, const QString& parent) {
        ExternalPlaylist record;
        record.id = playlist.id;
        record.parentId = parent;
        record.name = playlist.name;
        record.folder = playlist.type == 0;
        for (const auto& entry : playlist.entries) {
            if (entry.trackIndex && *entry.trackIndex >= 0 &&
                    *entry.trackIndex < sourceKeys.size()) {
                record.trackIds.push_back(sourceKeys.at(*entry.trackIndex));
            }
        }
        snapshot->playlists.push_back(std::move(record));
        for (const auto& child : playlist.children) {
            append(child, playlist.id);
        }
    };
    for (const auto& playlist : decoded.snapshot->playlists.children) {
        append(playlist, {});
    }
    if (cancelled(request, &result)) {
        return result;
    }
    result.outcome = result.diagnostics.isEmpty() ? ImportOutcome::Success : ImportOutcome::Warning;
    result.snapshot = std::move(snapshot);
    return result;
}

SourceResult decodeUsbSource(const SourceRequest& request, const QByteArray& bytes) {
    SourceResult result;
    result.request = request;
    if (bytes.size() < 28) {
        throw std::runtime_error("Truncated PDB header");
    }
    const quint32 pageSize = qFromLittleEndian<quint32>(bytes.constData() + 4);
    const quint32 tableCount = qFromLittleEndian<quint32>(bytes.constData() + 8);
    if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1)) != 0 ||
            tableCount > 64 || 28ULL + 16ULL * tableCount > static_cast<quint64>(bytes.size())) {
        throw std::runtime_error("Unsupported or invalid PDB header limits");
    }
    std::istringstream stream(
            std::string(bytes.constData(), static_cast<size_t>(bytes.size())),
            std::ios::binary);
    kaitai::kstream input(&stream);
    rekordbox_pdb_t pdb(&input);
    auto snapshot = std::make_shared<SourceSnapshot>();
    snapshot->source = request.source;
    snapshot->generation = request.generation;
    snapshot->accessLease = request.accessLease;
    snapshot->fingerprint = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                    .toHex());
    QHash<uint32_t, QString> keys, genres, artists, albums, labels, artworks;
    QMap<uint32_t, ExternalPlaylist> playlists;
    QHash<uint32_t, QMap<uint32_t, uint32_t>> memberships;
    QHash<uint32_t, uint32_t> playlistOrder;
    QSet<QString> trackKeys;
    const QVector<rekordbox_pdb_t::page_type_t> types{
            rekordbox_pdb_t::PAGE_TYPE_KEYS,
            rekordbox_pdb_t::PAGE_TYPE_GENRES,
            rekordbox_pdb_t::PAGE_TYPE_ARTISTS,
            rekordbox_pdb_t::PAGE_TYPE_ALBUMS,
            rekordbox_pdb_t::PAGE_TYPE_LABELS,
            rekordbox_pdb_t::PAGE_TYPE_ARTWORK,
            rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES,
            rekordbox_pdb_t::PAGE_TYPE_TRACKS,
            rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE};
    for (const auto type : types) {
        for (const auto* table : *pdb.tables()) {
            if (table->type() != type) {
                continue;
            }
            auto* pageRef = table->first_page();
            const auto last = table->last_page()->index();
            QSet<uint32_t> visited;
            while (pageRef) {
                if (cancelled(request, &result)) {
                    return result;
                }
                if (visited.contains(pageRef->index()) || visited.size() > 100000) {
                    throw std::runtime_error("Cyclic or excessive PDB page chain");
                }
                visited.insert(pageRef->index());
                if ((static_cast<quint64>(pageRef->index()) + 1) * pageSize >
                        static_cast<quint64>(bytes.size())) {
                    throw std::runtime_error("PDB page reference extends beyond the source");
                }
                auto* page = pageRef->body();
                if (page->type() != type || page->page_index() != pageRef->index()) {
                    throw std::runtime_error(
                            "PDB page type or identity does not match its "
                            "table reference");
                }
                if (page->is_data_page()) {
                    for (auto* group : *page->row_groups()) {
                        for (auto* rowRef : *group->rows()) {
                            if (cancelled(request, &result)) {
                                return result;
                            }
                            if (!rowRef->present()) {
                                continue;
                            }
                            switch (type) {
                            case rekordbox_pdb_t::PAGE_TYPE_KEYS: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::key_row_t*>(
                                        rowRef->body());
                                keys[row->id()] = pdbText(row->name());
                                break;
                            }
                            case rekordbox_pdb_t::PAGE_TYPE_GENRES: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::genre_row_t*>(
                                        rowRef->body());
                                genres[row->id()] = pdbText(row->name());
                                break;
                            }
                            case rekordbox_pdb_t::PAGE_TYPE_ARTISTS: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::artist_row_t*>(
                                        rowRef->body());
                                artists[row->id()] = pdbText(row->name());
                                break;
                            }
                            case rekordbox_pdb_t::PAGE_TYPE_ALBUMS: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::album_row_t*>(
                                        rowRef->body());
                                albums[row->id()] = pdbText(row->name());
                                break;
                            }
                            case rekordbox_pdb_t::PAGE_TYPE_LABELS: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::label_row_t*>(
                                        rowRef->body());
                                labels[row->id()] = pdbText(row->name());
                                break;
                            }
                            case rekordbox_pdb_t::PAGE_TYPE_ARTWORK: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::artwork_row_t*>(
                                        rowRef->body());
                                artworks[row->id()] =
                                        volumePath(request.source.locator,
                                                pdbText(row->path()));
                                break;
                            }
                            case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::playlist_entry_row_t*>(
                                        rowRef->body());
                                auto& entries = memberships[row->playlist_id()];
                                if (entries.contains(row->entry_index())) {
                                    throw std::runtime_error(
                                            "Duplicate PDB playlist membership "
                                            "position");
                                }
                                entries.insert(row->entry_index(), row->track_id());
                                break;
                            }
                            case rekordbox_pdb_t::PAGE_TYPE_TRACKS: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::track_row_t*>(
                                        rowRef->body());
                                ExternalTrack external;
                                external.sourceTrackId = QStringLiteral("pdb:%1").arg(row->id());
                                if (trackKeys.contains(external.sourceTrackId)) {
                                    throw std::runtime_error("Duplicate PDB track identity");
                                }
                                trackKeys.insert(external.sourceTrackId);
                                auto& track = external.metadata;
                                track.sourceOrdinal = snapshot->tracks.size();
                                track.id = static_cast<int>(row->id());
                                track.hasValidId = true;
                                track.title = pdbText(row->title());
                                track.artist = artists.value(row->artist_id());
                                track.composer = artists.value(row->composer_id());
                                track.remixer = artists.value(row->remixer_id());
                                track.label = labels.value(row->label_id());
                                track.artworkReference = artworks.value(row->artwork_id());
                                track.album = albums.value(row->album_id());
                                track.genre = genres.value(row->genre_id());
                                track.key = keys.value(row->key_id());
                                track.year = row->year();
                                track.trackNumber = row->track_number();
                                track.discNumber = row->disc_number();
                                track.playCount = row->play_count();
                                track.dateAdded = pdbText(row->date_added());
                                track.bpm = row->tempo() / 100.0;
                                track.bitrate = row->bitrate();
                                track.durationSeconds = row->duration();
                                track.rating = row->rating();
                                track.comment = pdbText(row->comment());
                                track.sampleRate = row->sample_rate();
                                track.location =
                                        volumePath(request.source.locator,
                                                pdbText(row->file_path()));
                                external.analyzePath =
                                        volumePath(request.source.locator,
                                                pdbText(row->analyze_path()));
                                external.color = pdbColor(row->color_id());
                                snapshot->tracks.push_back(std::move(external));
                                break;
                            }
                            case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE: {
                                auto* row = static_cast<
                                        rekordbox_pdb_t::playlist_tree_row_t*>(
                                        rowRef->body());
                                if (playlists.contains(row->id())) {
                                    throw std::runtime_error("Duplicate PDB playlist identity");
                                }
                                ExternalPlaylist playlist;
                                playlist.id = QStringLiteral("pdb:%1").arg(row->id());
                                if (row->parent_id() != 0) {
                                    playlist.parentId =
                                            QStringLiteral("pdb:%1").arg(
                                                    row->parent_id());
                                }
                                playlist.name = pdbText(row->name());
                                playlist.folder = row->is_folder();
                                playlists.insert(row->id(), std::move(playlist));
                                playlistOrder.insert(row->id(), row->sort_order());
                                break;
                            }
                            default:
                                break;
                            }
                        }
                    }
                }
                if (pageRef->index() == last) {
                    break;
                }
                pageRef = page->next_page();
            }
        }
    }
    QVector<uint32_t> ids;
    for (auto it = playlists.cbegin(); it != playlists.cend(); ++it) {
        ids.push_back(it.key());
    }
    std::stable_sort(ids.begin(), ids.end(), [&](uint32_t left, uint32_t right) {
        return playlistOrder.value(left) < playlistOrder.value(right);
    });
    for (const auto id : ids) {
        auto playlist = playlists.value(id);
        for (const auto track : memberships.value(id)) {
            const QString key = QStringLiteral("pdb:%1").arg(track);
            if (trackKeys.contains(key)) {
                playlist.trackIds.push_back(key);
            } else {
                result.diagnostics.append(QStringLiteral("Playlist %1 references missing track %2.")
                                .arg(playlist.name)
                                .arg(track));
            }
        }
        snapshot->playlists.push_back(std::move(playlist));
    }
    if (cancelled(request, &result)) {
        return result;
    }
    result.outcome = result.diagnostics.isEmpty() ? ImportOutcome::Success : ImportOutcome::Warning;
    result.snapshot = std::move(snapshot);
    return result;
}
} // namespace

QString rekordboxPdbPath(const QString& root) {
    for (const auto& folder : {QStringLiteral("PIONEER"), QStringLiteral(".PIONEER")}) {
        const QString path = QDir(root).filePath(folder + QStringLiteral("/rekordbox/export.pdb"));
        if (QFileInfo(path).isFile()) {
            return path;
        }
    }
    return {};
}

QString xmlSourceTrackId(const Library& library, int trackIndex) {
    const auto& track = library.tracks.at(trackIndex);
    int ids = 0;
    int locations = 0;
    for (const auto& candidate : library.tracks) {
        ids += track.hasValidId && candidate.hasValidId && candidate.id == track.id;
        locations += !track.location.isEmpty() && candidate.location == track.location;
    }
    if (track.hasValidId && ids == 1) {
        return QStringLiteral("id:%1").arg(track.id);
    }
    if (!track.hasValidId && locations == 1) {
        return QStringLiteral("location:") +
                QString::fromLatin1(QCryptographicHash::hash(
                        track.location.toUtf8(), QCryptographicHash::Sha256)
                                .toHex());
    }
    return QStringLiteral("record:%1").arg(trackIndex);
}

SourceResult decodeSource(const SourceRequest& request) {
    SourceResult result;
    result.request = request;
    if (cancelled(request, &result)) {
        return result;
    }
    try {
        const QString path = request.source.kind == SourceKind::Xml
                ? request.source.locator
                : rekordboxPdbPath(request.source.locator);
        if (path.isEmpty()) {
            result.outcome = ImportOutcome::Unavailable;
            result.diagnostics = {QStringLiteral("The rekordbox export database is unavailable.")};
            return result;
        }
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly)) {
            result.outcome = input.error() == QFileDevice::PermissionsError
                    ? ImportOutcome::Denied
                    : ImportOutcome::Unavailable;
            result.diagnostics = {input.errorString()};
            return result;
        }
        if (input.size() < 0 || input.size() > kMaximumSourceBytes) {
            result.outcome = ImportOutcome::Unsupported;
            result.diagnostics = {QStringLiteral("The source exceeds the 256 MiB reader limit.")};
            return result;
        }
        const auto initialSize = input.size();
        const auto initialModified = input.fileTime(QFileDevice::FileModificationTime);
        const QByteArray bytes = input.read(kMaximumSourceBytes + 1);
        if (input.error() != QFileDevice::NoError ||
                bytes.size() != initialSize || input.size() != initialSize ||
                input.fileTime(QFileDevice::FileModificationTime) !=
                        initialModified) {
            result.outcome = ImportOutcome::Unavailable;
            result.diagnostics = {
                    QStringLiteral("The source changed or could not be read "
                                   "completely. Refresh to retry.")};
            return result;
        }
        if (cancelled(request, &result)) {
            return result;
        }
        result = request.source.kind == SourceKind::Xml
                ? decodeXmlSource(request, bytes)
                : decodeUsbSource(request, bytes);
        if (result.snapshot) {
            auto snapshot = std::make_shared<SourceSnapshot>(*result.snapshot);
            snapshot->sourcePath = path;
            snapshot->sourceFileSize = initialSize;
            snapshot->sourceModified = initialModified;
            result.snapshot = std::move(snapshot);
        }
        if (!QFileInfo::exists(path) || input.size() != initialSize ||
                input.fileTime(QFileDevice::FileModificationTime) != initialModified) {
            result.outcome = ImportOutcome::Unavailable;
            result.snapshot.reset();
            result.diagnostics = {
                    QStringLiteral("The source disappeared or changed during "
                                   "decoding. Refresh to retry.")};
        }
        cancelled(request, &result);
        return result;
    } catch (const std::exception& exception) {
        result.outcome = ImportOutcome::Invalid;
        result.diagnostics = {QString::fromUtf8(exception.what())};
    } catch (...) {
        result.outcome = ImportOutcome::Failed;
        result.diagnostics = {QStringLiteral("Unexpected source reader failure.")};
    }
    result.snapshot.reset();
    return result;
}
} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
