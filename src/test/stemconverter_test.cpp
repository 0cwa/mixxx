#include "stems/stemconverter.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <algorithm>
#include <vector>

#ifdef __STEM_CONVERSION__
TEST(StemConverterTest, FindsModelInInstalledResourceDirectory) {
    QTemporaryDir resourceDir;
    ASSERT_TRUE(resourceDir.isValid());
    QTemporaryDir userModelDir;
    ASSERT_TRUE(userModelDir.isValid());

    const QString modelsDir = QDir(resourceDir.path()).filePath("models");
    ASSERT_TRUE(QDir().mkpath(modelsDir));
    const QString modelPath = QDir(modelsDir).filePath("htdemucs.onnx");
    QFile modelFile(modelPath);
    ASSERT_TRUE(modelFile.open(QIODevice::WriteOnly));
    modelFile.close();

    EXPECT_EQ(StemConverter::findModelPath(
                      QStringLiteral("htdemucs.onnx"),
                      QString(),
                      resourceDir.path(),
                      userModelDir.path()),
            modelPath);
}

TEST(StemConverterTest, ExplicitModelDirectoryDoesNotFallBack) {
    QTemporaryDir resourceDir;
    ASSERT_TRUE(resourceDir.isValid());
    QTemporaryDir overrideDir;
    ASSERT_TRUE(overrideDir.isValid());

    const QString modelsDir = QDir(resourceDir.path()).filePath("models");
    ASSERT_TRUE(QDir().mkpath(modelsDir));
    QFile modelFile(QDir(modelsDir).filePath("htdemucs.onnx"));
    ASSERT_TRUE(modelFile.open(QIODevice::WriteOnly));
    modelFile.close();

    EXPECT_TRUE(StemConverter::findModelPath(
            QStringLiteral("htdemucs.onnx"),
            overrideDir.path(),
            resourceDir.path(),
            QString())
                    .isEmpty());
}

TEST(StemConverterTest, FallsBackToUserModelDirectory) {
    QTemporaryDir resourceDir;
    ASSERT_TRUE(resourceDir.isValid());
    QTemporaryDir userModelDir;
    ASSERT_TRUE(userModelDir.isValid());

    const QString modelPath = QDir(userModelDir.path()).filePath("htdemucs.onnx");
    QFile modelFile(modelPath);
    ASSERT_TRUE(modelFile.open(QIODevice::WriteOnly));
    modelFile.close();

    EXPECT_EQ(StemConverter::findModelPath(
                      QStringLiteral("htdemucs.onnx"),
                      QString(),
                      resourceDir.path(),
                      userModelDir.path()),
            modelPath);
}

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
    int conversionStartedCount = 0;
    int conversionProgressCount = 0;
    QObject::connect(
            &converter,
            &StemConverter::conversionStarted,
            [&conversionStartedCount](TrackId, const QString&) { ++conversionStartedCount; });
    QObject::connect(
            &converter,
            &StemConverter::conversionProgress,
            [&conversionProgressCount](TrackId, float, const QString&) {
                ++conversionProgressCount;
            });
    QString failureMessage;
    QObject::connect(
            &converter,
            &StemConverter::conversionFailed,
            [&failureMessage](TrackId, const QString& message) { failureMessage = message; });
    converter.convertTrack(pTrack, StemConverter::Resolution::Low);

    if (hadModelDirectory) {
        qputenv("MIXXX_STEM_MODEL_DIR", previousModelDirectory);
    } else {
        qunsetenv("MIXXX_STEM_MODEL_DIR");
    }

    EXPECT_EQ(converter.getState(), StemConverter::ConversionState::Failed);
    EXPECT_EQ(conversionStartedCount, 0);
    EXPECT_EQ(conversionProgressCount, 0);
    EXPECT_EQ(failureMessage, QStringLiteral("Verified HTDemucs model unavailable"));
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

TEST(StemConverterTest, DetectsOverlappingExactWindow) {
    constexpr std::size_t kChunkFrames = 8;
    constexpr std::size_t kChunkStepFrames = 6;

    EXPECT_TRUE(StemConverter::hasNextChunk(0, kChunkStepFrames, kChunkFrames));
}

TEST(StemConverterTest, DetectsPartialFinalWindow) {
    constexpr std::size_t kChunkStepFrames = 6;

    EXPECT_TRUE(StemConverter::hasNextChunk(0, kChunkStepFrames, 7));
    EXPECT_FALSE(StemConverter::hasNextChunk(kChunkStepFrames, kChunkStepFrames, 7));
}

TEST(StemConverterTest, DoesNotCreateWindowAtExactEnd) {
    constexpr std::size_t kChunkStepFrames = 6;

    EXPECT_FALSE(StemConverter::hasNextChunk(
            kChunkStepFrames, kChunkStepFrames, kChunkStepFrames));
}

TEST(StemConverterTest, BlendsAtNextWindowStartForShortWindow) {
    constexpr std::size_t kChunkWindowFrames = 8;
    constexpr std::size_t kChunkStepFrames = 6;
    constexpr std::size_t kTotalFrames = 7;

    const float firstWindowWeight = StemConverter::getChunkFrameWeight(
            0, 6, kChunkWindowFrames, kChunkStepFrames, kTotalFrames);
    const float nextWindowWeight = StemConverter::getChunkFrameWeight(
            kChunkStepFrames, 0, kChunkWindowFrames, kChunkStepFrames, kTotalFrames);

    EXPECT_FLOAT_EQ(firstWindowWeight, 0.5f);
    EXPECT_FLOAT_EQ(nextWindowWeight, 0.5f);
    EXPECT_FLOAT_EQ(firstWindowWeight + nextWindowWeight, 1.0f);

    const float blendedOutput = (1.0f * firstWindowWeight + 3.0f * nextWindowWeight) /
            (firstWindowWeight + nextWindowWeight);
    EXPECT_FLOAT_EQ(blendedOutput, 2.0f);
}

TEST(StemConverterTest, KeepsConstantWaveContinuousAcrossPartialOverlap) {
    constexpr std::size_t kChunkWindowFrames = 8;
    constexpr std::size_t kChunkStepFrames = 6;
    constexpr std::size_t kTotalFrames = 7;
    constexpr float kWaveValue = 2.5f;

    std::vector<float> output(kTotalFrames, 0.0f);
    std::vector<float> outputWeights(kTotalFrames, 0.0f);

    for (std::size_t frameOffset = 0; frameOffset < kTotalFrames;
            frameOffset += kChunkStepFrames) {
        const std::size_t chunkFrames = std::min(
                kChunkWindowFrames, kTotalFrames - frameOffset);
        for (std::size_t frame = 0; frame < chunkFrames; ++frame) {
            const std::size_t outputFrame = frameOffset + frame;
            const float weight = StemConverter::getChunkFrameWeight(
                    frameOffset,
                    frame,
                    kChunkWindowFrames,
                    kChunkStepFrames,
                    kTotalFrames);
            output[outputFrame] += kWaveValue * weight;
            outputWeights[outputFrame] += weight;
        }
        if (!StemConverter::hasNextChunk(
                    frameOffset, kChunkStepFrames, kTotalFrames)) {
            break;
        }
    }

    for (std::size_t frame = 0; frame < kTotalFrames; ++frame) {
        ASSERT_GT(outputWeights[frame], 0.0f);
        EXPECT_FLOAT_EQ(outputWeights[frame], 1.0f);
        EXPECT_FLOAT_EQ(output[frame] / outputWeights[frame], kWaveValue);
    }
}

TEST(StemConverterTest, PreservesFullWindowFadeRanges) {
    constexpr std::size_t kChunkWindowFrames = 8;
    constexpr std::size_t kChunkStepFrames = 6;
    constexpr std::size_t kTotalFrames = 20;

    EXPECT_FLOAT_EQ(StemConverter::getChunkFrameWeight(kChunkStepFrames,
                            0,
                            kChunkWindowFrames,
                            kChunkStepFrames,
                            kTotalFrames),
            0.5f);
    EXPECT_FLOAT_EQ(StemConverter::getChunkFrameWeight(kChunkStepFrames,
                            1,
                            kChunkWindowFrames,
                            kChunkStepFrames,
                            kTotalFrames),
            1.0f);
    EXPECT_FLOAT_EQ(StemConverter::getChunkFrameWeight(kChunkStepFrames,
                            5,
                            kChunkWindowFrames,
                            kChunkStepFrames,
                            kTotalFrames),
            1.0f);
    EXPECT_FLOAT_EQ(StemConverter::getChunkFrameWeight(kChunkStepFrames,
                            6,
                            kChunkWindowFrames,
                            kChunkStepFrames,
                            kTotalFrames),
            0.5f);
    EXPECT_FLOAT_EQ(StemConverter::getChunkFrameWeight(kChunkStepFrames,
                            7,
                            kChunkWindowFrames,
                            kChunkStepFrames,
                            kTotalFrames),
            0.0f);
}

TEST(StemConverterTest, DoesNotFadeOutWithoutNextWindow) {
    constexpr std::size_t kChunkWindowFrames = 8;
    constexpr std::size_t kChunkStepFrames = 6;

    EXPECT_FLOAT_EQ(StemConverter::getChunkFrameWeight(
                            0, 5, kChunkWindowFrames, kChunkStepFrames, 6),
            1.0f);
    EXPECT_FLOAT_EQ(StemConverter::getChunkFrameWeight(
                            kChunkStepFrames, 0, kChunkWindowFrames, kChunkStepFrames, 7),
            0.5f);
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
