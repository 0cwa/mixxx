#include "library/rekordbox/rekordboxfeature.h"

#include <gtest/gtest.h>

#include <utility>

#include "track/cue.h"
#include "track/track.h"

namespace {

using mixxx::audio::FramePos;
using mixxx::rekordbox::MemoryCueLoop;

MemoryCueLoop cue(double start,
        double end,
        QString comment,
        mixxx::RgbColor::optional_t color,
        bool extended,
        int order) {
    return {FramePos(start), end < 0 ? FramePos() : FramePos(end), std::move(comment), color, extended, order};
}

TEST(RekordboxFeatureTest, ExtendedLoopReplacesLegacyDuplicate) {
    const auto normalized = mixxx::rekordbox::normalizeMemoryCueLoops({
            cue(100, 200, QString(), mixxx::RgbColor::nullopt(), false, 0),
            cue(100, 200, QStringLiteral("loop"), mixxx::RgbColor::optional(0x123456), true, 1),
    });

    ASSERT_EQ(normalized.size(), 1);
    EXPECT_EQ(normalized.front().comment, QStringLiteral("loop"));
    EXPECT_TRUE(normalized.front().fromExtendedSection);
}

TEST(RekordboxFeatureTest, IdenticalMemoryAndLoopRecordsAreDeduplicated) {
    const auto normalized = mixxx::rekordbox::normalizeMemoryCueLoops({
            cue(100, -1, QStringLiteral("cue"), mixxx::RgbColor::nullopt(), false, 0),
            cue(100, -1, QStringLiteral("cue"), mixxx::RgbColor::nullopt(), false, 1),
            cue(200, 300, QStringLiteral("loop"), mixxx::RgbColor::nullopt(), false, 2),
            cue(200, 300, QStringLiteral("loop"), mixxx::RgbColor::nullopt(), false, 3),
    });

    ASSERT_EQ(normalized.size(), 2);
    EXPECT_EQ(normalized[0].comment, QStringLiteral("cue"));
    EXPECT_EQ(normalized[1].comment, QStringLiteral("loop"));
}

TEST(RekordboxFeatureTest, SamePositionMemoryCuesAreNotPositionDeduplicated) {
    const auto normalized = mixxx::rekordbox::normalizeMemoryCueLoops({
            cue(100, -1, QStringLiteral("one"), mixxx::RgbColor::nullopt(), false, 0),
            cue(100, -1, QStringLiteral("two"), mixxx::RgbColor::nullopt(), false, 1),
    });

    ASSERT_EQ(normalized.size(), 2);
    EXPECT_EQ(normalized[0].comment, QStringLiteral("one"));
    EXPECT_EQ(normalized[1].comment, QStringLiteral("two"));
}

TEST(RekordboxFeatureTest, OrderingUsesCompleteCueRecord) {
    const auto normalized = mixxx::rekordbox::normalizeMemoryCueLoops({
            cue(200, 300, QStringLiteral("b"), mixxx::RgbColor::nullopt(), true, 1),
            cue(100, 150, QStringLiteral("loop"), mixxx::RgbColor::nullopt(), true, 2),
            cue(100, -1, QStringLiteral("main"), mixxx::RgbColor::nullopt(), true, 0),
    });

    ASSERT_EQ(normalized.size(), 3);
    EXPECT_EQ(normalized[0].comment, QStringLiteral("main"));
    EXPECT_EQ(normalized[1].comment, QStringLiteral("loop"));
    EXPECT_EQ(normalized[2].comment, QStringLiteral("b"));
}

TEST(RekordboxFeatureTest, RepeatedAnalyzeImportUsesSameSourceIdentity) {
    const QString sourceIdentity = mixxx::rekordbox::analyzeImportSourceIdentity(
            QStringLiteral("/music/track.mp3"),
            QStringLiteral("/rekordbox/ANLZ/00000001.DAT"));
    QSet<QString> importedSources;

    EXPECT_TRUE(mixxx::rekordbox::shouldImportAnalyzeSource(
            sourceIdentity, importedSources));
    importedSources.insert(sourceIdentity);
    EXPECT_FALSE(mixxx::rekordbox::shouldImportAnalyzeSource(
            sourceIdentity, importedSources));
    EXPECT_TRUE(mixxx::rekordbox::shouldImportAnalyzeSource(
            mixxx::rekordbox::analyzeImportSourceIdentity(
                    QStringLiteral("/music/other.mp3"),
                    QStringLiteral("/rekordbox/ANLZ/00000001.DAT")),
            importedSources));
}

TEST(RekordboxFeatureTest, XmlMappingKeepsReusedDatabaseTrackId) {
    QHash<int, int> trackIds;
    QVector<int> importedTrackIds;

    mixxx::rekordbox::recordXmlTrackMapping(
            &trackIds, &importedTrackIds, 42, 7);

    ASSERT_EQ(importedTrackIds, QVector<int>{42});
    EXPECT_EQ(trackIds.value(42), 7);
}

TEST(RekordboxFeatureTest, AppliesXmlBeatgridAndMultipleLoopsToTrack) {
    const auto track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::audio::ChannelCount(2),
            mixxx::audio::SampleRate(44100),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(30));

    const QByteArray annotations = R"json({
        "beatgrid": [{"position": 0.0, "bpm": 120.0},
                     {"position": 0.5, "bpm": 120.0},
                     {"position": 1.0, "bpm": 120.0}],
        "cues": [{"name": "Intro", "type": "cue", "start": 2.0,
                   "end": 2.0, "color": "255,0,0"},
                 {"name": "Loop A", "type": "loop", "start": 4.0,
                   "end": 6.0, "color": "0,255,0"},
                 {"name": "Loop B", "type": "loop", "start": 8.0,
                   "end": 10.0, "color": "0,0,255"}]
    })json";

    mixxx::rekordbox::applyXmlTrackAnnotations(
            track, track->getSampleRate(), annotations);

    ASSERT_TRUE(track->getBeats());
    // A constant-tempo grid has no explicit tempo-change markers.
    EXPECT_TRUE(track->getBeats()->getMarkers().empty());
    EXPECT_EQ(track->getBeats()->getSampleRate().value(), track->getSampleRate().value());
    EXPECT_NEAR(track->getBeats()->getLastMarkerBpm().value(), 120.0, 0.01);
    EXPECT_NEAR(track->getBpm(), 120.0, 0.01);
    EXPECT_TRUE(track->isBpmLocked());
    const auto cues = track->getCuePoints();
    ASSERT_EQ(cues.size(), 3);
    EXPECT_EQ(cues[0]->getType(), mixxx::CueType::Memory);
    EXPECT_EQ(cues[1]->getType(), mixxx::CueType::Loop);
    EXPECT_EQ(cues[1]->getEndPosition(), mixxx::audio::FramePos(264600));
    EXPECT_EQ(cues[2]->getType(), mixxx::CueType::Loop);

    // Re-applying the sidecar is safe and does not duplicate imported cues.
    mixxx::rekordbox::applyXmlTrackAnnotations(
            track, track->getSampleRate(), annotations);
    EXPECT_EQ(track->getCuePoints().size(), 3);
}

TEST(RekordboxFeatureTest, AppliesCuesToBpmLockedTrackWithoutReplacingBeatgrid) {
    const auto track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::audio::ChannelCount(2),
            mixxx::audio::SampleRate(44100),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(30));
    track->trySetBpm(100.0);
    track->setBpmLocked(true);

    const QByteArray annotations = R"json({
        "beatgrid": [{"position": 0.0, "bpm": 120.0},
                     {"position": 0.5, "bpm": 120.0}],
        "cues": [{"name": "Cue", "type": "cue", "start": 1.0,
                   "end": 1.0}]
    })json";
    mixxx::rekordbox::applyXmlTrackAnnotations(
            track, track->getSampleRate(), annotations);

    EXPECT_NEAR(track->getBpm(), 100.0, 0.01);
    ASSERT_EQ(track->getCuePoints().size(), 1);
    EXPECT_EQ(track->getCuePoints().front()->getType(), mixxx::CueType::Memory);
}

} // namespace
