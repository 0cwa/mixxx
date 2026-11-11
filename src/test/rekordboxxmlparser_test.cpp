#include "library/rekordbox/rekordboxxmlparser.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>
#include <algorithm>

#include "test/rekordboxfixtures/xmlfixtures.h"

namespace {

std::string diagnostics(const mixxx::rekordbox::Library& library) {
    QStringList messages;
    for (const auto& diagnostic : library.diagnostics) {
        messages.append(QStringLiteral("%1 at %2:%3: %4")
                        .arg(diagnostic.severity ==
                                                mixxx::rekordbox::Diagnostic::Severity::Error
                                        ? QStringLiteral("error")
                                        : QStringLiteral("warning"))
                        .arg(diagnostic.line)
                        .arg(diagnostic.column)
                        .arg(diagnostic.message));
    }
    return messages.join(QLatin1Char('\n')).toStdString();
}

mixxx::rekordbox::Library parse(const QByteArray& data) {
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    return mixxx::rekordbox::parseXml(&buffer);
}

} // namespace

TEST(RekordboxXmlParserTest, ParsesCollectionPlaylistsBeatgridCuesAndArtwork) {
    const auto library = parse(R"xml(
<DJ_PLAYLISTS Version="1.0.0">
 <PRODUCT Name="rekordbox" Version="6.6.2" Company="Pioneer DJ"/>
 <COLLECTION Entries="1"><TRACK TrackID="7" Name="Song" Artist="Artist" Location="file://localhost/song.mp3" ArtworkPath="cover.jpg" AverageBpm="128.0" TotalTime="240">
  <TEMPO Inizio="0" Bpm="128"/><TEMPO Inizio="4" Bpm="128"/>
  <POSITION_MARK Name="Intro" Type="0" Start="1.5" End="1.5" Num="1" Red="255" Green="0" Blue="0"/>
  <POSITION_MARK Name="Loop" Type="4" Start="8" End="12" Num="2"/>
 </TRACK></COLLECTION>
 <PLAYLISTS><NODE Type="0" Name="ROOT"><NODE Type="1" Name="Set"><TRACK Key="7"/></NODE></NODE></PLAYLISTS>
</DJ_PLAYLISTS>)xml");

    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    ASSERT_EQ(library.version, "1.0.0");
    ASSERT_EQ(library.tracks.size(), 1);
    EXPECT_EQ(library.tracks[0].location,
            mixxx::rekordbox::normalizeLocation("file://localhost/song.mp3"));
    EXPECT_EQ(library.tracks[0].artworkReference, "cover.jpg");
    ASSERT_EQ(library.tracks[0].beatgrid.size(), 2);
    ASSERT_EQ(library.tracks[0].cues.size(), 2);
    const QJsonDocument annotations = QJsonDocument::fromJson(
            mixxx::rekordbox::serializeTrackAnnotations(library.tracks[0]));
    ASSERT_TRUE(annotations.isObject());
    EXPECT_EQ(annotations.object().value("beatgrid").toArray().size(), 2);
    EXPECT_EQ(annotations.object().value("cues").toArray().size(), 2);
    ASSERT_EQ(library.playlists.children[0].children[0].trackIds[0], 7);
}

TEST(RekordboxXmlParserTest, RetainsUnlistedTracksAndReportsMalformedInput) {
    const auto library = parse(R"xml(
<DJ_PLAYLISTS><COLLECTION Entries="1"><TRACK TrackID="8" Name="Unlisted"/></COLLECTION>
<PLAYLISTS><NODE Name="ROOT"></PLAYLISTS>
)xml");

    ASSERT_EQ(library.tracks.size(), 1);
    EXPECT_EQ(library.tracks[0].id, 8);
    EXPECT_TRUE(library.hasErrors());
    ASSERT_FALSE(library.diagnostics.isEmpty());
    const auto error = std::find_if(library.diagnostics.cbegin(),
            library.diagnostics.cend(),
            [](const auto& diagnostic) {
                return diagnostic.severity ==
                        mixxx::rekordbox::Diagnostic::Severity::Error;
            });
    ASSERT_TRUE(error != library.diagnostics.cend()) << diagnostics(library);
    EXPECT_GT(error->line, 0);
}

TEST(RekordboxXmlParserTest, NullDeviceIsSafe) {
    const auto library = mixxx::rekordbox::parseXml(nullptr);
    EXPECT_TRUE(library.hasErrors());
    EXPECT_TRUE(library.tracks.isEmpty());
}

// Prepared by ChatGPT: prove mutable DTO copies cannot reuse stale immutable
// validation indexes after replacing data without changing vector sizes.
TEST(RekordboxXmlParserTest, MutatedCopiesInvalidateDecodedValidationIndexes) {
    QByteArray xml(R"xml(<DJ_PLAYLISTS><COLLECTION>
<TRACK TrackID="1" Location="/same.wav"/>
<TRACK TrackID="2" Location="/same.wav"/>
</COLLECTION></DJ_PLAYLISTS>)xml");
    QBuffer buffer(&xml);
    ASSERT_TRUE(buffer.open(QIODevice::ReadOnly));
    const auto decoded = mixxx::rekordbox::decodeXml(&buffer);
    ASSERT_TRUE(decoded.snapshot);
    EXPECT_EQ(decoded.snapshot->sourceIdentityMatches(0), 1);
    EXPECT_EQ(decoded.snapshot->sourceIdentityMatches(1), 1);
    EXPECT_FALSE(decoded.snapshot->hasErrors());
    ASSERT_FALSE(decoded.snapshot->diagnostics.isEmpty());
    const auto fingerprint = decoded.snapshot->diagnosticsFingerprint();
    auto changed = *decoded.snapshot;
    changed.tracks[1].id = 1;
    changed.diagnostics[0].severity = mixxx::rekordbox::Diagnostic::Severity::Error;
    EXPECT_EQ(changed.sourceIdentityMatches(0), 2);
    EXPECT_TRUE(changed.hasErrors());
    EXPECT_NE(changed.diagnosticsFingerprint(), fingerprint);
    EXPECT_EQ(decoded.snapshot->sourceIdentityMatches(0), 1);
    EXPECT_FALSE(decoded.snapshot->hasErrors());
    EXPECT_EQ(decoded.snapshot->diagnosticsFingerprint(), fingerprint);
}

TEST(RekordboxXmlParserTest, ConstructedRecordsRetainDefensiveIdentityAndDiagnosticValidation) {
    mixxx::rekordbox::Library library;
    mixxx::rekordbox::Track first;
    first.location = QStringLiteral("/same.wav");
    library.tracks = {first, first};
    EXPECT_EQ(library.sourceIdentityMatches(0), 2);
    EXPECT_EQ(library.sourceIdentityMatches(-1), 0);
    EXPECT_EQ(library.sourceIdentityMatches(2), 0);
    library.tracks[1].location = QStringLiteral("/other.wav");
    EXPECT_EQ(library.sourceIdentityMatches(0), 1);
    const auto fingerprint = library.diagnosticsFingerprint();
    library.diagnostics.push_back({mixxx::rekordbox::Diagnostic::Severity::Error,
            QStringLiteral("Constructed error"),
            1,
            2});
    EXPECT_TRUE(library.hasErrors());
    EXPECT_NE(library.diagnosticsFingerprint(), fingerprint);
}
// End of ChatGPT-generated validation-index regressions.

TEST(RekordboxXmlParserTest, ReportsMissingLocationsAndDuplicateIdentities) {
    const auto library = parse(R"xml(
<DJ_PLAYLISTS><COLLECTION>
 <TRACK TrackID="1" Name="Missing"/>
 <TRACK TrackID="1" Name="Duplicate" Location="file://localhost/song.mp3"/>
 <TRACK TrackID="2" Name="Same location" Location="file://localhost/song.mp3"/>
</COLLECTION></DJ_PLAYLISTS>)xml");

    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    ASSERT_GE(library.diagnostics.size(), 3);
}

TEST(RekordboxXmlParserTest, NormalizesLocalFileLocationsBeforeDuplicateDetection) {
    const auto library = parse(R"xml(
<DJ_PLAYLISTS><COLLECTION>
 <TRACK TrackID="1" Location="file://localhost/song.mp3"/>
 <TRACK TrackID="2" Location="/song.mp3"/>
</COLLECTION></DJ_PLAYLISTS>)xml");

    ASSERT_EQ(library.tracks.size(), 2);
    EXPECT_EQ(library.tracks[0].location, "/song.mp3");
    EXPECT_EQ(library.tracks[1].location, "/song.mp3");
    EXPECT_FALSE(library.tracks[0].location.isEmpty());
    EXPECT_EQ(library.tracks[0].location, library.tracks[1].location);
    EXPECT_TRUE(std::any_of(library.diagnostics.cbegin(),
            library.diagnostics.cend(),
            [](const auto& diagnostic) {
                return diagnostic.message ==
                        "Duplicate track Location: /song.mp3";
            }));
}

TEST(RekordboxXmlParserTest, PreservesNonLocalLocations) {
    EXPECT_EQ(mixxx::rekordbox::normalizeLocation("file://localhost/song.mp3"),
            "/song.mp3");
    EXPECT_EQ(mixxx::rekordbox::normalizeLocation("/song.mp3"), "/song.mp3");
    EXPECT_EQ(mixxx::rekordbox::normalizeLocation("https://example.com/song.mp3"),
            "https://example.com/song.mp3");
    EXPECT_EQ(mixxx::rekordbox::normalizeLocation("file://remote/song.mp3"),
            "file://remote/song.mp3");
}

#ifdef Q_OS_WIN
TEST(RekordboxXmlParserTest, NormalizesWindowsLocalhostFileLocations) {
    const QString location = QStringLiteral("file://localhost/C:/song.mp3");
    EXPECT_EQ(mixxx::rekordbox::normalizeLocation(location),
            QDir::cleanPath(QStringLiteral("C:/song.mp3")));
}
#endif

TEST(RekordboxXmlParserTest, PreservesRemoteFileLocationsDuringDuplicateDetection) {
    const auto library = parse(R"xml(
<DJ_PLAYLISTS><COLLECTION>
 <TRACK TrackID="1" Location="file://remote/song.mp3"/>
 <TRACK TrackID="2" Location="file://remote/song.mp3"/>
</COLLECTION></DJ_PLAYLISTS>)xml");

    ASSERT_EQ(library.tracks.size(), 2);
    EXPECT_EQ(library.tracks[0].location, "file://remote/song.mp3");
    EXPECT_EQ(library.tracks[1].location, "file://remote/song.mp3");
    EXPECT_TRUE(std::any_of(library.diagnostics.cbegin(),
            library.diagnostics.cend(),
            [](const auto& diagnostic) {
                return diagnostic.message ==
                        "Duplicate track Location: file://remote/song.mp3";
            }));
}

TEST(RekordboxXmlParserTest, AcceptsCompleteDocumentWithAndWithoutTrailingWhitespace) {
    for (const auto& suffix : {QByteArray(), QByteArray("\n"), QByteArray("\r\n \t")}) {
        SCOPED_TRACE(suffix.toStdString());
        const auto library = parse("<DJ_PLAYLISTS/>" + suffix);
        EXPECT_FALSE(library.hasErrors()) << diagnostics(library);
    }
}

TEST(RekordboxXmlParserTest, AcceptsEmptySectionsAndTrailingComments) {
    const auto library = parse(
            "<DJ_PLAYLISTS><COLLECTION/><PLAYLISTS/></DJ_PLAYLISTS>"
            "<!-- exported --><?end complete?>");
    EXPECT_FALSE(library.hasErrors()) << diagnostics(library);
    EXPECT_TRUE(library.tracks.isEmpty());
    EXPECT_TRUE(library.playlists.children.isEmpty());
}

TEST(RekordboxXmlParserTest, RejectsTruncationAtEveryDocumentLevel) {
    for (const auto* input : {"",
                 "<",
                 "<DJ_PLAYLISTS>",
                 "<DJ_PLAYLISTS><COLLECTION>",
                 "<DJ_PLAYLISTS><COLLECTION><TRACK "
                 "Location=\"file://localhost/a\">",
                 "<DJ_PLAYLISTS><PLAYLISTS><NODE Type=\"0\">"}) {
        SCOPED_TRACE(input);
        const auto library = parse(input);
        EXPECT_TRUE(library.hasErrors()) << diagnostics(library);
    }
}

TEST(RekordboxXmlParserTest, RejectsExtraRootsAndMalformedTrailingContent) {
    for (const auto* suffix : {"<DJ_PLAYLISTS/>",
                 "<OTHER/>",
                 "garbage",
                 "<",
                 "&bad;",
                 "<!-- unfinished",
                 "<?unfinished",
                 "<?xml version=\"1.0\"?>"}) {
        SCOPED_TRACE(suffix);
        const auto library = parse(QByteArray("<DJ_PLAYLISTS/>") + suffix);
        EXPECT_TRUE(library.hasErrors()) << diagnostics(library);
        ASSERT_FALSE(library.diagnostics.isEmpty());
        EXPECT_GT(library.diagnostics.back().line, 0);
        EXPECT_GT(library.diagnostics.back().column, 0);
    }
}

TEST(RekordboxXmlParserTest, RejectsWrongRoot) {
    const auto library = parse("<OTHER><DJ_PLAYLISTS/></OTHER>");
    EXPECT_TRUE(library.hasErrors()) << diagnostics(library);
}

TEST(RekordboxXmlParserTest, ReadsProductChildMetadata) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS Version="1.0.0">
        <PRODUCT Name="rekordbox" Version="6.6.2" Company="AlphaTheta"/>
        <COLLECTION/><PLAYLISTS/></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    EXPECT_EQ(library.product, "rekordbox");
    EXPECT_EQ(library.productVersion, "6.6.2");
    EXPECT_EQ(library.productCompany, "AlphaTheta");
}

TEST(RekordboxXmlParserTest, PreservesRepeatedPlaylistOrderAndDuplicateNames) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION>
        <TRACK TrackID="101" Location="file://localhost/a.mp3"/>
        <TRACK TrackID="202" Location="file://localhost/b.mp3"/>
        </COLLECTION><PLAYLISTS><NODE Type="0" Name="ROOT">
        <NODE Type="1" Name="Same%_--&gt;" KeyType="0" Entries="3">
        <TRACK Key="202"/><TRACK Key="101"/><TRACK Key="202"/></NODE>
        <NODE Type="1" Name="Same%_--&gt;" KeyType="0" Entries="0"/>
        <NODE Type="0" Name="Empty folder" Count="0"/>
        </NODE></PLAYLISTS></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    ASSERT_EQ(library.playlists.children.size(), 1);
    const auto& root = library.playlists.children.front();
    ASSERT_EQ(root.children.size(), 3);
    EXPECT_EQ(root.children[0].trackIds, (QVector<int>{202, 101, 202}));
    ASSERT_EQ(root.children[0].entries.size(), 3);
    EXPECT_EQ(root.children[0].entries[2].ordinal, 2);
    EXPECT_NE(root.children[0].id, root.children[1].id);
    EXPECT_EQ(root.children[0].parentId, root.id);
    EXPECT_EQ(root.children[0].name, root.children[1].name);
    EXPECT_TRUE(root.children[1].trackIds.isEmpty());
    EXPECT_EQ(root.children[1].type, 1);
    EXPECT_EQ(root.children[2].type, 0);
    EXPECT_TRUE(root.children[2].children.isEmpty());
}

TEST(RekordboxXmlParserTest, ResolvesLocationKeysAfterCollectionWithUnicodeAndPercent) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><PLAYLISTS>
        <NODE Type="1" Name="Locations" KeyType="1">
        <TRACK Key="file://localhost/music/%E9%9B%AA%20%2520%25.mp3"/>
        <TRACK Key="/music/雪 %20%.mp3"/></NODE></PLAYLISTS><COLLECTION>
        <TRACK TrackID="202" Location="file://localhost/music/%E9%9B%AA%20%2520%25.mp3"/>
        </COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    ASSERT_EQ(library.tracks.size(), 1);
    EXPECT_EQ(library.tracks.front().location, QString::fromUtf8("/music/雪 %20%.mp3"));
    EXPECT_EQ(library.playlists.children.front().trackIds, (QVector<int>{202, 202}));
}

TEST(RekordboxXmlParserTest, KeepsUnresolvedOccurrencesWithoutGuessingAmbiguousKeys) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION>
        <TRACK TrackID="1" Location="/same.mp3"/>
        <TRACK TrackID="2" Location="/same.mp3"/>
        </COLLECTION><PLAYLISTS><NODE Type="1" KeyType="1" Name="Ambiguous">
        <TRACK Key="/same.mp3"/><TRACK Key="/missing.mp3"/>
        </NODE></PLAYLISTS></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    const auto& playlist = library.playlists.children.front();
    ASSERT_EQ(playlist.entries.size(), 2);
    EXPECT_FALSE(playlist.entries[0].trackId);
    EXPECT_FALSE(playlist.entries[1].trackId);
    EXPECT_EQ(playlist.entries[0].key, "/same.mp3");
    EXPECT_TRUE(playlist.trackIds.isEmpty());
    EXPECT_GE(library.diagnostics.size(), 3);
}

TEST(RekordboxXmlParserTest, MapsAllDocumentedRatingsToStars) {
    QByteArray xml("<DJ_PLAYLISTS><COLLECTION>");
    for (int stars = 0; stars <= 5; ++stars) {
        xml += QStringLiteral("<TRACK TrackID=\"%1\" Location=\"/%1.mp3\" Rating=\"%2\"/>")
                       .arg(stars)
                       .arg(stars * 51)
                       .toUtf8();
    }
    const auto library = parse(xml + "</COLLECTION></DJ_PLAYLISTS>");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    ASSERT_EQ(library.tracks.size(), 6);
    for (int stars = 0; stars <= 5; ++stars) {
        EXPECT_EQ(library.tracks[stars].rating, stars);
        EXPECT_EQ(library.tracks[stars].attributes.value("Rating"), QString::number(stars * 51));
    }
}

TEST(RekordboxXmlParserTest, RetainsNumericCueRolesSlotsLoopsAndDistinctSourceOrdinals) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION><TRACK TrackID="1" Location="/a">
        <POSITION_MARK Name="memory" Type="0" Start="4" Num="-1" Red="255" Green="0" Blue="0"/>
        <POSITION_MARK Name="hot" Type="0" Start="4" Num="0" Red="0" Green="0" Blue="255"/>
        <POSITION_MARK Name="fade in" Type="1" Start="1" Num="-1"/>
        <POSITION_MARK Name="fade out" Type="2" Start="10" Num="-1"/>
        <POSITION_MARK Name="load" Type="3" Start="0" Num="-1"/>
        <POSITION_MARK Name="hot loop" Type="4" Start="4" End="6" Num="2"/>
        <POSITION_MARK Name="memory loop" Type="4" Start="8" End="10" Num="-1"/>
        </TRACK></COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    const auto& track = library.tracks.front();
    ASSERT_EQ(track.cues.size(), 7);
    EXPECT_EQ(track.cues[0].number, -1);
    EXPECT_FALSE(track.cues[0].hotCue);
    EXPECT_EQ(track.cues[1].hotCue, 0);
    EXPECT_EQ(track.cues[0].start, track.cues[1].start);
    EXPECT_NE(track.cues[0].color, track.cues[1].color);
    EXPECT_NE(track.cues[0].sourceOrdinal, track.cues[1].sourceOrdinal);
    EXPECT_EQ(track.cues[2].type, "fade-in");
    EXPECT_EQ(track.cues[3].type, "fade-out");
    EXPECT_EQ(track.cues[4].type, "load");
    EXPECT_EQ(track.cues[5].type, "loop");
    EXPECT_EQ(track.cues[5].hotCue, 2);
    EXPECT_EQ(track.cues[5].end, 6);
    EXPECT_FALSE(track.cues[6].hotCue);
    const auto cues = QJsonDocument::fromJson(
            mixxx::rekordbox::serializeTrackAnnotations(track))
                              .object()
                              .value("cues")
                              .toArray();
    EXPECT_EQ(cues[5].toObject().value("hotCue").toInt(), 2);
    EXPECT_EQ(cues[5].toObject().value("sourceType").toString(), "4");
    EXPECT_EQ(cues[5].toObject().value("sourceOrdinal").toInt(), 5);
    EXPECT_FALSE(cues[0].toObject().contains("hotCue"));
}

TEST(RekordboxXmlParserTest, DistinguishesAbsentCueFieldsFromZeroAndEmpty) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION><TRACK TrackID="1" Location="/a">
        <POSITION_MARK Type="0" Start="0"/>
        <POSITION_MARK Name="" Type="0" Start="0" End="0" Num="0" Red="0" Green="0" Blue="0"/>
        </TRACK></COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    const auto& track = library.tracks.front();
    EXPECT_FALSE(track.cues[0].number);
    EXPECT_FALSE(track.cues[0].hasEnd);
    EXPECT_TRUE(track.cues[1].hasEnd);
    EXPECT_EQ(track.cues[1].color, "0,0,0");
    EXPECT_FALSE(track.cues[0].attributes.contains("Name"));
    EXPECT_TRUE(track.cues[1].attributes.contains("Name"));
    const auto cues = QJsonDocument::fromJson(
            mixxx::rekordbox::serializeTrackAnnotations(track))
                              .object()
                              .value("cues")
                              .toArray();
    EXPECT_FALSE(cues[0].toObject().contains("end"));
    EXPECT_FALSE(cues[0].toObject().contains("number"));
    EXPECT_TRUE(cues[1].toObject().contains("end"));
    EXPECT_TRUE(cues[1].toObject().contains("number"));
}

TEST(RekordboxXmlParserTest, DoesNotInventSemanticsForUnknownMarkTypes) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION><TRACK TrackID="1" Location="/a">
        <POSITION_MARK Type="99" Start="0" Num="-1" FutureField="retained"/>
        </TRACK></COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    const auto& cue = library.tracks.front().cues.front();
    EXPECT_FALSE(cue.supported);
    EXPECT_EQ(cue.type, "unsupported");
    EXPECT_EQ(cue.attributes.value("FutureField"), "retained");
    EXPECT_FALSE(library.diagnostics.isEmpty());
}

TEST(RekordboxXmlParserTest, RejectsInvalidCueTimingAndSlots) {
    for (const auto* mark : {"Type=\"0\" Start=\"nan\"",
                 "Type=\"0\" Start=\"inf\"",
                 "Type=\"0\" Start=\"-1\"",
                 "Type=\"0\"",
                 "Type=\"4\" Start=\"4\"",
                 "Type=\"4\" Start=\"4\" End=\"4\"",
                 "Type=\"4\" Start=\"4\" End=\"nan\"",
                 "Type=\"0\" Start=\"0\" Num=\"-2\"",
                 "Type=\"0\" Start=\"0\" Num=\"abc\""}) {
        SCOPED_TRACE(mark);
        const auto library = parse(QByteArray("<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"1\" "
                                              "Location=\"/a\"><POSITION_MARK ") +
                mark +
                "/></TRACK></COLLECTION></DJ_PLAYLISTS>");
        EXPECT_TRUE(library.hasErrors()) << diagnostics(library);
    }
}

TEST(RekordboxXmlParserTest, KeepsTempoSegmentsWithMeterAndPhase) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION><TRACK TrackID="1" Location="/a">
        <TEMPO Inizio="0.5" Bpm="120" Metro="4/4" Battito="3"/>
        <TEMPO Inizio="30.5" Bpm="150" Metro="4/4" Battito="3"/>
        </TRACK></COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    const auto& track = library.tracks.front();
    ASSERT_EQ(track.beatgrid.size(), 2);
    EXPECT_EQ(track.beatgrid[1].position, 30.5);
    EXPECT_EQ(track.beatgrid[1].bpm, 150.0);
    EXPECT_EQ(track.beatgrid[1].meterNumerator, 4);
    EXPECT_EQ(track.beatgrid[1].meterDenominator, 4);
    EXPECT_EQ(track.beatgrid[1].beatInBar, 3);
    const auto serialized = QJsonDocument::fromJson(
            mixxx::rekordbox::serializeTrackAnnotations(track))
                                    .object();
    EXPECT_EQ(serialized.value("gridKind").toString(), "tempo-segments");
    EXPECT_EQ(serialized.value("beatgrid").toArray()[1].toObject().value("beatInBar").toInt(), 3);
}

TEST(RekordboxXmlParserTest, LeavesUndeclaredMeterUnknown) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION><TRACK TrackID="1" Location="/a">
        <TEMPO Inizio="0" Bpm="120"/></TRACK></COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    EXPECT_FALSE(library.tracks.front().beatgrid.front().meterNumerator);
    EXPECT_FALSE(library.tracks.front().beatgrid.front().beatInBar);
}

TEST(RekordboxXmlParserTest, RejectsInvalidTempoSegments) {
    for (const auto* tempo : {"Inizio=\"0\" Bpm=\"0\"",
                 "Inizio=\"-1\" Bpm=\"120\"",
                 "Inizio=\"nan\" Bpm=\"120\"",
                 "Inizio=\"0\" Bpm=\"inf\"",
                 "Inizio=\"0\" Bpm=\"120\" Metro=\"0/4\"",
                 "Inizio=\"0\" Bpm=\"120\" Metro=\"4/4\" Battito=\"5\""}) {
        SCOPED_TRACE(tempo);
        const auto library = parse(QByteArray("<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"1\" "
                                              "Location=\"/a\"><TEMPO ") +
                tempo +
                "/></TRACK></COLLECTION></DJ_PLAYLISTS>");
        EXPECT_TRUE(library.hasErrors()) << diagnostics(library);
    }
}

TEST(RekordboxXmlParserTest, RetainsMetadataPresenceForApplication) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION>
        <TRACK TrackID="1" Location="file://localhost/missing%20track.mp3" AlbumArtist="Ensemble"
         Composer="Composer" Label="Label" Remixer="Remixer" DateAdded="2026-11-10"
         DateModified="2026-11-11" LastPlayed="2026-11-09" DiscNumber="2" PlayCount="0"
         Colour="0x000000" ArtworkPath="cover.jpg" Comments="" TotalTime="240.0"/>
        </COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    const auto& track = library.tracks.front();
    EXPECT_EQ(track.durationSeconds, 240.0);
    EXPECT_EQ(track.albumArtist, "Ensemble");
    EXPECT_EQ(track.composer, "Composer");
    EXPECT_EQ(track.label, "Label");
    EXPECT_EQ(track.remixer, "Remixer");
    EXPECT_EQ(track.discNumber, 2);
    EXPECT_EQ(track.dateModified, "2026-11-11");
    EXPECT_EQ(track.lastPlayed, "2026-11-09");
    EXPECT_EQ(track.color, "0x000000");
    const auto fields = QJsonDocument::fromJson(
            mixxx::rekordbox::serializeTrackAnnotations(track))
                                .object()
                                .value("fields")
                                .toObject();
    EXPECT_TRUE(fields.contains("PlayCount"));
    EXPECT_EQ(fields.value("PlayCount").toString(), "0");
    EXPECT_TRUE(fields.contains("Comments"));
    EXPECT_TRUE(fields.value("Comments").toString().isEmpty());
    EXPECT_FALSE(fields.contains("Album"));
    EXPECT_EQ(fields.value("ArtworkPath").toString(), "cover.jpg");
}

TEST(RekordboxXmlParserTest, ReadsExternalFixtureLabelled586WithoutChangingBytes) {
    const QByteArray data(kRekordbox586Xml);
    EXPECT_EQ(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(),
            "d3f7c9b649ce40b7606de8bd49346f91450c5acafeadeec99b832da6b46ba2f1");
    const auto library = parse(data);
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    EXPECT_EQ(library.productVersion, "5.8.6");
    ASSERT_EQ(library.tracks.size(), 6);
    EXPECT_EQ(library.tracks[4].cues.size(), 4);
    EXPECT_EQ(library.tracks[5].cues.size(), 4);
    EXPECT_EQ(library.playlists.children.front().children[0].children[0].trackIds,
            (QVector<int>{5, 6}));
    EXPECT_EQ(library.tracks[5].beatgrid[1].position, 48.026);
    EXPECT_EQ(library.tracks[5].beatgrid[1].beatInBar, 1);
}

TEST(RekordboxXmlParserTest, ReadsExternalFixtureLabelled662AndItsEmptyPlaylist) {
    const QByteArray data(kRekordbox662Xml);
    EXPECT_EQ(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(),
            "fed8bf4f63759b03c9627170289e79d140008fe681c783651f36547553368101");
    const auto library = parse(data);
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    EXPECT_EQ(library.productVersion, "6.6.2");
    ASSERT_EQ(library.tracks.size(), 6);
    ASSERT_EQ(library.playlists.children.front().children.size(), 1);
    EXPECT_EQ(library.playlists.children.front().children.front().type, 1);
    EXPECT_TRUE(library.playlists.children.front().children.front().entries.isEmpty());
}

TEST(RekordboxXmlParserTest, NeverAliasesMalformedTrackIdentityToZero) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION>
        <TRACK TrackID="bad" Location="/invalid"/><TRACK TrackID="0" Location="/zero"/>
        </COLLECTION><PLAYLISTS><NODE Type="1" KeyType="0" Name="Ids">
        <TRACK Key="0"/></NODE></PLAYLISTS></DJ_PLAYLISTS>)xml");
    EXPECT_TRUE(library.hasErrors()) << diagnostics(library);
    EXPECT_FALSE(library.tracks[0].hasValidId);
    EXPECT_TRUE(library.tracks[1].hasValidId);
    EXPECT_EQ(library.playlists.children.front().entries.front().trackIndex, 1);
}

TEST(RekordboxXmlParserTest, LocationKeysCanIdentifyTracksWithoutTrackId) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION>
        <TRACK Location="/without-id"/><TRACK TrackID="0" Location="/zero"/>
        </COLLECTION><PLAYLISTS><NODE Type="1" KeyType="1" Name="Locations">
        <TRACK Key="/without-id"/><TRACK Key="/zero"/>
        </NODE></PLAYLISTS></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    const auto& entries = library.playlists.children.front().entries;
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].trackIndex, 0);
    EXPECT_FALSE(entries[0].trackId);
    EXPECT_EQ(entries[1].trackIndex, 1);
    EXPECT_EQ(entries[1].trackId, 0);
}

TEST(RekordboxXmlParserTest, DoesNotFallBackToIdKeysForUnsupportedPlaylistGrammar) {
    for (const auto* attributes : {"Type=\"1\" KeyType=\"bad\"",
                 "Type=\"1\" KeyType=\"2\"",
                 "Type=\"bad\" KeyType=\"0\"",
                 "Type=\"0\" KeyType=\"0\""}) {
        SCOPED_TRACE(attributes);
        const auto library = parse(QByteArray("<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"0\" "
                                              "Location=\"/a\"/></COLLECTION><PLAYLISTS><NODE ") +
                attributes + "><TRACK Key=\"0\"/></NODE></PLAYLISTS></DJ_PLAYLISTS>");
        EXPECT_TRUE(library.playlists.children.front().trackIds.isEmpty());
        EXPECT_FALSE(library.playlists.children.front().entries.front().trackIndex);
        EXPECT_FALSE(library.diagnostics.isEmpty());
    }
}

TEST(RekordboxXmlParserTest, RejectsIncompleteOrOutOfRangeRgbExtensionsWithoutInventingColor) {
    for (const auto* color : {"Red=\"0\"",
                 "Red=\"256\" Green=\"0\" Blue=\"0\"",
                 "Red=\"0\" Green=\"-1\" Blue=\"0\"",
                 "Red=\"x\" Green=\"0\" Blue=\"0\""}) {
        SCOPED_TRACE(color);
        const auto library = parse(
                QByteArray("<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"1\" "
                           "Location=\"/a\"><POSITION_MARK Type=\"0\" "
                           "Start=\"0\" ") +
                color + "/></TRACK></COLLECTION></DJ_PLAYLISTS>");
        ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
        EXPECT_TRUE(library.tracks.front().cues.front().color.isEmpty());
        EXPECT_FALSE(library.diagnostics.isEmpty());
    }
}

TEST(RekordboxXmlParserTest, AcceptsFloatingPointSampleRateAndRejectsNonFiniteMetadata) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION>
        <TRACK TrackID="1" Location="/a" SampleRate="44100.0" AverageBpm="nan"/>
        </COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    EXPECT_EQ(library.tracks.front().sampleRate, 44100.0);
    EXPECT_EQ(library.tracks.front().bpm, 0.0);
    EXPECT_FALSE(library.diagnostics.isEmpty());
}

TEST(RekordboxXmlParserTest, PreservesQualifiedAttributeNamesWithoutSemanticCollisions) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS xmlns:ext="urn:other"><COLLECTION>
        <TRACK TrackID="1" Location="/a" Rating="255" ext:Rating="0">
        <POSITION_MARK Type="0" ext:Type="4" Start="0" Num="-1"/>
        </TRACK></COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    const auto& track = library.tracks.front();
    EXPECT_EQ(track.rating, 5);
    EXPECT_EQ(track.attributes.value("Rating"), "255");
    EXPECT_EQ(track.attributes.value("ext:Rating"), "0");
    const auto metadata = QJsonDocument::fromJson(
            mixxx::rekordbox::serializeTrackAnnotations(track))
                                  .object();
    EXPECT_EQ(metadata.value("fields").toObject().value("Rating").toString(), "255");
    EXPECT_EQ(metadata.value("cues").toArray()[0].toObject().value("sourceType").toString(), "0");
}

TEST(RekordboxXmlParserTest, PreservesTempoAttributePresenceAndRejectsExplicitEmptyPhase) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS><COLLECTION><TRACK TrackID="1" Location="/a">
        <TEMPO Inizio="0.0" Bpm="120.00" Metro="" Battito=""/>
        </TRACK></COLLECTION></DJ_PLAYLISTS>)xml");
    EXPECT_TRUE(library.hasErrors()) << diagnostics(library);
    const auto& segment = library.tracks.front().beatgrid.front();
    EXPECT_TRUE(segment.attributes.contains("Metro"));
    EXPECT_TRUE(segment.attributes.value("Metro").isEmpty());
    EXPECT_EQ(segment.attributes.value("Inizio"), "0.0");
    EXPECT_EQ(segment.attributes.value("Bpm"), "120.00");
}

TEST(RekordboxXmlParserTest, DoesNotResolveLossyOrQualifiedFileUrisToOtherTracks) {
    for (const auto* key : {"file://localhost/a.mp3?version=1",
                 "file://localhost/a.mp3#part",
                 "file://localhost/%FF.mp3",
                 "file://localhost/%00.mp3",
                 "file://localhost/bad%escape.mp3",
                 "file:relative.mp3"}) {
        SCOPED_TRACE(key);
        EXPECT_TRUE(mixxx::rekordbox::normalizeLocation(QString::fromUtf8(key)).isEmpty());
        const auto library = parse(QByteArray("<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"1\" "
                                              "Location=\"/a.mp3\"/></COLLECTION><PLAYLISTS>"
                                              "<NODE Type=\"1\" KeyType=\"1\"><TRACK Key=\"") +
                key + "\"/></NODE></PLAYLISTS></DJ_PLAYLISTS>");
        EXPECT_TRUE(library.playlists.children.front().trackIds.isEmpty());
        EXPECT_FALSE(library.playlists.children.front().entries.front().trackIndex);
    }
    EXPECT_EQ(mixxx::rekordbox::normalizeLocation("/a?literal#100%.mp3"),
            "/a?literal#100%.mp3");
}

TEST(RekordboxXmlParserTest, DoesNotInterpretForeignNamespaceElementsAsDocumentedFields) {
    const auto library = parse(R"xml(<DJ_PLAYLISTS xmlns:ext="urn:other"><COLLECTION>
        <TRACK TrackID="1" Location="/a"><ext:TEMPO Inizio="0" Bpm="120"/>
        <ext:POSITION_MARK Type="0" Start="0" Num="0"/></TRACK>
        <ext:TRACK TrackID="2" Location="/b"/></COLLECTION></DJ_PLAYLISTS>)xml");
    ASSERT_FALSE(library.hasErrors()) << diagnostics(library);
    ASSERT_EQ(library.tracks.size(), 1);
    EXPECT_TRUE(library.tracks.front().beatgrid.isEmpty());
    EXPECT_TRUE(library.tracks.front().cues.isEmpty());
}

TEST(RekordboxXmlParserTest, RejectsUnknownFormatRevisionSeparatelyFromExporterVersion) {
    QBuffer buffer;
    buffer.setData("<DJ_PLAYLISTS Version=\"999\"><PRODUCT Version=\"6.6.2\"/></DJ_PLAYLISTS>");
    ASSERT_TRUE(buffer.open(QIODevice::ReadOnly));
    const auto result = mixxx::rekordbox::decodeXml(&buffer);
    EXPECT_EQ(result.outcome, mixxx::rekordbox::XmlOutcome::Unsupported);
    EXPECT_FALSE(result.snapshot);
    EXPECT_FALSE(result.diagnostics.isEmpty());
}

TEST(RekordboxXmlParserTest, FrozenDecodePublishesNoPartialSnapshotAfterFailureAndCanRetry) {
    QBuffer buffer;
    buffer.setData("<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"1\" Location=\"/a\"/>");
    ASSERT_TRUE(buffer.open(QIODevice::ReadOnly));
    const auto failed = mixxx::rekordbox::decodeXml(&buffer);
    EXPECT_EQ(failed.outcome, mixxx::rekordbox::XmlOutcome::Invalid);
    EXPECT_FALSE(failed.snapshot);
    buffer.close();
    buffer.setData(
            "<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"1\" Location=\"/a\"/>"
            "</COLLECTION></DJ_PLAYLISTS>");
    ASSERT_TRUE(buffer.open(QIODevice::ReadOnly));
    const auto repaired = mixxx::rekordbox::decodeXml(&buffer);
    EXPECT_EQ(repaired.outcome, mixxx::rekordbox::XmlOutcome::Success);
    ASSERT_TRUE(repaired.snapshot);
    EXPECT_EQ(repaired.snapshot->tracks.size(), 1);
}

TEST(RekordboxXmlParserTest, ReportsUnrecognizedChildElementsAsExplicitLoss) {
    QBuffer buffer;
    buffer.setData("<DJ_PLAYLISTS><FUTURE_DATA/></DJ_PLAYLISTS>");
    ASSERT_TRUE(buffer.open(QIODevice::ReadOnly));
    const auto result = mixxx::rekordbox::decodeXml(&buffer);
    EXPECT_EQ(result.outcome, mixxx::rekordbox::XmlOutcome::Warning);
    EXPECT_TRUE(result.snapshot);
    ASSERT_EQ(result.diagnostics.size(), 1);
    EXPECT_TRUE(result.diagnostics.front().message.contains("FUTURE_DATA"));
}
