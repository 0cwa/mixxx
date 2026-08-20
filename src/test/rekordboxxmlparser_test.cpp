#include "library/rekordbox/rekordboxxmlparser.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>

namespace {

mixxx::rekordbox::Library parse(const QByteArray& data) {
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    return mixxx::rekordbox::parseXml(&buffer);
}

} // namespace

TEST(RekordboxXmlParserTest, ParsesCollectionPlaylistsBeatgridCuesAndArtwork) {
    const auto library = parse(R"xml(
<DJ_PLAYLISTS Version="1.0.0" Product="rekordbox">
 <COLLECTION Entries="1"><TRACK TrackID="7" Name="Song" Artist="Artist" Location="file://localhost/song.mp3" ArtworkPath="cover.jpg" AverageBpm="128.0" TotalTime="240">
  <TEMPO Inizio="0" Bpm="128"/><TEMPO Inizio="4" Bpm="128"/>
  <POSITION_MARK Name="Intro" Type="cue" Start="1.5" End="1.5" Num="1" Red="255" Green="0" Blue="0"/>
  <POSITION_MARK Name="Loop" Type="loop" Start="8" End="12" Num="2"/>
 </TRACK></COLLECTION>
 <PLAYLISTS><NODE Type="0" Name="ROOT"><NODE Type="1" Name="Set"><PLAYLIST Key="7"/></NODE></NODE></PLAYLISTS>
</DJ_PLAYLISTS>)xml");

    ASSERT_FALSE(library.hasErrors());
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
    ASSERT_NE(error, library.diagnostics.cend());
    EXPECT_GT(error->line, 0);
}

TEST(RekordboxXmlParserTest, NullDeviceIsSafe) {
    const auto library = mixxx::rekordbox::parseXml(nullptr);
    EXPECT_TRUE(library.hasErrors());
    EXPECT_TRUE(library.tracks.isEmpty());
}

TEST(RekordboxXmlParserTest, ReportsMissingLocationsAndDuplicateIdentities) {
    const auto library = parse(R"xml(
<DJ_PLAYLISTS><COLLECTION>
 <TRACK TrackID="1" Name="Missing"/>
 <TRACK TrackID="1" Name="Duplicate" Location="file://localhost/song.mp3"/>
 <TRACK TrackID="2" Name="Same location" Location="file://localhost/song.mp3"/>
</COLLECTION></DJ_PLAYLISTS>)xml");

    ASSERT_FALSE(library.hasErrors());
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
