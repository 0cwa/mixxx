#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryFile>

#include "stems/stemconverter.h"

namespace {

// Discovery and model work are stubbed. No fixture file is executed, decoded,
// or loaded as a model, and no external process is launched by these tests.
class StubStemConverter final : public StemConverter {
  public:
    QString ffmpegPath;
    QString mp4boxPath;
    bool modelAvailable = true;
    int modelLoadCount = 0;
    int separationCount = 0;

  private:
    QString findFFmpeg() override {
        return ffmpegPath;
    }
    QString findMP4Box() override {
        return mp4boxPath;
    }
    bool loadOnnxModel() override {
        ++modelLoadCount;
        return modelAvailable;
    }
    bool runOnnxSeparation(const QString&, const QString&) override {
        ++separationCount;
        return false;
    }
};

} // namespace

class StemConverterPrerequisiteTest : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_input.open());
        ASSERT_TRUE(m_tool.open());
        ASSERT_TRUE(m_tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        m_converter.ffmpegPath = m_tool.fileName();
        m_converter.mp4boxPath = m_tool.fileName();
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

    QTemporaryFile m_input;
    QTemporaryFile m_tool;
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
    ASSERT_TRUE(m_tool.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
    // Keep FFmpeg usable so the MP4Box-specific failure is exercised.
    ASSERT_TRUE(m_input.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    m_converter.ffmpegPath = m_input.fileName();
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
