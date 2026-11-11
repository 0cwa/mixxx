// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxnativereader.h"

#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace mixxx::rekordbox {
namespace {

using Row = DatabaseProperties;
using Rows = std::vector<Row>;
using Index = std::map<std::string, Row>;

struct ReadFailure {
    DatabaseOutcome outcome;
    DatabaseIssue issue;
};

[[noreturn]] void fail(DatabaseOutcome outcome,
        const std::string& code,
        const std::string& message,
        std::optional<std::string> id = {}) {
    throw ReadFailure{outcome, {code, message, std::move(id)}};
}

std::optional<std::string> field(const Row& row, const std::string& key) {
    const auto found = row.find(key);
    return found == row.end() ? std::nullopt : found->second;
}

std::string required(const Row& row, const std::string& key, bool allowEmpty = false) {
    const auto value = field(row, key);
    if (!value || (!allowEmpty && value->empty())) {
        fail(DatabaseOutcome::Corrupt,
                "native.required_value",
                "A required native database value is absent.",
                field(row, "ID"));
    }
    return *value;
}

std::optional<std::int64_t> integer(const Row& row, const std::string& key) {
    const auto value = field(row, key);
    if (!value) {
        return {};
    }
    std::int64_t parsed = 0;
    const auto conversion = std::from_chars(
            value->data(), value->data() + value->size(), parsed);
    if (conversion.ec != std::errc() || conversion.ptr != value->data() + value->size()) {
        fail(DatabaseOutcome::Corrupt,
                "native.invalid_integer",
                "A native database integer is invalid or out of range.",
                field(row, "ID"));
    }
    return parsed;
}

std::int64_t requiredInteger(const Row& row, const std::string& key) {
    const auto value = integer(row, key);
    if (!value) {
        fail(DatabaseOutcome::Corrupt,
                "native.required_integer",
                "A required native database integer is absent.",
                field(row, "ID"));
    }
    return *value;
}

bool deleted(const Row& row) {
    const auto value = integer(row, "rb_local_deleted");
    if (value && *value != 0 && *value != 1) {
        fail(DatabaseOutcome::Unsupported,
                "native.deletion_flag",
                "The native deletion flag is not a qualified value.",
                field(row, "ID"));
    }
    return value && *value == 1;
}

std::string normalPath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool windowsDrive(const std::string& path) {
    return path.size() >= 2 &&
            ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':';
}

bool uri(const std::string& path) {
    return !windowsDrive(path) && path.find(':') != std::string::npos;
}

class Reader {
  public:
    Reader(sqlite3* database, const DatabaseReadContext& context)
            : m_database(database),
              m_context(context) {
    }

    DatabaseReadResult read();

  private:
    void checkCancelled() const {
        if (m_context.isCancelled()) {
            fail(DatabaseOutcome::Cancelled,
                    "native.cancelled",
                    "Native library reading was cancelled.");
        }
    }

    Rows query(const std::string& sql);
    Rows table(const std::string& name,
            const std::vector<std::string>& requiredColumns,
            bool optional = false);
    Index index(const std::string& name, const std::vector<std::string>& columns);
    void issue(const std::string& code,
            const std::string& message,
            std::optional<std::string> id = {}) {
        m_result.issues.push_back({code, message, std::move(id)});
    }
    std::optional<std::string> lookup(const Row& row,
            const std::string& reference,
            const Index& values,
            const std::string& name = "Name") const;
    std::optional<std::filesystem::path> asset(const std::filesystem::path& root,
            const std::optional<std::string>& original,
            const std::string& id);
    void media(DatabaseTrack* track, const Row& row);
    void readPlaylists(DatabaseLibrary* library,
            const std::set<std::string>& trackIds,
            bool history);
    void readAnnotations(DatabaseLibrary* library, const std::set<std::string>& trackIds);

    sqlite3* m_database;
    const DatabaseReadContext& m_context;
    std::size_t m_rowCount = 0;
    bool m_missingMedia = false;
    DatabaseReadResult m_result;
};

Rows Reader::query(const std::string& sql) {
    checkCancelled();
    sqlite3_stmt* raw = nullptr;
    const int prepared = sqlite3_prepare_v2(m_database, sql.c_str(), -1, &raw, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (prepared != SQLITE_OK) {
        fail(prepared == SQLITE_NOTADB || prepared == SQLITE_CORRUPT
                        ? DatabaseOutcome::Corrupt
                        : DatabaseOutcome::Failed,
                "native.query_failed",
                "The private native database could not be read.");
    }
    if (!sqlite3_stmt_readonly(statement.get())) {
        fail(DatabaseOutcome::Failed,
                "native.query_not_readonly",
                "The native reader refused a non-reading statement.");
    }
    Rows rows;
    int result = SQLITE_OK;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        checkCancelled();
        if (++m_rowCount > m_context.maximumRecords) {
            fail(DatabaseOutcome::Unsupported,
                    "native.record_limit",
                    "The native database exceeds the configured record limit.");
        }
        Row row;
        for (int column = 0; column < sqlite3_column_count(statement.get()); ++column) {
            const auto name = std::string(sqlite3_column_name(statement.get(), column));
            if (sqlite3_column_type(statement.get(), column) == SQLITE_NULL) {
                row.emplace(name, std::nullopt);
                continue;
            }
            if (sqlite3_column_type(statement.get(), column) == SQLITE_BLOB) {
                fail(DatabaseOutcome::Unsupported,
                        "native.blob_value",
                        "A native record contains an unqualified binary "
                        "value.");
            }
            const int bytes = sqlite3_column_bytes(statement.get(), column);
            if (bytes < 0 || static_cast<std::size_t>(bytes) > m_context.maximumStringBytes) {
                fail(DatabaseOutcome::Unsupported,
                        "native.string_limit",
                        "A native database value exceeds the configured byte "
                        "limit.");
            }
            const auto* text = sqlite3_column_text(statement.get(), column);
            if (!text) {
                fail(DatabaseOutcome::Failed,
                        "native.value_unavailable",
                        "A native database value could not be materialized.");
            }
            auto value = std::string(reinterpret_cast<const char*>(text), bytes);
            if (value.find('\0') != std::string::npos) {
                fail(DatabaseOutcome::Corrupt,
                        "native.text_nul",
                        "A native text value contains an embedded null byte.");
            }
            row.emplace(name, std::move(value));
        }
        rows.push_back(std::move(row));
    }
    checkCancelled();
    if (result != SQLITE_DONE) {
        fail(result == SQLITE_NOTADB || result == SQLITE_CORRUPT
                        ? DatabaseOutcome::Corrupt
                        : DatabaseOutcome::Failed,
                "native.query_failed",
                "The private native database read did not complete.");
    }
    return rows;
}

Rows Reader::table(const std::string& name,
        const std::vector<std::string>& requiredColumns,
        bool optional) {
    // All identifiers here are fixed reader constants, never database text.
    const auto definition = query("SELECT type, sql FROM sqlite_schema WHERE name='" + name + "'");
    if (definition.empty() && optional) {
        return {};
    }
    if (definition.size() != 1 || field(definition.front(), "type") != "table") {
        fail(DatabaseOutcome::Unsupported,
                "native.schema_table",
                "The native profile requires ordinary database tables.");
    }
    auto declaration = field(definition.front(), "sql").value_or("");
    std::transform(declaration.begin(),
            declaration.end(),
            declaration.begin(),
            [](char character) {
                return character >= 'a' && character <= 'z'
                        ? static_cast<char>(character - 'a' + 'A')
                        : character;
            });
    if (declaration.find("VIRTUAL") != std::string::npos) {
        fail(DatabaseOutcome::Unsupported,
                "native.schema_table",
                "Virtual tables are outside the qualified native profile.");
    }
    const auto schema = query("PRAGMA table_info(\"" + name + "\")");
    if (schema.empty() && optional) {
        return {};
    }
    std::set<std::string> columns;
    for (const auto& row : schema) {
        columns.insert(required(row, "name"));
    }
    for (const auto& column : requiredColumns) {
        if (columns.count(column) == 0) {
            fail(DatabaseOutcome::Unsupported,
                    "native.schema_columns",
                    "The native database does not match the qualified "
                    "DBVersion 6000 profile.");
        }
    }
    return query("SELECT * FROM \"" + name + "\"");
}

Index Reader::index(const std::string& name, const std::vector<std::string>& columns) {
    Index result;
    for (auto& row : table(name, columns)) {
        checkCancelled();
        if (deleted(row)) {
            continue;
        }
        const auto id = required(row, "ID");
        if (!result.emplace(id, std::move(row)).second) {
            fail(DatabaseOutcome::Corrupt,
                    "native.duplicate_id",
                    "A native table contains duplicate active identifiers.",
                    id);
        }
    }
    return result;
}

std::optional<std::string> Reader::lookup(const Row& row,
        const std::string& reference,
        const Index& values,
        const std::string& name) const {
    const auto id = field(row, reference);
    if (!id || id->empty() || *id == "0") {
        return {};
    }
    const auto found = values.find(*id);
    if (found == values.end()) {
        fail(DatabaseOutcome::Corrupt,
                "native.missing_reference",
                "A native metadata reference does not identify an active row.",
                field(row, "ID"));
    }
    return field(found->second, name);
}

std::optional<std::filesystem::path> Reader::asset(const std::filesystem::path& root,
        const std::optional<std::string>& original,
        const std::string& id) {
    if (!original || original->empty()) {
        return {};
    }
    auto value = normalPath(*original);
    if (windowsDrive(value) || uri(value) || value.starts_with("//") ||
            value.find('\0') != std::string::npos) {
        issue("native.asset_path",
                "A native asset locator is not a safe library-relative path.",
                id);
        return {};
    }
    if (value.starts_with('/')) {
        value.erase(0, 1);
    }
    const auto relative = std::filesystem::path(std::u8string(value.begin(), value.end()));
    for (const auto& part : relative) {
        if (part == "..") {
            issue("native.asset_path", "A native asset locator escapes its library directory.", id);
            return {};
        }
    }
    if (root.empty() || !root.is_absolute() || relative.empty() || relative.is_absolute()) {
        issue("native.asset_root",
                "The original native library asset directory is unavailable.",
                id);
        return {};
    }
    const auto result = (root / relative).lexically_normal();
    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) {
        issue("native.asset_root",
                "The original native library asset directory cannot be "
                "resolved.",
                id);
        return {};
    }
    const auto canonicalPath = std::filesystem::weakly_canonical(result, error);
    if (error) {
        issue("native.asset_path", "A native asset path cannot be resolved safely.", id);
        return {};
    }
    const auto mismatch = std::mismatch(
            canonicalRoot.begin(), canonicalRoot.end(), canonicalPath.begin(), canonicalPath.end());
    if (mismatch.first != canonicalRoot.end()) {
        issue("native.asset_path", "A native asset symlink escapes its library directory.", id);
        return {};
    }
    if (!std::filesystem::is_regular_file(result, error)) {
        issue("native.missing_asset",
                "A referenced native analysis or artwork file is unavailable.",
                id);
    }
    return result;
}

void Reader::media(DatabaseTrack* track, const Row& row) {
    track->originalMediaPath = field(row, "FolderPath");
    const auto service = integer(row, "ServiceID");
    if (service && *service != 0) {
        track->mediaKind = DatabaseMediaKind::Streaming;
        issue("native.streaming",
                "Streaming service media is preserved but is not playable by "
                "this reader.",
                track->sourceTrackId);
        return;
    }
    if (!track->originalMediaPath || track->originalMediaPath->empty()) {
        issue("native.unavailable_media",
                "A native track has no local media locator.",
                track->sourceTrackId);
        return;
    }
    const auto value = normalPath(*track->originalMediaPath);
    if (uri(value) || value.starts_with("//") || value.find('\0') != std::string::npos) {
        issue("native.unavailable_media",
                "A native media locator is not a local filesystem path.",
                track->sourceTrackId);
        return;
    }
#ifndef _WIN32
    if (windowsDrive(value)) {
        issue("native.foreign_path",
                "A native Windows media path needs an explicit relocation on "
                "this platform.",
                track->sourceTrackId);
        return;
    }
#endif
    auto resolved = std::filesystem::path(std::u8string(value.begin(), value.end()));
    if (!resolved.is_absolute()) {
        if (windowsDrive(value) || m_context.mediaRoot.empty() ||
                !m_context.mediaRoot.is_absolute()) {
            issue("native.media_root",
                    "A relative native media path needs an explicit original "
                    "media directory.",
                    track->sourceTrackId);
            return;
        }
        for (const auto& part : resolved) {
            if (part == "..") {
                issue("native.media_path",
                        "A relative native media path escapes its supplied "
                        "directory.",
                        track->sourceTrackId);
                return;
            }
        }
        resolved = m_context.mediaRoot / resolved;
    }
    track->mediaPath = resolved.lexically_normal();
    std::error_code error;
    if (std::filesystem::is_regular_file(*track->mediaPath, error)) {
        track->mediaKind = DatabaseMediaKind::Local;
    } else {
        track->mediaKind = DatabaseMediaKind::Missing;
        m_missingMedia = true;
        issue("native.missing_media",
                "A referenced native media file is unavailable.",
                track->sourceTrackId);
    }
}

void Reader::readPlaylists(DatabaseLibrary* library,
        const std::set<std::string>& trackIds,
        bool history) {
    const std::string tableName = history ? "djmdHistory" : "djmdPlaylist";
    const std::string songTable = history ? "djmdSongHistory" : "djmdSongPlaylist";
    const std::string reference = history ? "HistoryID" : "PlaylistID";
    const std::string prefix = history ? "history:" : "playlist:";
    std::map<std::string, DatabasePlaylist> playlists;
    std::set<std::pair<std::optional<std::string>, std::int64_t>> siblingPositions;
    for (auto& row : table(tableName,
                 {"ID", "Seq", "Name", "Attribute", "ParentID", "rb_local_deleted"},
                 history)) {
        checkCancelled();
        if (deleted(row)) {
            continue;
        }
        DatabasePlaylist playlist;
        playlist.id = prefix + required(row, "ID");
        playlist.name = required(row, "Name", true);
        playlist.position = requiredInteger(row, "Seq");
        if (playlist.position < 0) {
            fail(DatabaseOutcome::Corrupt,
                    "native.playlist_position",
                    "A native playlist position is negative.",
                    playlist.id);
        }
        const auto parent = field(row, "ParentID");
        if (parent && !parent->empty() && *parent != "root" && *parent != "0") {
            playlist.parentId = prefix + *parent;
        }
        if (!siblingPositions.emplace(playlist.parentId, playlist.position).second) {
            fail(DatabaseOutcome::Corrupt,
                    "native.ambiguous_hierarchy_order",
                    "Native sibling playlists have ambiguous repeated "
                    "positions.",
                    playlist.id);
        }
        const auto type = requiredInteger(row, "Attribute");
        if (type == 0) {
            playlist.kind = history ? DatabasePlaylistKind::History : DatabasePlaylistKind::Regular;
        } else if (type == 1) {
            playlist.kind = DatabasePlaylistKind::Folder;
        } else if (type == 4 && !history) {
            playlist.kind = DatabasePlaylistKind::Smart;
            issue("native.smart_playlist",
                    "Smart playlist definitions are preserved without "
                    "evaluation.",
                    playlist.id);
        } else {
            playlist.kind = DatabasePlaylistKind::Unsupported;
            issue("native.playlist_type",
                    "A native playlist type is preserved without an import "
                    "mapping.",
                    playlist.id);
        }
        playlist.properties = std::move(row);
        const auto id = playlist.id;
        if (!playlists.emplace(id, std::move(playlist)).second) {
            fail(DatabaseOutcome::Corrupt,
                    "native.duplicate_playlist",
                    "The native database contains duplicate playlist "
                    "identifiers.",
                    id);
        }
    }
    std::set<std::string> membershipIds;
    for (auto& row : table(songTable,
                 {"ID", reference, "ContentID", "TrackNo", "rb_local_deleted"},
                 history)) {
        checkCancelled();
        if (deleted(row)) {
            continue;
        }
        const auto id = required(row, "ID");
        const auto playlist = playlists.find(prefix + required(row, reference));
        const auto contentId = required(row, "ContentID");
        if (!membershipIds.insert(id).second || playlist == playlists.end() ||
                trackIds.count(contentId) == 0 ||
                playlist->second.kind == DatabasePlaylistKind::Folder) {
            fail(DatabaseOutcome::Corrupt,
                    "native.membership_reference",
                    "Native playlist membership has a duplicate or unresolved "
                    "reference.",
                    id);
        }
        const auto position = requiredInteger(row, "TrackNo");
        if (position <= 0) {
            fail(DatabaseOutcome::Corrupt,
                    "native.membership_position",
                    "Native playlist positions must be positive one-based "
                    "integers.",
                    id);
        }
        playlist->second.entries.push_back({id, contentId, position, std::move(row)});
    }
    std::set<std::string> validatedParents;
    for (auto& [id, playlist] : playlists) {
        checkCancelled();
        std::set<std::string> ancestors{id};
        auto parent = playlist.parentId;
        while (parent && validatedParents.count(*parent) == 0) {
            checkCancelled();
            const auto ancestor = playlists.find(*parent);
            if (ancestor == playlists.end() || !ancestors.insert(*parent).second ||
                    ancestor->second.kind != DatabasePlaylistKind::Folder) {
                fail(DatabaseOutcome::Corrupt,
                        "native.playlist_hierarchy",
                        "Native playlist parentage is cyclic, missing, or not "
                        "a folder.",
                        id);
            }
            parent = ancestor->second.parentId;
        }
        if (parent && playlists.at(*parent).kind != DatabasePlaylistKind::Folder) {
            fail(DatabaseOutcome::Corrupt,
                    "native.playlist_hierarchy",
                    "A native playlist parent is not a folder.",
                    id);
        }
        validatedParents.insert(ancestors.begin(), ancestors.end());
        std::sort(playlist.entries.begin(),
                playlist.entries.end(),
                [](const auto& left, const auto& right) {
                    return left.position < right.position;
                });
        for (std::size_t i = 1; i < playlist.entries.size(); ++i) {
            if (playlist.entries[i - 1].position == playlist.entries[i].position) {
                fail(DatabaseOutcome::Corrupt,
                        "native.ambiguous_order",
                        "Native playlist entries have ambiguous repeated "
                        "positions.",
                        id);
            }
        }
    }
    std::vector<DatabasePlaylist> ordered;
    for (auto& [id, playlist] : playlists) {
        ordered.push_back(std::move(playlist));
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.parentId != right.parentId) {
            return left.parentId < right.parentId;
        }
        return left.position != right.position ? left.position < right.position
                                               : left.id < right.id;
    });
    for (auto& playlist : ordered) {
        library->playlists.push_back(std::move(playlist));
    }
}

void Reader::readAnnotations(DatabaseLibrary* library, const std::set<std::string>& trackIds) {
    std::set<std::string> ids;
    std::set<std::pair<std::string, std::uint32_t>> hotSlots;
    for (auto& row : table("djmdCue",
                 {"ID",
                         "ContentID",
                         "InMsec",
                         "OutMsec",
                         "Kind",
                         "Color",
                         "ActiveLoop",
                         "Comment",
                         "rb_local_deleted"})) {
        checkCancelled();
        if (deleted(row)) {
            continue;
        }
        DatabaseAnnotation annotation;
        annotation.sourceAnnotationId = required(row, "ID");
        annotation.sourceTrackId = required(row, "ContentID");
        if (!ids.insert(annotation.sourceAnnotationId).second ||
                trackIds.count(annotation.sourceTrackId) == 0) {
            fail(DatabaseOutcome::Corrupt,
                    "native.annotation_reference",
                    "A native annotation has a duplicate or unresolved "
                    "identifier.",
                    annotation.sourceAnnotationId);
        }
        annotation.startMilliseconds = requiredInteger(row, "InMsec");
        const auto end = requiredInteger(row, "OutMsec");
        const auto kind = requiredInteger(row, "Kind");
        if (annotation.startMilliseconds < 0 || end < -1 ||
                (end != -1 && end <= annotation.startMilliseconds) || kind < 0 ||
                kind > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            fail(DatabaseOutcome::Corrupt,
                    "native.annotation_timing",
                    "A native annotation has an invalid role or millisecond "
                    "interval.",
                    annotation.sourceAnnotationId);
        }
        if (end != -1) {
            annotation.endMilliseconds = end;
        }
        if (kind == 0) {
            annotation.role = annotation.endMilliseconds
                    ? DatabaseCueRole::MemoryLoop
                    : DatabaseCueRole::MemoryCue;
        } else {
            annotation.hotSlot = static_cast<std::uint32_t>(kind - 1);
            annotation.role = annotation.endMilliseconds
                    ? DatabaseCueRole::HotLoop
                    : DatabaseCueRole::HotCue;
            if (!hotSlots.emplace(annotation.sourceTrackId, *annotation.hotSlot).second) {
                fail(DatabaseOutcome::Corrupt,
                        "native.duplicate_hot_slot",
                        "A native track has more than one annotation in the "
                        "same hot slot.",
                        annotation.sourceAnnotationId);
            }
        }
        annotation.label = field(row, "Comment");
        annotation.colorIndex = integer(row, "Color");
        if (annotation.colorIndex && *annotation.colorIndex == -1) {
            annotation.colorIndex.reset();
        }
        annotation.startFrame150 = integer(row, "InFrame");
        annotation.endFrame150 = integer(row, "OutFrame");
        const auto active = integer(row, "ActiveLoop");
        if (active) {
            if ((*active != 0 && *active != 1) || (*active == 1 && !annotation.endMilliseconds)) {
                fail(DatabaseOutcome::Unsupported,
                        "native.active_loop",
                        "A native active-loop value has unqualified semantics.",
                        annotation.sourceAnnotationId);
            }
            annotation.activeLoop = *active != 0;
        }
        annotation.properties = std::move(row);
        library->annotations.push_back(std::move(annotation));
    }
    std::sort(library->annotations.begin(),
            library->annotations.end(),
            [](const auto& left, const auto& right) {
                return left.sourceAnnotationId < right.sourceAnnotationId;
            });
    for (const auto& row : table("contentCue", {"ID", "ContentID", "Cues"}, true)) {
        checkCancelled();
        if (!deleted(row) && field(row, "Cues") && !field(row, "Cues")->empty()) {
            fail(DatabaseOutcome::Unsupported,
                    "native.alternate_cues",
                    "The library contains alternate cue payloads whose "
                    "authority has not been qualified.",
                    field(row, "ID"));
        }
    }
}

DatabaseReadResult Reader::read() {
    checkCancelled();
    if (!m_database || m_context.sourceId.empty() || m_context.fingerprint.empty() ||
            !m_context.originalDatabasePath.is_absolute()) {
        fail(DatabaseOutcome::Failed,
                "native.request",
                "A private database, fingerprint, original absolute database "
                "path and stable source binding are required.");
    }
    auto library = std::make_shared<DatabaseLibrary>();
    library->context = m_context;
    library->format = "rekordbox-native";
    library->schemaProfile = "native-dbversion-6000-v1";
    const auto properties = table("djmdProperty", {"DBID", "DBVersion"});
    if (properties.size() != 1) {
        fail(DatabaseOutcome::Corrupt,
                "native.property_count",
                "The native database must identify exactly one library.");
    }
    if (required(properties.front(), "DBVersion") != "6000") {
        fail(DatabaseOutcome::Unsupported,
                "native.schema_version",
                "This native database version has not been qualified.");
    }
    library->databaseIdentity = required(properties.front(), "DBID");
    library->properties = properties.front();
    const auto artists = index("djmdArtist", {"ID", "Name", "rb_local_deleted"});
    const auto albums = index("djmdAlbum", {"ID", "Name", "AlbumArtistID", "rb_local_deleted"});
    const auto genres = index("djmdGenre", {"ID", "Name", "rb_local_deleted"});
    const auto keys = index("djmdKey", {"ID", "ScaleName", "rb_local_deleted"});
    const auto labels = index("djmdLabel", {"ID", "Name", "rb_local_deleted"});
    // codespell:ignore-next-line commnt
    const auto colors = index("djmdColor", {"ID", "ColorCode", "Commnt", "rb_local_deleted"});
    for (const auto& [name, rows] :
            {std::pair{"djmdArtist", &artists},
                    {"djmdAlbum", &albums},
                    {"djmdGenre", &genres},
                    {"djmdKey", &keys},
                    {"djmdLabel", &labels},
                    {"djmdColor", &colors}}) {
        for (const auto& [id, row] : *rows) {
            checkCancelled();
            library->opaqueRecords.push_back({name, row});
        }
    }
    const auto tracks = index("djmdContent",
            {"ID",
                    "FolderPath",
                    "Title",
                    "BPM",
                    "Length",
                    "ArtistID",
                    "AlbumID",
                    "GenreID",
                    "KeyID",
                    "AnalysisDataPath",
                    "ImagePath",
                    "FileType",
                    "ServiceID",
                    "rb_local_deleted"});
    const auto libraryRoot = m_context.assetRoot.empty()
            ? m_context.originalDatabasePath.parent_path()
            : m_context.assetRoot;
    std::set<std::string> trackIds;
    for (const auto& [id, row] : tracks) {
        checkCancelled();
        DatabaseTrack track;
        track.sourceTrackId = id;
        track.title = field(row, "Title");
        track.artist = lookup(row, "ArtistID", artists);
        track.album = lookup(row, "AlbumID", albums);
        const auto albumId = field(row, "AlbumID");
        if (albumId && albums.count(*albumId)) {
            track.albumArtist = lookup(albums.at(*albumId), "AlbumArtistID", artists);
        }
        track.composer = lookup(row, "ComposerID", artists);
        track.remixer = lookup(row, "RemixerID", artists);
        track.genre = lookup(row, "GenreID", genres);
        track.key = lookup(row, "KeyID", keys, "ScaleName");
        track.label = lookup(row, "LabelID", labels);
        // codespell:ignore-next-line commnt
        track.comment = field(row, "Commnt");
        track.dateAdded = field(row, "StockDate");
        track.dateModified = field(row, "updated_at");
        track.year = integer(row, "ReleaseYear");
        track.trackNumber = integer(row, "TrackNo");
        track.discNumber = integer(row, "DiscNo");
        track.bitrate = integer(row, "BitRate");
        track.sampleRate = integer(row, "SampleRate");
        track.rating = integer(row, "Rating");
        track.playCount = integer(row, "DJPlayCount");
        track.colorIndex = integer(row, "ColorID");
        for (const auto value : {track.year,
                     track.trackNumber,
                     track.discNumber,
                     track.bitrate,
                     track.sampleRate,
                     track.rating,
                     track.playCount,
                     track.colorIndex}) {
            if (value && *value < 0) {
                fail(DatabaseOutcome::Corrupt,
                        "native.metadata_range",
                        "A native metadata count or measurement is negative.",
                        id);
            }
        }
        const auto bpm = integer(row, "BPM");
        const auto seconds = integer(row, "Length");
        if ((bpm && *bpm < 0) ||
                (seconds &&
                        (*seconds < 0 ||
                                *seconds > std::numeric_limits<
                                                   std::int64_t>::max() /
                                                1000))) {
            fail(DatabaseOutcome::Corrupt,
                    "native.track_timing",
                    "A native track has invalid duration or tempo.",
                    id);
        }
        if (bpm) {
            track.bpm = static_cast<double>(*bpm) / 100.0;
        }
        if (seconds) {
            track.durationMilliseconds = *seconds * 1000;
        }
        const auto type = integer(row, "FileType");
        const std::map<std::int64_t, std::string> types{{1, "MP3"},
                {3, "MP4"},
                {4, "AAC"},
                {5, "FLAC"},
                {6, "ALAC"},
                {11, "WAV"},
                {12, "AIFF"},
                {16, "Video"}};
        if (type && types.count(*type)) {
            track.kind = types.at(*type);
        }
        track.originalAnalysisPath = field(row, "AnalysisDataPath");
        track.originalArtworkPath = field(row, "ImagePath");
        track.analysisPath =
                asset(libraryRoot.empty() ? libraryRoot : libraryRoot / "share",
                        track.originalAnalysisPath,
                        id);
        track.artworkPath = asset(libraryRoot, track.originalArtworkPath, id);
        track.properties = row;
        // codespell:ignore-next-line commnt
        const auto colorName = lookup(row, "ColorID", colors, "Commnt");
        if (field(row, "ColorID")) {
            track.properties.emplace("ColorName", colorName);
            track.properties.emplace("ColorCode", lookup(row, "ColorID", colors, "ColorCode"));
        }
        media(&track, row);
        trackIds.insert(id);
        library->tracks.push_back(std::move(track));
    }
    readPlaylists(library.get(), trackIds, false);
    readPlaylists(library.get(), trackIds, true);
    readAnnotations(library.get(), trackIds);
    for (const auto* name : {"djmdActiveCensor",
                 "contentActiveCensor",
                 "djmdHotCueBanklist",
                 "djmdSongHotCueBanklist",
                 "hotCueBanklistCue",
                 "djmdMyTag",
                 "djmdSongMyTag",
                 "djmdRelatedTracks",
                 "djmdSongRelatedTracks",
                 "djmdSampler",
                 "djmdSongSampler",
                 "djmdSongTagList",
                 "djmdMixerParam"}) {
        bool retained = false;
        for (auto& row : table(name, {"ID"}, true)) {
            checkCancelled();
            if (!deleted(row)) {
                library->opaqueRecords.push_back({name, std::move(row)});
                retained = true;
            }
        }
        if (retained) {
            issue("native.ancillary_records",
                    "Native ancillary records are preserved without a canonical mapping.",
                    name);
        }
    }
    checkCancelled();
    library->capabilities.metadata = true;
    library->capabilities.orderedPlaylists = true;
    library->capabilities.databaseAnnotations = true;
    library->capabilities.analysisLocators = true;
    library->capabilities.artworkLocators = true;
    library->capabilities.limitations = {
            "DBVersion 6000 structural mapping; no application-version "
            "compatibility claim.",
            "Native cue records retain database milliseconds and 1/150-second "
            "frame fields independently.",
            "ANLZ/database cue precedence, RGB palette mapping and beat grids "
            "await shared-decoder qualification.",
            "Active censor, cue-bank, tag, related-track, sampler and mixer "
            "records are opaque provenance only.",
            "Smart playlist evaluation, canonical application, native "
            "writeback and rekordbox 7 are not enabled."};
    m_result.outcome = m_missingMedia ? DatabaseOutcome::MissingMedia
            : m_result.issues.empty() ? DatabaseOutcome::Success
                                      : DatabaseOutcome::Warning;
    m_result.library = std::move(library);
    return std::move(m_result);
}

} // namespace

DatabaseReadResult readNativeLibrary(sqlite3* privateDatabase, const DatabaseReadContext& context) {
    try {
        return Reader(privateDatabase, context).read();
    } catch (const ReadFailure& failure) {
        return {failure.outcome, {failure.issue}, {}};
    } catch (const std::exception&) {
        return {DatabaseOutcome::Failed,
                {{"native.exception",
                        "Native library reading failed without publishing "
                        "partial records.",
                        {}}},
                {}};
    }
}

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
