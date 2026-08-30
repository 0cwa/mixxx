#include "stems/stemconverter.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUrl>
#include <algorithm>
#include <vector>

#include "sources/soundsourceffmpeg.h"
#include "test/mixxxtest.h"

#ifdef __STEM_CONVERSION__
class StemgenMasterConversionTest : public MixxxTest {
  protected:
    static bool convertMasterToM4A(const QString& inputPath, const QString& outputPath) {
        StemConverter converter;
        return converter.convertTrackToM4A(inputPath, outputPath);
    }
};

TEST(StemConverterTest, AcceptsModelMatchingExpectedSizeAndHash) {
    QTemporaryFile modelFile;
    ASSERT_TRUE(modelFile.open());
    const QByteArray modelContents = QByteArrayLiteral("valid model contents");
    ASSERT_EQ(modelFile.write(modelContents), modelContents.size());
    modelFile.close();

    EXPECT_TRUE(StemConverter::isVerifiedModelFile(
            modelFile.fileName(),
            modelContents.size(),
            QCryptographicHash::hash(modelContents, QCryptographicHash::Sha256).toHex()));
}

TEST(StemConverterTest, RejectsMissingAndUnmaterializedModels) {
    QTemporaryDir modelDir;
    ASSERT_TRUE(modelDir.isValid());
    const QByteArray expectedSha256 = QByteArrayLiteral("0123456789abcdef");

    EXPECT_FALSE(StemConverter::isVerifiedModelFile(
            QDir(modelDir.path()).filePath("missing.onnx"), 1, expectedSha256));

    const QString pointerPath = QDir(modelDir.path()).filePath("pointer.onnx");
    QFile pointerFile(pointerPath);
    ASSERT_TRUE(pointerFile.open(QIODevice::WriteOnly));
    ASSERT_GT(pointerFile.write(
                      "version https://git-lfs.github.com/spec/v1\n"
                      "oid sha256:0123456789abcdef\n"
                      "size 123\n"),
            0);
    pointerFile.close();

    EXPECT_FALSE(StemConverter::isVerifiedModelFile(pointerPath, 80, expectedSha256));
}

TEST(StemConverterTest, RejectsWrongModelSizeAndHash) {
    QTemporaryFile modelFile;
    ASSERT_TRUE(modelFile.open());
    const QByteArray modelContents = QByteArrayLiteral("model contents");
    ASSERT_EQ(modelFile.write(modelContents), modelContents.size());
    modelFile.close();

    const QByteArray modelSha256 =
            QCryptographicHash::hash(modelContents, QCryptographicHash::Sha256).toHex();
    EXPECT_FALSE(StemConverter::isVerifiedModelFile(
            modelFile.fileName(), modelContents.size() + 1, modelSha256));
    EXPECT_FALSE(StemConverter::isVerifiedModelFile(
            modelFile.fileName(), modelContents.size(), QByteArrayLiteral("wrong")));
}

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

TEST(StemConverterTest, DoesNotFindModelInGenericParentDirectory) {
    QTemporaryDir parentDir;
    ASSERT_TRUE(parentDir.isValid());
    QTemporaryDir userModelDir;
    ASSERT_TRUE(userModelDir.isValid());

    const QString resourceDir = QDir(parentDir.path()).filePath("res");
    ASSERT_TRUE(QDir().mkpath(resourceDir));

    QFile genericProjectMarker(QDir(parentDir.path()).filePath("CMakeLists.txt"));
    ASSERT_TRUE(genericProjectMarker.open(QIODevice::WriteOnly));
    genericProjectMarker.close();

    const QString modelsDir = QDir(parentDir.path()).filePath("models");
    ASSERT_TRUE(QDir().mkpath(modelsDir));
    QFile siblingModel(QDir(modelsDir).filePath("htdemucs.onnx"));
    ASSERT_TRUE(siblingModel.open(QIODevice::WriteOnly));
    siblingModel.close();

    const QString userModelPath = QDir(userModelDir.path()).filePath("htdemucs.onnx");
    QFile userModel(userModelPath);
    ASSERT_TRUE(userModel.open(QIODevice::WriteOnly));
    userModel.close();

    EXPECT_EQ(StemConverter::findModelPath(
                      QStringLiteral("htdemucs.onnx"),
                      QString(),
                      resourceDir,
                      userModelDir.path()),
            userModelPath);
}

TEST(StemConverterTest, DoesNotFindModelInUnrelatedSourceShapedDirectory) {
    QTemporaryDir parentDir;
    ASSERT_TRUE(parentDir.isValid());
    QTemporaryDir userModelDir;
    ASSERT_TRUE(userModelDir.isValid());

    const QString resourceDir = QDir(parentDir.path()).filePath("res");
    ASSERT_TRUE(QDir().mkpath(resourceDir));

    const QString manifestPath =
            QDir(parentDir.path())
                    .filePath("models/htdemucs.onnx.manifest.json");
    ASSERT_TRUE(QDir().mkpath(QFileInfo(manifestPath).path()));
    QFile modelManifest(manifestPath);
    ASSERT_TRUE(modelManifest.open(QIODevice::WriteOnly));
    modelManifest.close();

    const QString stemSourcePath =
            QDir(parentDir.path()).filePath("src/stems/stemconverter.cpp");
    ASSERT_TRUE(QDir().mkpath(QFileInfo(stemSourcePath).path()));
    QFile stemSource(stemSourcePath);
    ASSERT_TRUE(stemSource.open(QIODevice::WriteOnly));
    stemSource.close();

    const QString siblingModelPath =
            QDir(parentDir.path()).filePath("models/htdemucs.onnx");
    QFile siblingModel(siblingModelPath);
    ASSERT_TRUE(siblingModel.open(QIODevice::WriteOnly));
    siblingModel.close();

    const QString userModelPath = QDir(userModelDir.path()).filePath("htdemucs.onnx");
    QFile userModel(userModelPath);
    ASSERT_TRUE(userModel.open(QIODevice::WriteOnly));
    userModel.close();

    EXPECT_EQ(StemConverter::findModelPath(
                      QStringLiteral("htdemucs.onnx"),
                      QString(),
                      resourceDir,
                      userModelDir.path()),
            userModelPath);
}

TEST(StemConverterTest, FindsModelInConfiguredOrUninstalledSourceTree) {
    QTemporaryDir userModelDir;
    ASSERT_TRUE(userModelDir.isValid());

    const QString resourceDir =
            QStringLiteral(MIXXX_STEM_MODEL_SOURCE_RESOURCE_PATH);
    const QString sourceTreeModelPath = QFileInfo(
            QDir(resourceDir).filePath("../models/htdemucs.onnx"))
                                                .absoluteFilePath();
    const QString modelDirectoryOverride =
            qEnvironmentVariable("MIXXX_STEM_MODEL_DIR");
    const QString modelPath = modelDirectoryOverride.isEmpty()
            ? sourceTreeModelPath
            : QFileInfo(QDir(modelDirectoryOverride).filePath("htdemucs.onnx"))
                      .absoluteFilePath();
    ASSERT_TRUE(QFileInfo(modelPath).isFile());

    EXPECT_EQ(StemConverter::findModelPath(
                      QStringLiteral("htdemucs.onnx"),
                      modelDirectoryOverride,
                      resourceDir,
                      userModelDir.path()),
            QFileInfo(modelPath).absoluteFilePath());
}

TEST(StemConverterTest, SkipsUnmaterializedGitLfsPointerInResourceDirectory) {
    QTemporaryDir resourceDir;
    ASSERT_TRUE(resourceDir.isValid());
    QTemporaryDir userModelDir;
    ASSERT_TRUE(userModelDir.isValid());

    const QString modelsDir = QDir(resourceDir.path()).filePath("models");
    ASSERT_TRUE(QDir().mkpath(modelsDir));
    QFile pointerFile(QDir(modelsDir).filePath("htdemucs.onnx"));
    ASSERT_TRUE(pointerFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(pointerFile.write(
                      "version https://git-lfs.github.com/spec/v1\n"
                      "oid sha256:0123456789abcdef\n"
                      "size 123\n"),
            80);
    pointerFile.close();

    const QString userModelPath = QDir(userModelDir.path()).filePath("htdemucs.onnx");
    QFile userModelFile(userModelPath);
    ASSERT_TRUE(userModelFile.open(QIODevice::WriteOnly));
    userModelFile.close();

    EXPECT_EQ(StemConverter::findModelPath(
                      QStringLiteral("htdemucs.onnx"),
                      QString(),
                      resourceDir.path(),
                      userModelDir.path()),
            userModelPath);
}

TEST(StemConverterTest, ExplicitGitLfsPointerDoesNotFallBack) {
    QTemporaryDir resourceDir;
    ASSERT_TRUE(resourceDir.isValid());
    QTemporaryDir overrideDir;
    ASSERT_TRUE(overrideDir.isValid());

    const QString modelsDir = QDir(resourceDir.path()).filePath("models");
    ASSERT_TRUE(QDir().mkpath(modelsDir));
    QFile resourceModelFile(QDir(modelsDir).filePath("htdemucs.onnx"));
    ASSERT_TRUE(resourceModelFile.open(QIODevice::WriteOnly));
    resourceModelFile.close();

    QFile pointerFile(QDir(overrideDir.path()).filePath("htdemucs.onnx"));
    ASSERT_TRUE(pointerFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(pointerFile.write(
                      "version https://git-lfs.github.com/spec/v1\n"
                      "oid sha256:0123456789abcdef\n"
                      "size 123\n"),
            80);
    pointerFile.close();

    EXPECT_TRUE(StemConverter::findModelPath(
            QStringLiteral("htdemucs.onnx"),
            overrideDir.path(),
            resourceDir.path(),
            QString())
                    .isEmpty());
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

TEST_F(MixxxTest, ConvertsRealAudioWithStemgenModel) {
    const QString inputPath = getTestDataDir().filePath("stemgen-smoke.wav");
    ASSERT_TRUE(QFile::copy(getTestDir().filePath("stems/mainmix.wav"), inputPath));

    const auto pTrack = Track::newTemporary(inputPath);
    ASSERT_TRUE(pTrack);

    StemConverter converter;
    bool conversionCompleted = false;
    QString failureMessage;
    QObject::connect(
            &converter,
            &StemConverter::conversionCompleted,
            [&conversionCompleted](TrackId) { conversionCompleted = true; });
    QObject::connect(
            &converter,
            &StemConverter::conversionFailed,
            [&failureMessage](TrackId, const QString& message) { failureMessage = message; });

    converter.convertTrack(pTrack, StemConverter::Resolution::Low);

    ASSERT_TRUE(conversionCompleted) << qPrintable(failureMessage);
    ASSERT_EQ(converter.getState(), StemConverter::ConversionState::Completed);

    const QFileInfo outputFile(
            getTestDataDir().filePath("stemgen-smoke.stem.m4a"));
    ASSERT_TRUE(outputFile.isFile());
    EXPECT_GT(outputFile.size(), 0);
}

TEST_F(StemgenMasterConversionTest, ConvertsMonoMasterToStereoM4A) {
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const QString inputPath = QDir(tempDir.path()).filePath("mono.wav");
    ASSERT_TRUE(QFile::copy(getTestDir().filePath("sine-30.wav"), inputPath));
    const QString outputPath = QDir(tempDir.path()).filePath("master.m4a");

    ASSERT_TRUE(convertMasterToM4A(inputPath, outputPath));
    ASSERT_TRUE(QFileInfo::exists(outputPath));

    mixxx::SoundSourceFFmpeg outputSource(QUrl::fromLocalFile(outputPath));
    mixxx::AudioSource::OpenParams openParams;
    ASSERT_EQ(outputSource.open(mixxx::AudioSource::OpenMode::Strict, openParams),
            mixxx::AudioSource::OpenResult::Succeeded);
    EXPECT_EQ(outputSource.getSignalInfo().getChannelCount(),
            mixxx::audio::ChannelCount::stereo());
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

#ifdef Q_OS_LINUX
TEST(StemConverterTest, TrustsOnlyExecutableMp4BoxInFlatpakPrefix) {
    QTemporaryDir flatpakPrefix;
    ASSERT_TRUE(flatpakPrefix.isValid());
    const QString binDir = QDir(flatpakPrefix.path()).filePath("bin");
    ASSERT_TRUE(QDir().mkpath(binDir));

    const QString mp4boxPath = QDir(binDir).filePath("MP4Box");
    QFile mp4boxFile(mp4boxPath);
    ASSERT_TRUE(mp4boxFile.open(QIODevice::WriteOnly));
    mp4boxFile.close();
    ASSERT_TRUE(mp4boxFile.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

    EXPECT_TRUE(StemConverter::isTrustedFlatpakMP4BoxPath(
            mp4boxPath, flatpakPrefix.path()));

    QTemporaryDir outsidePrefix;
    ASSERT_TRUE(outsidePrefix.isValid());
    const QString outsideBinDir = QDir(outsidePrefix.path()).filePath("bin");
    ASSERT_TRUE(QDir().mkpath(outsideBinDir));
    const QString outsidePath = QDir(outsideBinDir).filePath("MP4Box");
    QFile outsideFile(outsidePath);
    ASSERT_TRUE(outsideFile.open(QIODevice::WriteOnly));
    outsideFile.close();
    ASSERT_TRUE(outsideFile.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    EXPECT_FALSE(StemConverter::isTrustedFlatpakMP4BoxPath(
            outsidePath, flatpakPrefix.path()));

    ASSERT_TRUE(mp4boxFile.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    EXPECT_FALSE(StemConverter::isTrustedFlatpakMP4BoxPath(
            mp4boxPath, flatpakPrefix.path()));
}
#endif

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
