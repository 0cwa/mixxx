#include "library/rekordbox/rekordboxfeature.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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
    return {FramePos(start),
            end < 0 ? FramePos() : FramePos(end),
            std::move(comment),
            color,
            extended,
            order};
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

TEST(RekordboxFeatureTest, SameTimeDifferentColorsHaveStrictDeterministicOrder) {
    const auto red = cue(100,
            -1,
            QStringLiteral("same"),
            mixxx::RgbColor::optional(0xff0000),
            true,
            0);
    const auto blue = cue(100,
            -1,
            QStringLiteral("same"),
            mixxx::RgbColor::optional(0x0000ff),
            true,
            0);
    const auto uncolored = cue(100,
            -1,
            QStringLiteral("same"),
            mixxx::RgbColor::nullopt(),
            true,
            0);
    const QList<MemoryCueLoop> values{red, blue, uncolored};
    for (const auto& a : values) {
        EXPECT_FALSE(mixxx::rekordbox::memoryCueLoopLess(a, a));
        for (const auto& b : values) {
            EXPECT_FALSE(mixxx::rekordbox::memoryCueLoopLess(a, b) &&
                    mixxx::rekordbox::memoryCueLoopLess(b, a));
            for (const auto& c : values) {
                if (mixxx::rekordbox::memoryCueLoopLess(a, b) &&
                        mixxx::rekordbox::memoryCueLoopLess(b, c)) {
                    EXPECT_TRUE(mixxx::rekordbox::memoryCueLoopLess(a, c));
                }
            }
        }
    }
    const auto forward = mixxx::rekordbox::normalizeMemoryCueLoops({red, blue});
    const auto reverse = mixxx::rekordbox::normalizeMemoryCueLoops({blue, red});
    ASSERT_EQ(2, forward.size());
    ASSERT_EQ(2, reverse.size());
    EXPECT_EQ(blue.color, forward[0].color);
    EXPECT_EQ(forward[0].color, reverse[0].color);
    EXPECT_EQ(forward[1].color, reverse[1].color);
}

TEST(RekordboxFeatureTest, DeviceRootPathsAndTraversalAreHandled) {
    const QString root = QDir::rootPath();
    EXPECT_EQ(QDir(root).filePath(QStringLiteral("Contents/test.wav")),
            mixxx::rekordbox::resolveDeviceMediaPath(root, "/Contents/test.wav"));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    EXPECT_EQ(QDir(QDir(directory.path()).canonicalPath())
                      .filePath(QStringLiteral("Contents/test.wav")),
            mixxx::rekordbox::resolveDeviceMediaPath(
                    directory.path(), "/Contents/test.wav"));
    EXPECT_TRUE(mixxx::rekordbox::resolveDeviceMediaPath(
            directory.path(), "../outside.wav")
                    .isEmpty());
    EXPECT_TRUE(mixxx::rekordbox::resolveDeviceMediaPath(directory.path(), "").isEmpty());
}

TEST(RekordboxFeatureTest, MissingDeviceMediaCannotEscapeThroughAnExistingSymlinkAncestor) {
#ifdef __WINDOWS__
    GTEST_SKIP() << "This regression requires a filesystem symlink";
#else
    QTemporaryDir device;
    QTemporaryDir outside;
    ASSERT_TRUE(device.isValid());
    ASSERT_TRUE(outside.isValid());
    ASSERT_TRUE(QFile::link(outside.path(), device.filePath(QStringLiteral("escape"))));
    ASSERT_FALSE(QFile::exists(outside.filePath(QStringLiteral("missing.wav"))));
    EXPECT_TRUE(mixxx::rekordbox::resolveDeviceMediaPath(
            device.path(), "escape/missing.wav")
                    .isEmpty());
    EXPECT_EQ(QDir(QDir(device.path()).canonicalPath())
                      .filePath(QStringLiteral("future/subdirectory/safe.wav")),
            mixxx::rekordbox::resolveDeviceMediaPath(
                    device.path(), "future/subdirectory/safe.wav"));
#endif
}

TEST(RekordboxFeatureTest, DistinctSameIntervalExtendedLoopsArePreserved) {
    const auto normalized = mixxx::rekordbox::normalizeMemoryCueLoops({
            cue(100, 200, QStringLiteral("one"), mixxx::RgbColor::optional(0xff0000), true, 0),
            cue(100, 200, QStringLiteral("two"), mixxx::RgbColor::optional(0x0000ff), true, 1),
    });
    EXPECT_EQ(2, normalized.size());
}

TEST(RekordboxFeatureTest, AnalyzeSourceIdentityIncludesBothMediaAndAnalysisPaths) {
    const QString sourceIdentity = mixxx::rekordbox::analyzeImportSourceIdentity(
            QStringLiteral("/music/track.mp3"),
            QStringLiteral("/rekordbox/ANLZ/00000001.DAT"));
    EXPECT_EQ(sourceIdentity,
            mixxx::rekordbox::analyzeImportSourceIdentity(
                    QStringLiteral("/music/track.mp3"),
                    QStringLiteral("/rekordbox/ANLZ/00000001.DAT")));
    EXPECT_NE(sourceIdentity,
            mixxx::rekordbox::analyzeImportSourceIdentity(
                    QStringLiteral("/music/other.mp3"),
                    QStringLiteral("/rekordbox/ANLZ/00000001.DAT")));
    EXPECT_NE(sourceIdentity,
            mixxx::rekordbox::analyzeImportSourceIdentity(
                    QStringLiteral("/music/track.mp3"),
                    QStringLiteral("/rekordbox/ANLZ/00000002.DAT")));
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
    EXPECT_EQ(cues[1]->getType(), mixxx::CueType::Memory);
    EXPECT_EQ(cues[1]->getHotCue(), Cue::kNoHotCue);
    EXPECT_EQ(cues[1]->getPosition(), mixxx::audio::FramePos(176400));
    EXPECT_EQ(cues[1]->getEndPosition(), mixxx::audio::FramePos(264600));
    EXPECT_EQ(cues[2]->getType(), mixxx::CueType::Memory);
    EXPECT_EQ(cues[2]->getHotCue(), Cue::kNoHotCue);
    EXPECT_EQ(cues[2]->getPosition(), mixxx::audio::FramePos(352800));
    EXPECT_EQ(cues[2]->getEndPosition(), mixxx::audio::FramePos(441000));

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
