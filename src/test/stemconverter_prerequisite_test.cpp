#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "stems/stemconverter.h"

class StemConverterPrerequisiteTest : public testing::Test {
  protected:
    // All inference/decoder/process work is fake. Production separation and
    // packaging orchestration receive controlled data and write only fixtures.
    class StubStemConverter final : public StemConverter {
      public:
        QString ffmpegPath;
        QString mp4boxPath;
        bool modelAvailable = true;
        bool exercisePipeline = false;
        bool decodeSucceeds = true;
        bool mutateDiscoveryDuringModelLoad = false;
        bool processesSucceed = true;
        int modelLoadCount = 0;
        int separationCount = 0;
        int ffmpegDiscoveryCount = 0;
        int mp4boxDiscoveryCount = 0;
        QStringList decodePrograms;
        QStringList programs;
        QStringList descriptions;

      private:
        QString findFFmpeg() override {
            ++ffmpegDiscoveryCount;
            return ffmpegPath;
        }
        QString findMP4Box() override {
            ++mp4boxDiscoveryCount;
            return mp4boxPath;
        }
        bool loadOnnxModel() override {
            ++modelLoadCount;
            if (mutateDiscoveryDuringModelLoad) {
                ffmpegPath += QStringLiteral(".changed");
                mp4boxPath += QStringLiteral(".changed");
            }
            return modelAvailable;
        }
        bool runOnnxSeparation(const QString& input,
                const QString& output,
                const ExecutablePaths& paths) override {
            ++separationCount;
            return exercisePipeline &&
                    StemConverter::runOnnxSeparation(input, output, paths);
        }
        bool decodeAudioFile(const QString&,
                const QString& executable,
                int targetRate,
                std::vector<float>& data,
                int& rate,
                int& channels,
                std::size_t& frames) override {
            decodePrograms.append(executable);
            if (!decodeSucceeds) {
                return false;
            }
            rate = targetRate;
            channels = 2;
            frames = 2;
            data.assign(4, 0.0f);
            return true;
        }
        bool runInference(const std::vector<float>&,
                int,
                std::vector<std::vector<float>>& stems) override {
            stems.assign(4, std::vector<float>(4, 0.0f));
            return true;
        }
        bool startAndWaitProcess(QProcess* process,
                const QString& executable,
                const QStringList& arguments,
                const QString& description) override {
            programs.append(executable);
            descriptions.append(description);
            EXPECT_EQ(0, process->exitCode());
            EXPECT_EQ(QProcess::NormalExit, process->exitStatus());
            if (!processesSucceed) {
                return false;
            }
            // FFmpeg outputs are placeholders, never encoded media. MP4Box
            // receives the existing placeholder and never edits its contents.
            if (description != QStringLiteral("MP4Box metadata update")) {
                QFile output(arguments.last());
                return output.open(QIODevice::WriteOnly);
            }
            return QFileInfo::exists(arguments.last());
        }
    };

    void SetUp() override {
        ASSERT_TRUE(m_input.open());
        ASSERT_TRUE(m_tool.open());
        ASSERT_TRUE(m_mp4boxTool.open());
#ifndef Q_OS_WIN
        ASSERT_TRUE(m_tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        ASSERT_TRUE(m_mp4boxTool.setPermissions(
                QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
#endif
        ASSERT_TRUE(QFileInfo(m_tool.fileName()).isExecutable());
        ASSERT_TRUE(QFileInfo(m_mp4boxTool.fileName()).isExecutable());
        m_converter.ffmpegPath = m_tool.fileName();
        m_converter.mp4boxPath = m_mp4boxTool.fileName();
        QObject::connect(&m_converter,
                &StemConverter::conversionStarted,
                [this](TrackId, const QString&) { ++m_startedCount; });
        QObject::connect(&m_converter,
                &StemConverter::conversionProgress,
                [this](TrackId, float, const QString&) { ++m_progressCount; });
        QObject::connect(&m_converter,
                &StemConverter::conversionFailed,
                [this](TrackId, const QString& message) {
                    ++m_failureCount;
                    m_failureMessage = message;
                });
    }

    void convert() {
        m_converter.convertTrack(Track::newTemporary(m_input.fileName()));
    }

    void expectPrerequisiteFailure(const QString& message) {
        convert();
        EXPECT_EQ(m_converter.getState(), StemConverter::ConversionState::Failed);
        EXPECT_FLOAT_EQ(m_converter.getProgress(), 0.0f);
        EXPECT_EQ(m_failureCount, 1);
        EXPECT_EQ(m_failureMessage, message);
        EXPECT_EQ(m_startedCount, 0);
        EXPECT_EQ(m_progressCount, 0);
        EXPECT_EQ(m_converter.modelLoadCount, 0);
        EXPECT_EQ(m_converter.separationCount, 0);
    }

    QTemporaryDir m_directory;
    QTemporaryFile m_input{m_directory.filePath(QStringLiteral("input-XXXXXX.wav"))};
    // Qt checks executable suffixes on Windows and permission bits on Unix.
    QTemporaryFile m_tool{m_directory.filePath(QStringLiteral("tool-XXXXXX.exe"))};
    QTemporaryFile m_mp4boxTool{m_directory.filePath(QStringLiteral("mp4box-XXXXXX.exe"))};
    StubStemConverter m_converter;
    int m_startedCount = 0;
    int m_progressCount = 0;
    int m_failureCount = 0;
    QString m_failureMessage;
};

TEST_F(StemConverterPrerequisiteTest, MissingFFmpegFailsBeforeModelLoading) {
    m_converter.ffmpegPath.clear();
    expectPrerequisiteFailure(QStringLiteral("FFmpeg executable unavailable"));
}

TEST_F(StemConverterPrerequisiteTest, MissingMP4BoxFailsBeforeModelLoading) {
    m_converter.mp4boxPath.clear();
    expectPrerequisiteFailure(QStringLiteral("Accepted MP4Box executable unavailable"));
}

TEST_F(StemConverterPrerequisiteTest, RemovedMP4BoxFailsBeforeModelLoading) {
    m_converter.mp4boxPath += QStringLiteral(".missing");
    expectPrerequisiteFailure(QStringLiteral("Accepted MP4Box executable unavailable"));
}

TEST_F(StemConverterPrerequisiteTest, NonExecutableMP4BoxFailsBeforeModelLoading) {
    QTemporaryFile nonExecutableTool(
            QDir::temp().filePath(QStringLiteral("mixxx-stem-tool-XXXXXX.txt")));
    ASSERT_TRUE(nonExecutableTool.open());
#ifndef Q_OS_WIN
    ASSERT_TRUE(nonExecutableTool.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
#endif
    ASSERT_TRUE(QFileInfo(nonExecutableTool.fileName()).isFile());
    ASSERT_FALSE(QFileInfo(nonExecutableTool.fileName()).isExecutable());
    // FFmpeg keeps the accepted executable fixture; only MP4Box is unusable.
    m_converter.mp4boxPath = nonExecutableTool.fileName();
    expectPrerequisiteFailure(QStringLiteral("Accepted MP4Box executable unavailable"));
}

TEST_F(StemConverterPrerequisiteTest, DirectoryMP4BoxFailsBeforeModelLoading) {
    m_converter.mp4boxPath = QDir::tempPath();
    expectPrerequisiteFailure(QStringLiteral("Accepted MP4Box executable unavailable"));
}

TEST_F(StemConverterPrerequisiteTest, AcceptedToolsRetainModelFailure) {
    m_converter.modelAvailable = false;
    convert();
    EXPECT_EQ(m_converter.modelLoadCount, 1);
    EXPECT_EQ(m_converter.separationCount, 0);
    EXPECT_EQ(m_failureMessage, QStringLiteral("Verified HTDemucs model unavailable"));
    EXPECT_EQ(m_failureCount, 1);
    EXPECT_EQ(m_startedCount, 0);
    EXPECT_EQ(m_progressCount, 0);
    EXPECT_FLOAT_EQ(m_converter.getProgress(), 0.0f);
    EXPECT_EQ(m_converter.getState(), StemConverter::ConversionState::Failed);
}

TEST_F(StemConverterPrerequisiteTest, AcceptedToolsRetainSeparationFailure) {
    convert();
    EXPECT_EQ(m_converter.modelLoadCount, 1);
    EXPECT_EQ(m_converter.separationCount, 1);
    EXPECT_EQ(m_failureMessage, QStringLiteral("ONNX separation failed"));
    EXPECT_EQ(m_failureCount, 1);
    EXPECT_EQ(m_startedCount, 1);
    EXPECT_EQ(m_progressCount, 1);
    EXPECT_FLOAT_EQ(m_converter.getProgress(), 0.1f);
    EXPECT_EQ(m_converter.getState(), StemConverter::ConversionState::Failed);
}

TEST_F(StemConverterPrerequisiteTest, RemovedFFmpegFailsBeforeModelLoading) {
    m_converter.ffmpegPath += QStringLiteral(".missing");
    expectPrerequisiteFailure(QStringLiteral("FFmpeg executable unavailable"));
}

TEST_F(StemConverterPrerequisiteTest, DirectoryFFmpegFailsBeforeModelLoading) {
    m_converter.ffmpegPath = m_directory.path();
    expectPrerequisiteFailure(QStringLiteral("FFmpeg executable unavailable"));
}

TEST_F(StemConverterPrerequisiteTest, NonExecutableFFmpegFailsBeforeModelLoading) {
    QTemporaryFile tool(m_directory.filePath(QStringLiteral("tool-XXXXXX.txt")));
    ASSERT_TRUE(tool.open());
#ifndef Q_OS_WIN
    ASSERT_TRUE(tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
#endif
    ASSERT_FALSE(QFileInfo(tool.fileName()).isExecutable());
    m_converter.ffmpegPath = tool.fileName();
    expectPrerequisiteFailure(QStringLiteral("FFmpeg executable unavailable"));
}

TEST_F(StemConverterPrerequisiteTest, DecodeReceivesValidatedPathAfterDiscoveryChanges) {
    const QString acceptedFFmpeg = m_converter.ffmpegPath;
    m_converter.exercisePipeline = true;
    m_converter.decodeSucceeds = false;
    m_converter.mutateDiscoveryDuringModelLoad = true;
    convert();
    EXPECT_EQ(QStringList{acceptedFFmpeg}, m_converter.decodePrograms);
    EXPECT_EQ(1, m_converter.ffmpegDiscoveryCount);
    EXPECT_EQ(1, m_converter.mp4boxDiscoveryCount);
    EXPECT_TRUE(m_converter.programs.empty());
    EXPECT_EQ(QStringLiteral("ONNX separation failed"), m_failureMessage);
}

TEST_F(StemConverterPrerequisiteTest, AllPackagingCommandsReuseValidatedPaths) {
    const QString acceptedFFmpeg = m_converter.ffmpegPath;
    const QString acceptedMP4Box = m_converter.mp4boxPath;
    m_converter.exercisePipeline = true;
    m_converter.mutateDiscoveryDuringModelLoad = true;
    convert();
    EXPECT_EQ(StemConverter::ConversionState::Completed, m_converter.getState());
    EXPECT_EQ(0, m_failureCount);
    EXPECT_EQ(QStringList{acceptedFFmpeg}, m_converter.decodePrograms);
    ASSERT_EQ(7, m_converter.programs.size());
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(acceptedFFmpeg, m_converter.programs[i]);
    }
    EXPECT_EQ(acceptedMP4Box, m_converter.programs[6]);
    EXPECT_EQ(QStringLiteral("ffmpeg track conversion"), m_converter.descriptions[4]);
    EXPECT_EQ(QStringLiteral("ffmpeg STEM container creation"), m_converter.descriptions[5]);
    EXPECT_EQ(QStringLiteral("MP4Box metadata update"), m_converter.descriptions[6]);
    EXPECT_EQ(1, m_converter.ffmpegDiscoveryCount);
    EXPECT_EQ(1, m_converter.mp4boxDiscoveryCount);
}

TEST_F(StemConverterPrerequisiteTest, PackagingFailureDoesNotRediscoverTools) {
    const QString acceptedFFmpeg = m_converter.ffmpegPath;
    m_converter.exercisePipeline = true;
    m_converter.processesSucceed = false;
    m_converter.mutateDiscoveryDuringModelLoad = true;
    convert();
    EXPECT_EQ(QStringList{acceptedFFmpeg}, m_converter.decodePrograms);
    EXPECT_EQ(QStringList{acceptedFFmpeg}, m_converter.programs);
    EXPECT_EQ(QStringLiteral("Stem conversion to M4A failed"), m_failureMessage);
    EXPECT_EQ(1, m_converter.ffmpegDiscoveryCount);
    EXPECT_EQ(1, m_converter.mp4boxDiscoveryCount);
}

TEST_F(StemConverterPrerequisiteTest, NextOperationValidatesItsOwnResolvedPaths) {
    m_converter.exercisePipeline = true;
    convert();
    ASSERT_EQ(StemConverter::ConversionState::Completed, m_converter.getState());
    m_converter.ffmpegPath += QStringLiteral(".missing");
    m_converter.mp4boxPath += QStringLiteral(".missing");
    convert();
    EXPECT_EQ(StemConverter::ConversionState::Failed, m_converter.getState());
    EXPECT_EQ(QStringLiteral("FFmpeg executable unavailable"), m_failureMessage);
    EXPECT_EQ(2, m_converter.ffmpegDiscoveryCount);
    EXPECT_EQ(2, m_converter.mp4boxDiscoveryCount);
    EXPECT_EQ(1, m_converter.separationCount);
    EXPECT_EQ(7, m_converter.programs.size());
}
