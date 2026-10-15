#pragma once

#include <mfidl.h>
#include <mfreadwrite.h>

#ifdef BUILD_TESTING
#include <functional>
#endif

#include "sources/soundsourceprovider.h"
#include "util/readaheadsamplebuffer.h"

namespace mixxx {

class StreamUnitConverter final {
    const static SINT kStreamUnitsPerSecond = 1000 * 1000 * 10; // frame length = 100 ns

  public:
    StreamUnitConverter()
            : m_pAudioSource(nullptr),
              m_fromSampleFramesToStreamUnits(0),
              m_fromStreamUnitsToSampleFrames(0) {
    }
    explicit StreamUnitConverter(const AudioSource* pAudioSource)
            : m_pAudioSource(pAudioSource),
              m_fromSampleFramesToStreamUnits(double(kStreamUnitsPerSecond) / double(pAudioSource->getSignalInfo().getSampleRate())),
              m_fromStreamUnitsToSampleFrames(double(pAudioSource->getSignalInfo().getSampleRate()) / double(kStreamUnitsPerSecond)) {
        // The stream units should actually be much shorter than
        // sample frames to minimize jitter and rounding. Even a
        // frame at 192 kHz has a length of about 5000 ns >> 100 ns.
        DEBUG_ASSERT(m_fromSampleFramesToStreamUnits > 50);
        DEBUG_ASSERT(m_fromStreamUnitsToSampleFrames < 0.02);
    }

    LONGLONG fromFrameIndex(SINT frameIndex) const {
        DEBUG_ASSERT(m_fromSampleFramesToStreamUnits > 0);
        // Used for seeking, so we need to round down to hit the
        // corresponding stream unit where the given stream unit
        // starts. The reader will skip samples until it reaches
        // the actual target position for reading.
        const SINT frameIndexOffset = frameIndex - m_pAudioSource->frameIndexMin();
        return static_cast<LONGLONG>(floor(frameIndexOffset * m_fromSampleFramesToStreamUnits));
    }

    SINT toFrameIndex(LONGLONG streamPos) const {
        DEBUG_ASSERT(m_fromStreamUnitsToSampleFrames > 0);
        // The stream reports positions in units of 100ns. We have
        // to round(!) this value to obtain the actual position in
        // sample frames.
        const SINT frameIndexOffset = static_cast<SINT>(round(streamPos * m_fromStreamUnitsToSampleFrames));
        return m_pAudioSource->frameIndexMin() + frameIndexOffset;
    }

  private:
    const AudioSource* m_pAudioSource;
    double m_fromSampleFramesToStreamUnits;
    double m_fromStreamUnitsToSampleFrames;
};

class SoundSourceMediaFoundation : public SoundSource {
  public:
    static const QString kDisplayName;

    explicit SoundSourceMediaFoundation(const QUrl& url);
    ~SoundSourceMediaFoundation() override;

    void close() override;

  protected:
    ReadableSampleFrames readSampleFramesClamped(
            const WritableSampleFrames& sampleFrames) override;

  private:
    OpenResult tryOpen(
            OpenMode mode,
            const AudioSource::OpenParams& params) override;

    bool configureAudioStream(const AudioSource::OpenParams& params);
    bool readProperties();

    void seekSampleFrame(SINT frameIndex);
    ReadableSampleFrames readSampleFramesClampedInternal(
            const WritableSampleFrames& writableSampleFrames,
            SINT seekTargetFrameIndex);

    HRESULT m_hrCoInitialize;
    HRESULT m_hrMFStartup;

    IMFSourceReader* m_pSourceReader;

    StreamUnitConverter m_streamUnitConverter;

    // Next output frame; during a pending gap, the buffered sample may start
    // later at m_streamGapEndFrameIndex.
    SINT m_currentFrameIndex;
    // Tick start is held until the next sample; gap end is held until silence
    // has been emitted through the following sample's timestamp.
    SINT m_streamTickFrameIndex;
    SINT m_streamGapEndFrameIndex;

    ReadAheadSampleBuffer m_sampleBuffer;

#ifdef BUILD_TESTING
    using ReadSampleProvider = std::function<HRESULT(
            DWORD dwStreamIndex,
            DWORD dwControlFlags,
            DWORD* pdwStreamFlags,
            LONGLONG* pllTimestamp,
            IMFSample** ppSample)>;

    using ReadSampleObserver = std::function<void(
            HRESULT hr,
            DWORD dwStreamFlags,
            LONGLONG timestamp,
            bool hasSample)>;

    using SetCurrentPositionObserver = std::function<void(
            LONGLONG requestedPosition,
            HRESULT hr)>;

    ReadSampleProvider m_readSampleProvider;
    ReadSampleObserver m_readSampleObserver;
    SetCurrentPositionObserver m_setCurrentPositionObserver;

    std::function<HRESULT(LONGLONG)> m_setCurrentPositionProvider;

    friend class SoundSourceMediaFoundationTest;
#endif
};

class SoundSourceProviderMediaFoundation : public SoundSourceProvider {
  public:
    static const QString kDisplayName;
    static const QStringList kSupportedFileTypes;

    QString getDisplayName() const override {
        return kDisplayName + QChar(' ') + getVersionString();
    }

    QStringList getSupportedFileTypes() const override {
        return kSupportedFileTypes;
    }

    SoundSourceProviderPriority getPriorityHint(
            const QString& supportedFileType) const override;

    SoundSourcePointer newSoundSource(const QUrl& url) override;

    QString getVersionString() const;
};

} // namespace mixxx
