#include "stems/stemconverter.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <vector>

#ifdef __STEM_CONVERSION__
TEST(StemConverterTest, MissingVerifiedBaseModelFailsBeforeProcessing) {
    QTemporaryDir modelDir;
    ASSERT_TRUE(modelDir.isValid());

    QTemporaryFile inputFile;
    ASSERT_TRUE(inputFile.open());
    const auto pTrack = Track::newTemporary(inputFile.fileName());
    ASSERT_TRUE(pTrack);

    const bool hadModelDirectory = qEnvironmentVariableIsSet("MIXXX_STEM_MODEL_DIR");
    const QByteArray previousModelDirectory = qgetenv("MIXXX_STEM_MODEL_DIR");
    qputenv("MIXXX_STEM_MODEL_DIR", modelDir.path().toUtf8());

    StemConverter converter;
    converter.convertTrack(pTrack, StemConverter::Resolution::Low);

    if (hadModelDirectory) {
        qputenv("MIXXX_STEM_MODEL_DIR", previousModelDirectory);
    } else {
        qunsetenv("MIXXX_STEM_MODEL_DIR");
    }

    EXPECT_EQ(converter.getState(), StemConverter::ConversionState::Failed);
    EXPECT_FLOAT_EQ(converter.getProgress(), 0.0f);
}
#endif

TEST(StemConverterTest, UsesOnlyTheVerifiedModelByDefault) {
    EXPECT_EQ(StemConverter::defaultResolution(), StemConverter::Resolution::Low);
    EXPECT_TRUE(StemConverter::isResolutionSupported(StemConverter::Resolution::Low));
    EXPECT_FALSE(StemConverter::isResolutionSupported(StemConverter::Resolution::High));
}

TEST(StemConverterTest, RejectsUnsupportedFineTunedModel) {
    QTemporaryFile inputFile;
    ASSERT_TRUE(inputFile.open());

    const auto pTrack = Track::newTemporary(inputFile.fileName());
    ASSERT_TRUE(pTrack);

    StemConverter converter;
    converter.convertTrack(pTrack, StemConverter::Resolution::High);

    EXPECT_EQ(converter.getState(), StemConverter::ConversionState::Failed);
    EXPECT_FLOAT_EQ(converter.getProgress(), 0.0f);
}

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
