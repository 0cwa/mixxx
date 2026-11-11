// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxonelibraryreader.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <djinterop/exceptions.hpp>
#include <djinterop/onelibrary/v1/library.hpp>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace mixxx::rekordbox {
namespace {

struct ReadFailure : std::runtime_error {
    DatabaseOutcome outcome;
    std::string code;

    ReadFailure(DatabaseOutcome result, std::string issue, const char* message)
            : std::runtime_error(message),
              outcome(result),
              code(std::move(issue)) {
    }
};

void checkCancelled(const DatabaseReadContext& context) {
    if (context.isCancelled()) {
        throw ReadFailure(DatabaseOutcome::Cancelled,
                "cancelled",
                "Library reading was cancelled.");
    }
}

class Query final {
  public:
    Query(sqlite3* database, const char* sql, const DatabaseReadContext& context)
            : m_context(context) {
        if (sqlite3_prepare_v2(database, sql, -1, &m_statement, nullptr) != SQLITE_OK) {
            sqlite3_finalize(m_statement);
            m_statement = nullptr;
            throw ReadFailure(DatabaseOutcome::Unsupported,
                    "schema-columns",
                    "The OneLibrary schema does not match the qualified profile.");
        }
        if (!sqlite3_stmt_readonly(m_statement)) {
            sqlite3_finalize(m_statement);
            m_statement = nullptr;
            throw ReadFailure(DatabaseOutcome::Failed,
                    "non-reading-query",
                    "The OneLibrary reader refused a non-reading query.");
        }
        std::set<std::string> columns;
        for (int i = 0; i < sqlite3_column_count(m_statement); ++i) {
            if (!columns.emplace(sqlite3_column_name(m_statement, i)).second) {
                sqlite3_finalize(m_statement);
                m_statement = nullptr;
                throw ReadFailure(DatabaseOutcome::Unsupported,
                        "duplicate-column",
                        "The OneLibrary schema contains ambiguous field names.");
            }
        }
    }
    ~Query() {
        sqlite3_finalize(m_statement);
    }
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    bool next() {
        checkCancelled(m_context);
        const int status = sqlite3_step(m_statement);
        if (status == SQLITE_DONE) {
            return false;
        }
        if (status != SQLITE_ROW) {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "database-read",
                    "The private OneLibrary snapshot could not be read.");
        }
        if (++m_count > m_context.maximumRecords) {
            throw ReadFailure(DatabaseOutcome::Unsupported,
                    "record-limit",
                    "The library exceeds the configured record limit.");
        }
        return true;
    }

    std::optional<std::string> text(const char* name) const {
        const int column = index(name);
        const int type = sqlite3_column_type(m_statement, column);
        if (type == SQLITE_NULL) {
            return std::nullopt;
        }
        if (type != SQLITE_TEXT) {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "text-type",
                    "A OneLibrary text field has an invalid storage type.");
        }
        return value(column);
    }

    std::optional<std::int64_t> integer(const char* name) const {
        const int column = index(name);
        const int type = sqlite3_column_type(m_statement, column);
        if (type == SQLITE_NULL) {
            return std::nullopt;
        }
        if (type != SQLITE_INTEGER) {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "integer-type",
                    "A OneLibrary integer field has an invalid storage type.");
        }
        return sqlite3_column_int64(m_statement, column);
    }

    std::int64_t id(const char* name) const {
        const auto result = integer(name);
        if (!result || *result <= 0) {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "invalid-id",
                    "A OneLibrary record has a missing or invalid identifier.");
        }
        return *result;
    }

    DatabaseProperties properties() const {
        DatabaseProperties values;
        for (int i = 0; i < sqlite3_column_count(m_statement); ++i) {
            values.emplace(sqlite3_column_name(m_statement, i), value(i));
        }
        return values;
    }

  private:
    int index(const char* name) const {
        for (int i = 0; i < sqlite3_column_count(m_statement); ++i) {
            if (std::string(sqlite3_column_name(m_statement, i)) == name) {
                return i;
            }
        }
        throw ReadFailure(DatabaseOutcome::Unsupported,
                "missing-column",
                "A required OneLibrary column is missing.");
    }

    std::optional<std::string> value(int column) const {
        const int type = sqlite3_column_type(m_statement, column);
        if (type == SQLITE_NULL) {
            return std::nullopt;
        }
        if (type == SQLITE_BLOB) {
            throw ReadFailure(DatabaseOutcome::Unsupported,
                    "blob-field",
                    "A OneLibrary field uses an unsupported binary representation.");
        }
        const auto* bytes = sqlite3_column_text(m_statement, column);
        const int size = sqlite3_column_bytes(m_statement, column);
        if (!bytes || size < 0 || static_cast<std::size_t>(size) > m_context.maximumStringBytes) {
            throw ReadFailure(DatabaseOutcome::Unsupported,
                    "text-limit",
                    "A library field exceeds the configured text limit.");
        }
        const std::string result(reinterpret_cast<const char*>(bytes),
                static_cast<std::size_t>(size));
        if (result.find('\0') != std::string::npos) {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "embedded-null",
                    "A library field contains an embedded null character.");
        }
        return result;
    }

    const DatabaseReadContext& m_context;
    sqlite3_stmt* m_statement = nullptr;
    std::size_t m_count = 0;
};

std::filesystem::path utf8Path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::filesystem::path checkedCanonical(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) {
        throw ReadFailure(DatabaseOutcome::Denied,
                "path-access",
                "A selected library path cannot be resolved with the current access.");
    }
    return result;
}

std::optional<std::filesystem::path> devicePath(
        const std::filesystem::path& root,
        const std::optional<std::string>& stored) {
    if (!stored || stored->empty()) {
        return std::nullopt;
    }
    if (stored->find('\\') != std::string::npos || stored->find(':') != std::string::npos) {
        throw ReadFailure(DatabaseOutcome::Unsupported,
                "device-path",
                "A device path is not a supported root-relative path.");
    }
    const auto begin = stored->find_first_not_of('/');
    if (begin == std::string::npos) {
        return std::nullopt;
    }
    const auto relative = utf8Path(stored->substr(begin));
    for (const auto& part : relative) {
        if (part == "..") {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "path-traversal",
                    "A library path escapes its selected device root.");
        }
    }
    const auto base = checkedCanonical(root);
    const auto path = checkedCanonical(base / relative);
    const auto mismatch = std::mismatch(base.begin(), base.end(), path.begin(), path.end());
    if (mismatch.first != base.end()) {
        throw ReadFailure(DatabaseOutcome::Corrupt,
                "path-traversal",
                "A library asset resolves outside its selected device root.");
    }
    return path;
}

bool isFile(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw ReadFailure(DatabaseOutcome::Denied,
                "asset-access",
                "An asset cannot be inspected with the current access.");
    }
    return exists;
}

bool sameFile(const std::filesystem::path& first, const std::filesystem::path& second) {
    if (checkedCanonical(first) == checkedCanonical(second)) {
        return true;
    }
    std::error_code error;
    const bool same = std::filesystem::equivalent(first, second, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw ReadFailure(DatabaseOutcome::Denied,
                "path-access",
                "The private snapshot cannot be distinguished from its source.");
    }
    return same;
}

void verifyTables(sqlite3* database, const DatabaseReadContext& context) {
    const std::set<std::string> required{"content",
            "artist",
            "album",
            "genre",
            "label",
            "key",
            "color",
            "image",
            "cue",
            "playlist",
            "playlist_content",
            "history",
            "history_content",
            "hotCueBankList",
            "hotCueBankList_cue",
            "myTag",
            "myTag_content",
            "menuItem",
            "category",
            "sort",
            "property",
            "recommendedLike"};
    std::set<std::string> found;
    Query tables(database,
            "SELECT name, type, sql FROM sqlite_schema WHERE name NOT LIKE "
            "'sqlite_%'",
            context);
    while (tables.next()) {
        const auto name = tables.text("name");
        if (!name || !required.count(*name)) {
            continue;
        }
        const auto type = tables.text("type");
        const auto sql = tables.text("sql");
        auto normalizedSql = sql.value_or("");
        std::transform(normalizedSql.begin(),
                normalizedSql.end(),
                normalizedSql.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::toupper(value));
                });
        if (type != std::optional<std::string>("table") || !sql ||
                normalizedSql.find("CREATE VIRTUAL TABLE") != std::string::npos) {
            throw ReadFailure(DatabaseOutcome::Unsupported,
                    "schema-table",
                    "A required OneLibrary table has an unsupported definition.");
        }
        found.insert(*name);
    }
    if (found != required) {
        throw ReadFailure(DatabaseOutcome::Unsupported,
                "schema-table",
                "The snapshot is missing required OneLibrary tables.");
    }
}

void preserveOpaque(sqlite3* database,
        const DatabaseReadContext& context,
        DatabaseLibrary* library) {
    for (const auto* table : {"artist",
                 "album",
                 "genre",
                 "label",
                 "key",
                 "color",
                 "image",
                 "cue",
                 "history",
                 "history_content",
                 "hotCueBankList",
                 "hotCueBankList_cue",
                 "myTag",
                 "myTag_content",
                 "recommendedLike",
                 "menuItem",
                 "category",
                 "sort"}) {
        const std::string sql = "SELECT * FROM \"" + std::string(table) + "\"";
        Query rows(database, sql.c_str(), context);
        while (rows.next()) {
            if (library->opaqueRecords.size() >= context.maximumRecords) {
                throw ReadFailure(DatabaseOutcome::Unsupported,
                        "record-limit",
                        "Ancillary records exceed the configured record limit.");
            }
            library->opaqueRecords.push_back({table, rows.properties()});
        }
    }
}

} // namespace

DeviceDatabaseSelection selectDeviceDatabase(
        bool hasLegacy, bool hasOneLibrary, DeviceDatabaseChoice choice) {
    DeviceDatabaseSelection result;
    if ((choice == DeviceDatabaseChoice::Legacy && !hasLegacy) ||
            (choice == DeviceDatabaseChoice::OneLibrary && !hasOneLibrary) ||
            (!hasLegacy && !hasOneLibrary)) {
        result.issues.push_back({"database-unavailable",
                "The selected device library is unavailable.",
                {}});
        return result;
    }
    if (hasLegacy && hasOneLibrary && choice == DeviceDatabaseChoice::RequireExplicit) {
        result.outcome = DatabaseOutcome::Unsupported;
        result.issues.push_back({"dual-library-choice",
                "The device has legacy and OneLibrary databases. Select one "
                "explicitly; their contents may differ.",
                {}});
        return result;
    }
    result.outcome = DatabaseOutcome::Success;
    result.selected = choice == DeviceDatabaseChoice::RequireExplicit
            ? (hasOneLibrary ? DeviceDatabaseChoice::OneLibrary : DeviceDatabaseChoice::Legacy)
            : choice;
    return result;
}

DatabaseReadResult readOneLibrary(const std::filesystem::path& privateDatabasePath,
        const std::string& privatePassphrase,
        sqlite3* privateDatabase,
        const DatabaseReadContext& context) {
    DatabaseReadResult result;
    try {
        checkCancelled(context);
        if (!privateDatabase || context.sourceId.empty() || context.fingerprint.empty() ||
                context.originalDatabasePath.empty() || !context.mediaRoot.is_absolute() ||
                !context.assetRoot.is_absolute() || context.maximumRecords == 0 ||
                context.maximumStringBytes == 0) {
            throw ReadFailure(DatabaseOutcome::Failed,
                    "reader-context",
                    "A stable source identity, snapshot fingerprint and "
                    "original roots are required.");
        }
        const auto* connectionPath = sqlite3_db_filename(privateDatabase, "main");
        if (!connectionPath || !sameFile(privateDatabasePath, utf8Path(connectionPath)) ||
                sameFile(privateDatabasePath, context.originalDatabasePath)) {
            throw ReadFailure(DatabaseOutcome::Failed,
                    "private-snapshot",
                    "OneLibrary reading requires a separate completed private snapshot.");
        }
        verifyTables(privateDatabase, context);
        auto output = std::make_shared<DatabaseLibrary>();
        output->context = context;
        output->format = "rekordbox-onelibrary";
        output->schemaProfile = "onelibrary-v1-db1000";
        Query property(privateDatabase, "SELECT * FROM property", context);
        if (!property.next() || property.text("dbVersion") != std::optional<std::string>("1000")) {
            throw ReadFailure(DatabaseOutcome::Unsupported,
                    "schema-version",
                    "Only the OneLibrary dbVersion 1000 profile is implemented.");
        }
        output->properties = property.properties();
        if (property.next()) {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "property-rows",
                    "The OneLibrary property table must contain exactly one row.");
        }

        const auto maintained = djinterop::onelibrary::v1::library::load(
                privateDatabasePath, privatePassphrase);
        if (maintained.property().get_db_version() != std::optional<std::string>("1000")) {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "snapshot-disagreement",
                    "The private snapshot changed while its readers were opening.");
        }
        Query tracks(privateDatabase,
                "SELECT c.*, artist.name AS resolved_artist, composer.name AS "
                "resolved_composer, "
                "remixer.name AS resolved_remixer, album.name AS "
                "resolved_album, "
                "albumArtist.name AS resolved_album_artist, genre.name AS "
                "resolved_genre, "
                "label.name AS resolved_label, k.name AS resolved_key, "
                "image.path AS resolved_artwork, "
                "artist.artist_id AS resolved_artist_id, composer.artist_id AS "
                "resolved_composer_id, "
                "remixer.artist_id AS resolved_remixer_id, album.album_id AS "
                "resolved_album_id, "
                "genre.genre_id AS resolved_genre_id, label.label_id AS "
                "resolved_label_id, "
                "k.key_id AS resolved_key_id, image.image_id AS "
                "resolved_image_id, album.artist_id AS referenced_album_artist_id, "
                "albumArtist.artist_id AS resolved_album_artist_id, "
                "color.color_id AS resolved_color_id, color.name AS resolved_color_name "
                "FROM content c LEFT JOIN artist ON "
                "artist.artist_id=c.artist_id_artist "
                "LEFT JOIN artist composer ON "
                "composer.artist_id=c.artist_id_composer "
                "LEFT JOIN artist remixer ON "
                "remixer.artist_id=c.artist_id_remixer "
                "LEFT JOIN album ON album.album_id=c.album_id "
                "LEFT JOIN artist albumArtist ON "
                "albumArtist.artist_id=album.artist_id "
                "LEFT JOIN genre ON genre.genre_id=c.genre_id LEFT JOIN label "
                "ON label.label_id=c.label_id "
                "LEFT JOIN \"key\" k ON k.key_id=c.key_id LEFT JOIN image ON "
                "image.image_id=c.image_id "
                "LEFT JOIN color ON color.color_id=c.color_id "
                "ORDER BY c.content_id",
                context);
        std::set<std::string> trackIds;
        bool missingMedia = false;
        while (tracks.next()) {
            const auto id = tracks.id("content_id");
            DatabaseTrack track;
            track.sourceTrackId = std::to_string(id);
            if (!trackIds.insert(track.sourceTrackId).second) {
                throw ReadFailure(DatabaseOutcome::Corrupt,
                        "duplicate-id",
                        "Track identifiers or lookup joins are ambiguous.");
            }
            track.properties = tracks.properties();
            for (const auto& lookup : {std::pair{"artist_id_artist", "resolved_artist_id"},
                         {"artist_id_composer", "resolved_composer_id"},
                         {"artist_id_remixer", "resolved_remixer_id"},
                         {"album_id", "resolved_album_id"},
                         {"genre_id", "resolved_genre_id"},
                         {"label_id", "resolved_label_id"},
                         {"key_id", "resolved_key_id"},
                         {"image_id", "resolved_image_id"},
                         {"referenced_album_artist_id", "resolved_album_artist_id"},
                         {"color_id", "resolved_color_id"}}) {
                const auto source = tracks.integer(lookup.first);
                if (source && *source != 0 && source != tracks.integer(lookup.second)) {
                    throw ReadFailure(DatabaseOutcome::Corrupt,
                            "missing-lookup",
                            "A OneLibrary metadata reference does not resolve.");
                }
            }
            track.title = tracks.text("title");
            track.artist = tracks.text("resolved_artist");
            track.composer = tracks.text("resolved_composer");
            track.album = tracks.text("resolved_album");
            track.albumArtist = tracks.text("resolved_album_artist");
            track.remixer = tracks.text("resolved_remixer");
            track.genre = tracks.text("resolved_genre");
            track.label = tracks.text("resolved_label");
            track.key = tracks.text("resolved_key");
            track.comment = tracks.text("djComment");
            track.dateAdded = tracks.text("dateAdded");
            track.trackNumber = tracks.integer("trackNo");
            track.discNumber = tracks.integer("discNo");
            track.year = tracks.integer("releaseYear");
            track.bitrate = tracks.integer("bitrate");
            track.sampleRate = tracks.integer("samplingRate");
            track.rating = tracks.integer("rating");
            track.playCount = tracks.integer("djPlayCount");
            track.colorIndex = tracks.integer("color_id");
            const auto bpm = tracks.integer("bpmx100");
            const auto seconds = tracks.integer("length");
            if ((bpm && *bpm < 0) ||
                    (seconds &&
                            (*seconds < 0 ||
                                    *seconds > std::numeric_limits<
                                                       std::int64_t>::max() /
                                                    1000)) ||
                    (track.rating &&
                            (*track.rating < 0 || *track.rating > 5))) {
                throw ReadFailure(DatabaseOutcome::Unsupported,
                        "numeric-range",
                        "OneLibrary timing or rating values exceed the qualified profile.");
            }
            for (const auto& value : {track.trackNumber,
                         track.discNumber,
                         track.year,
                         track.bitrate,
                         track.sampleRate,
                         track.playCount}) {
                if (value && *value < 0) {
                    throw ReadFailure(DatabaseOutcome::Corrupt,
                            "negative-metadata",
                            "A OneLibrary metadata value is unexpectedly negative.");
                }
            }
            const auto api = maintained.content().get(id);
            if (!api || api->id != id) {
                throw ReadFailure(DatabaseOutcome::Corrupt,
                        "snapshot-disagreement",
                        "The OneLibrary readers disagree about a track.");
            }
            if (bpm != api->bpm_x100 ||
                    (seconds &&
                            (!api->length ||
                                    api->length->count() != *seconds))) {
                throw ReadFailure(DatabaseOutcome::Corrupt,
                        "snapshot-disagreement",
                        "The OneLibrary readers disagree about track timing.");
            }
            if (bpm) {
                track.bpm = static_cast<double>(*bpm) / 100.0;
            }
            if (seconds) {
                track.durationMilliseconds = *seconds * 1000;
            }
            track.originalMediaPath = tracks.text("path");
            track.originalAnalysisPath = tracks.text("analysisDataFilePath");
            track.originalArtworkPath = tracks.text("resolved_artwork");
            track.mediaPath = devicePath(context.mediaRoot, track.originalMediaPath);
            track.analysisPath = devicePath(context.assetRoot, track.originalAnalysisPath);
            track.artworkPath = devicePath(context.assetRoot, track.originalArtworkPath);
            track.mediaKind = track.mediaPath && isFile(*track.mediaPath)
                    ? DatabaseMediaKind::Local
                    : DatabaseMediaKind::Missing;
            if (track.mediaKind == DatabaseMediaKind::Missing) {
                missingMedia = true;
                result.issues.push_back({"missing-media",
                        "A source track has no available local audio file.",
                        track.sourceTrackId});
            }
            if (track.analysisPath && !isFile(*track.analysisPath)) {
                result.issues.push_back({"missing-analysis",
                        "A source analysis file is unavailable.",
                        track.sourceTrackId});
            }
            if (track.artworkPath && !isFile(*track.artworkPath)) {
                result.issues.push_back({"missing-artwork",
                        "A source artwork file is unavailable.",
                        track.sourceTrackId});
            }
            output->tracks.push_back(std::move(track));
        }

        std::map<std::int64_t, DatabasePlaylist> playlists;
        Query playlistRows(privateDatabase,
                "SELECT * FROM playlist ORDER BY sequenceNo, playlist_id",
                context);
        while (playlistRows.next()) {
            const auto id = playlistRows.id("playlist_id");
            const auto parent = playlistRows.integer("playlist_id_parent");
            const auto sequence = playlistRows.integer("sequenceNo");
            const auto name = playlistRows.text("name");
            const auto attribute = playlistRows.integer("attribute");
            if (!sequence || *sequence < 0 || !name || !attribute || (parent && *parent < 0)) {
                throw ReadFailure(DatabaseOutcome::Unsupported,
                        "playlist-profile",
                        "A playlist lacks supported name, ordering or kind fields.");
            }
            DatabasePlaylist playlist;
            playlist.id = std::to_string(id);
            playlist.name = *name;
            playlist.position = *sequence;
            if (parent && *parent != 0) {
                playlist.parentId = std::to_string(*parent);
            }
            playlist.kind = *attribute == 1 ? DatabasePlaylistKind::Folder
                    : *attribute == 0       ? DatabasePlaylistKind::Regular
                                            : DatabasePlaylistKind::Unsupported;
            if (playlist.kind == DatabasePlaylistKind::Unsupported) {
                result.issues.push_back({"playlist-kind",
                        "An advanced playlist kind is preserved without "
                        "evaluation.",
                        playlist.id});
            }
            playlist.properties = playlistRows.properties();
            if (!playlists.emplace(id, std::move(playlist)).second) {
                throw ReadFailure(DatabaseOutcome::Corrupt,
                        "duplicate-id",
                        "Playlist identifiers are ambiguous.");
            }
        }
        Query entries(privateDatabase,
                "SELECT rowid AS membership_rowid, playlist_id, content_id, sequenceNo "
                "FROM playlist_content ORDER BY playlist_id, sequenceNo, rowid",
                context);
        while (entries.next()) {
            const auto playlistId = entries.id("playlist_id");
            const auto trackId = std::to_string(entries.id("content_id"));
            const auto found = playlists.find(playlistId);
            const auto sequence = entries.integer("sequenceNo");
            if (found == playlists.end() || !trackIds.count(trackId) ||
                    !sequence || *sequence < 0 ||
                    found->second.kind == DatabasePlaylistKind::Folder) {
                throw ReadFailure(DatabaseOutcome::Corrupt,
                        "playlist-membership",
                        "A playlist entry has an invalid reference, order or folder parent.");
            }
            found->second.entries.push_back(
                    {"rowid:" + std::to_string(entries.id("membership_rowid")),
                            trackId,
                            *sequence,
                            entries.properties()});
        }

        const auto apiPlaylists = maintained.playlist();
        auto pending = apiPlaylists.root_ids();
        std::reverse(pending.begin(), pending.end());
        std::set<std::int64_t> visited;
        while (!pending.empty()) {
            checkCancelled(context);
            const auto id = pending.back();
            pending.pop_back();
            const auto found = playlists.find(id);
            if (found == playlists.end() || !visited.insert(id).second) {
                throw ReadFailure(DatabaseOutcome::Corrupt,
                        "playlist-cycle",
                        "The OneLibrary playlist hierarchy is cyclic or inconsistent.");
            }
            const auto members = apiPlaylists.track_ids(id);
            if (members.size() != found->second.entries.size()) {
                throw ReadFailure(DatabaseOutcome::Corrupt,
                        "snapshot-disagreement",
                        "The OneLibrary readers disagree about playlist membership.");
            }
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (std::to_string(members[i]) != found->second.entries[i].sourceTrackId) {
                    throw ReadFailure(DatabaseOutcome::Corrupt,
                            "snapshot-disagreement",
                            "The OneLibrary readers disagree about playlist order.");
                }
            }
            auto children = apiPlaylists.child_ids(id);
            if (!children.empty() && found->second.kind != DatabasePlaylistKind::Folder) {
                throw ReadFailure(DatabaseOutcome::Corrupt,
                        "playlist-parent",
                        "A non-folder playlist contains child playlists.");
            }
            pending.insert(pending.end(), children.rbegin(), children.rend());
            output->playlists.push_back(std::move(found->second));
        }
        if (visited.size() != playlists.size()) {
            throw ReadFailure(DatabaseOutcome::Corrupt,
                    "playlist-orphan",
                    "A playlist has a missing parent or a cyclic ancestry.");
        }
        preserveOpaque(privateDatabase, context, output.get());
        output->capabilities.metadata = true;
        output->capabilities.orderedPlaylists = true;
        output->capabilities.analysisLocators = true;
        output->capabilities.artworkLocators = true;
        output->capabilities.limitations = {
                "ANLZ cues, loops, grids and assets require the shared decoder "
                "and composed qualification.",
                "Unmapped lookup, cue, history, hot-cue-bank, tag and device-setting "
                "records are preserved as opaque data.",
                "Canonical application and complete modern/native read "
                "acceptance remain gated.",
                "The source identity is supplied by the registry; masterDbId "
                "is not a device identity."};
        result.issues.push_back({"performance-unqualified",
                "Metadata and playlist reading does not qualify cue, loop or beat-grid import.",
                {}});
        checkCancelled(context);
        result.outcome = missingMedia ? DatabaseOutcome::MissingMedia : DatabaseOutcome::Warning;
        result.library = std::move(output);
    } catch (const ReadFailure& failure) {
        result.outcome = failure.outcome;
        result.library.reset();
        result.issues = {{failure.code, failure.what(), {}}};
    } catch (const djinterop::unsupported_operation&) {
        result.outcome = DatabaseOutcome::Unsupported;
        result.issues = {{"dependency-capability",
                "This dependency build does not provide the required "
                "OneLibrary and SQLCipher support.",
                {}}};
    } catch (const djinterop::database_inconsistency&) {
        result.outcome = DatabaseOutcome::Unsupported;
        result.issues = {{"dependency-schema",
                "The private snapshot does not match the dependency's qualified OneLibrary schema.",
                {}}};
    } catch (const djinterop::unsupported_database&) {
        result.outcome = DatabaseOutcome::Unsupported;
        result.issues = {{"dependency-profile",
                "The dependency cannot open this private snapshot with its selected profile.",
                {}}};
    } catch (const djinterop::database_not_found&) {
        result.outcome = DatabaseOutcome::Unavailable;
        result.issues = {{"private-snapshot-unavailable",
                "The private snapshot is no longer available.",
                {}}};
    } catch (const std::exception&) {
        result.outcome = context.isCancelled() ? DatabaseOutcome::Cancelled
                                               : DatabaseOutcome::Failed;
        result.library.reset();
        result.issues = {{"dependency-read",
                "The qualified OneLibrary dependency could not read the "
                "private snapshot.",
                {}}};
    }
    return result;
}

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
