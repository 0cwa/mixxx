#include "stems/stemconverter.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <vector>

TEST(StemConverterTest, ConvertsStereoInterleavedAudioToPlanarAndBack) {
    const std::vector<float> interleaved{1.0f, 10.0f, 2.0f, 20.0f, 3.0f, 30.0f};
    const std::vector<float> expectedPlanar{1.0f, 2.0f, 3.0f, 10.0f, 20.0f, 30.0f};
    std::vector<float> planar;

    ASSERT_TRUE(StemConverter::interleavedToPlanar(interleaved, 2, 3, &planar));
    EXPECT_EQ(planar, expectedPlanar);

    std::vector<float> roundTrip;
    ASSERT_TRUE(StemConverter::planarToInterleaved(planar, 2, 3, &roundTrip));
    EXPECT_EQ(roundTrip, interleaved);
}

TEST(StemConverterTest, RejectsInvalidAudioLayout) {
    const std::vector<float> samples{1.0f, 2.0f, 3.0f};
    std::vector<float> output;

    EXPECT_FALSE(StemConverter::interleavedToPlanar(samples, 0, 3, &output));
    EXPECT_FALSE(StemConverter::interleavedToPlanar(samples, 2, 3, &output));
    EXPECT_FALSE(StemConverter::planarToInterleaved(samples, 2, 3, &output));
    EXPECT_FALSE(StemConverter::planarToInterleaved(samples, 2, 0, &output));
}

TEST(StemConverterTest, UsesPathAsIdentityForTemporaryTrack) {
    QTemporaryFile inputFile;
    ASSERT_TRUE(inputFile.open());

    const auto pTrack = Track::newTemporary(inputFile.fileName());
    ASSERT_TRUE(pTrack);
    EXPECT_FALSE(pTrack->getId().isValid());
    EXPECT_TRUE(StemConverter::isValidConversionInput(pTrack));
}

TEST(StemConverterTest, DetectsAudioThatNeedsModelFormatConversion) {
    EXPECT_FALSE(StemConverter::requiresAudioFormatConversion(44100, 2));
    EXPECT_TRUE(StemConverter::requiresAudioFormatConversion(48000, 2));
    EXPECT_TRUE(StemConverter::requiresAudioFormatConversion(44100, 1));
}

TEST(StemConverterTest, SeparatesSameBasenameWithDifferentExtensions) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const QString mp3Path = QDir(tempDir.path()).filePath("song.mp3");
    const QString wavPath = QDir(tempDir.path()).filePath("song.wav");

    const QString firstStemsDir = StemConverter::getStemsDirectory(mp3Path);
    EXPECT_EQ(firstStemsDir, QDir(tempDir.path()).filePath("song"));
    ASSERT_TRUE(QDir().mkpath(firstStemsDir));

    const QString firstOutputPath = StemConverter::getStemOutputPath(firstStemsDir);
    EXPECT_EQ(firstOutputPath,
            QDir(tempDir.path()).filePath("song.stem.m4a"));
    QFile firstOutput(firstOutputPath);
    ASSERT_TRUE(firstOutput.open(QIODevice::WriteOnly));
    firstOutput.close();

    const QString secondStemsDir = StemConverter::getStemsDirectory(wavPath);
    EXPECT_EQ(secondStemsDir, QDir(tempDir.path()).filePath("song-1"));
    EXPECT_EQ(StemConverter::getStemOutputPath(secondStemsDir),
            QDir(tempDir.path()).filePath("song-1.stem.m4a"));
}

TEST(StemConverterTest, AllocatesDistinctIdentitiesForRepeatedQueuedInputs) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const QString inputPath = QDir(tempDir.path()).filePath("song.mp3");
    const QString firstStemsDir = StemConverter::getStemsDirectory(inputPath);
    ASSERT_TRUE(QDir().mkpath(firstStemsDir));
    QFile firstOutput(StemConverter::getStemOutputPath(firstStemsDir));
    ASSERT_TRUE(firstOutput.open(QIODevice::WriteOnly));
    firstOutput.close();

    const QString secondStemsDir = StemConverter::getStemsDirectory(inputPath);
    EXPECT_EQ(secondStemsDir, QDir(tempDir.path()).filePath("song-1"));
    ASSERT_TRUE(QDir().mkpath(secondStemsDir));
    QFile secondOutput(StemConverter::getStemOutputPath(secondStemsDir));
    ASSERT_TRUE(secondOutput.open(QIODevice::WriteOnly));
    secondOutput.close();

    EXPECT_EQ(StemConverter::getStemsDirectory(inputPath),
            QDir(tempDir.path()).filePath("song-2"));
}
