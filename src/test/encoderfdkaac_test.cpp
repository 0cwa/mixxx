#include "encoder/encoderfdkaac.h"

#include <gtest/gtest.h>

#include <QDebug>
#include <QStringList>
#include <algorithm>
#include <memory>
#include <mutex>

#include "encoder/encodercallback.h"
#include "encoder/encodersettings.h"
#include "recording/defs_recording.h"

namespace {

std::mutex s_captureMutex;
QStringList* s_pCapturedWarnings = nullptr;

void captureQtWarnings(
        QtMsgType type,
        const QMessageLogContext&,
        const QString& message) {
    if (type != QtWarningMsg) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_captureMutex);
    if (s_pCapturedWarnings) {
        s_pCapturedWarnings->append(message);
    }
}

class ScopedQtWarningCapture final {
  public:
    explicit ScopedQtWarningCapture(QStringList* pWarnings)
            : m_previousHandler(qInstallMessageHandler(captureQtWarnings)) {
        std::lock_guard<std::mutex> lock(s_captureMutex);
        s_pCapturedWarnings = pWarnings;
    }

    ~ScopedQtWarningCapture() {
        qInstallMessageHandler(m_previousHandler);
        std::lock_guard<std::mutex> lock(s_captureMutex);
        s_pCapturedWarnings = nullptr;
    }

    ScopedQtWarningCapture(const ScopedQtWarningCapture&) = delete;
    ScopedQtWarningCapture& operator=(const ScopedQtWarningCapture&) = delete;

  private:
    QtMessageHandler m_previousHandler;
};

class AcceptanceCallback final : public EncoderCallback {
  public:
    void write(const unsigned char*, const unsigned char*, int, int) override {
    }
    int tell() override {
        return 0;
    }
    void seek(int) override {
    }
    int filelen() override {
        return 0;
    }
};

class AacLcStereo128Settings final : public EncoderSettings {
  public:
    QString getFormat() const override {
        return QString::fromLatin1(ENCODING_AAC);
    }
    int getQuality() const override {
        return 128;
    }
    ChannelMode getChannelMode() const override {
        return ChannelMode::STEREO;
    }
};

TEST(FdkAacNativeAcceptance, RejectsProviderMissingAacEncInfo) {
    AcceptanceCallback callback;
    QStringList warnings;
    std::unique_ptr<EncoderFdkAac> encoder;
    {
        ScopedQtWarningCapture warningCapture(&warnings);
        encoder = std::make_unique<EncoderFdkAac>(&callback);
    }

    QString userErrorMessage;
    EXPECT_EQ(
            -1,
            encoder->initEncoder(
                    mixxx::audio::SampleRate(44100), &userErrorMessage));
    EXPECT_FALSE(userErrorMessage.isEmpty());
    EXPECT_TRUE(userErrorMessage.contains(
            QStringLiteral("FDK-AAC"), Qt::CaseInsensitive))
            << userErrorMessage.toStdString();
    encoder.reset(); // The destructor must remain safe after rejection.

    const auto rejectionWarning = std::find_if(
            warnings.cbegin(),
            warnings.cend(),
            [](const QString& warning) {
                return warning.contains(
                        QStringLiteral("Interface of"), Qt::CaseInsensitive);
            });
    ASSERT_NE(warnings.cend(), rejectionWarning)
            << warnings.join(QLatin1Char('\n')).toStdString();
    // QLibrary receives a basename in EncoderFdkAac, so this warning identifies
    // the library name. The native loader trace proves the selected full path.
    EXPECT_TRUE(rejectionWarning->contains(
            QStringLiteral("fdk-aac"), Qt::CaseInsensitive))
            << rejectionWarning->toStdString();
}

TEST(FdkAacNativeAcceptance, InitializesCompatibleProvider) {
    AcceptanceCallback callback;
    EncoderFdkAac encoder(&callback);
    encoder.setEncoderSettings(AacLcStereo128Settings());

    QString userErrorMessage;
    EXPECT_EQ(
            0,
            encoder.initEncoder(
                    mixxx::audio::SampleRate(44100), &userErrorMessage))
            << userErrorMessage.toStdString();
    // encoder's destructor performs normal FDK cleanup at scope exit.
}

} // namespace
