#include "sources/soundsourcemediafoundation.h"

#include <gtest/gtest.h>
#include <mfapi.h>

#include <QCryptographicHash>
#include <QFile>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include "test/mixxxtest.h"
#include "util/sample.h"

namespace mixxx {

namespace {

constexpr SINT kUnknownFrameIndex = -1;
constexpr LONGLONG kStreamUnitsPerSecond = 10'000'000;

class TestSoundSourceMediaFoundation final : public SoundSourceMediaFoundation {
  public:
    using SoundSourceMediaFoundation::readSampleFramesClamped;
    using SoundSourceMediaFoundation::SoundSourceMediaFoundation;
};

IMFSample* createSample(
        const audio::SignalInfo& signalInfo,
        SINT frameCount,
        CSAMPLE firstSampleValue) {
    IMFSample* pSample = nullptr;
    HRESULT hr = MFCreateSample(&pSample);
    if (FAILED(hr)) {
        ADD_FAILURE() << "MFCreateSample failed: " << std::hex << hr;
        return nullptr;
    }

    const SINT sampleCount = signalInfo.frames2samples(frameCount);
    const DWORD byteCount = static_cast<DWORD>(sampleCount * sizeof(CSAMPLE));
    IMFMediaBuffer* pBuffer = nullptr;
    hr = MFCreateMemoryBuffer(byteCount, &pBuffer);
    if (FAILED(hr)) {
        ADD_FAILURE() << "MFCreateMemoryBuffer failed: " << std::hex << hr;
        pSample->Release();
        return nullptr;
    }

    BYTE* pData = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = pBuffer->Lock(&pData, &maxLength, &currentLength);
    if (FAILED(hr)) {
        ADD_FAILURE() << "IMFMediaBuffer::Lock failed: " << std::hex << hr;
        pBuffer->Release();
        pSample->Release();
        return nullptr;
    }
    auto* pSamples = reinterpret_cast<CSAMPLE*>(pData);
    for (SINT i = 0; i < sampleCount; ++i) {
        pSamples[i] = firstSampleValue + static_cast<CSAMPLE>(i);
    }
    hr = pBuffer->Unlock();
    if (FAILED(hr)) {
        ADD_FAILURE() << "IMFMediaBuffer::Unlock failed: " << std::hex << hr;
        pBuffer->Release();
        pSample->Release();
        return nullptr;
    }
    hr = pBuffer->SetCurrentLength(byteCount);
    if (FAILED(hr)) {
        ADD_FAILURE() << "IMFMediaBuffer::SetCurrentLength failed: " << std::hex << hr;
        pBuffer->Release();
        pSample->Release();
        return nullptr;
    }
    hr = pSample->AddBuffer(pBuffer);
    pBuffer->Release();
    if (FAILED(hr)) {
        ADD_FAILURE() << "IMFSample::AddBuffer failed: " << std::hex << hr;
        pSample->Release();
        return nullptr;
    }
    return pSample;
}

struct ReadSampleEvent {
    ReadSampleEvent(
            HRESULT hrIn,
            DWORD flagsIn,
            SINT frameIndexIn,
            SINT frameCountIn,
            CSAMPLE firstSampleValueIn = 1)
            : hr(hrIn),
              flags(flagsIn),
              frameIndex(frameIndexIn),
              frameCount(frameCountIn),
              firstSampleValue(firstSampleValueIn) {
    }

    HRESULT hr;
    DWORD flags;
    SINT frameIndex;
    SINT frameCount;
    CSAMPLE firstSampleValue;
};

enum class ReadSamplePhase {
    SeekPrefetch,
    AfterSeekReturn,
};

struct RawReadSampleObservation {
    ReadSamplePhase phase;
    HRESULT hr;
    DWORD flags;
    LONGLONG timestamp;
    bool hasSample;
};

struct SetCurrentPositionObservation {
    LONGLONG requestedPosition;
    HRESULT hr;
};

const char* readSamplePhaseName(ReadSamplePhase phase) {
    switch (phase) {
    case ReadSamplePhase::SeekPrefetch:
        return "seek_prefetch";
    case ReadSamplePhase::AfterSeekReturn:
        return "after_seek_return";
    }
    return "unknown";
}

class ReadSampleQueue final {
  public:
    ReadSampleQueue(
            const audio::SignalInfo& signalInfo,
            SINT frameIndexMin)
            : m_signalInfo(signalInfo),
              m_frameIndexMin(frameIndexMin) {
    }

    void push(ReadSampleEvent event) {
        m_events.push_back(event);
    }

    HRESULT operator()(
            DWORD streamIndex,
            DWORD controlFlags,
            DWORD* pFlags,
            LONGLONG* pTimestamp,
            IMFSample** ppSample) {
        EXPECT_EQ(MF_SOURCE_READER_FIRST_AUDIO_STREAM, streamIndex);
        EXPECT_EQ(0u, controlFlags);
        ++m_callCount;
        *pFlags = 0;
        *pTimestamp = 0;
        *ppSample = nullptr;
        if (m_events.empty()) {
            ADD_FAILURE() << "ReadSample provider was called after its sequence ended";
            return E_UNEXPECTED;
        }

        const ReadSampleEvent event = m_events.front();
        m_events.pop_front();
        *pFlags = event.flags;
        *pTimestamp = timestampForFrame(event.frameIndex);
        if (SUCCEEDED(event.hr) && event.frameCount > 0) {
            *ppSample = createSample(
                    m_signalInfo,
                    event.frameCount,
                    event.firstSampleValue);
        }
        return event.hr;
    }

    int callCount() const {
        return m_callCount;
    }

  private:
    LONGLONG timestampForFrame(SINT frameIndex) const {
        return static_cast<LONGLONG>(std::llround(
                (frameIndex - m_frameIndexMin) *
                static_cast<double>(kStreamUnitsPerSecond) /
                static_cast<double>(m_signalInfo.getSampleRate())));
    }

    const audio::SignalInfo m_signalInfo;
    const SINT m_frameIndexMin;
    std::deque<ReadSampleEvent> m_events;
    int m_callCount{0};
};

} // namespace

class SoundSourceMediaFoundationTest : public ::MixxxTest {
  protected:
    QUrl testUrl() const {
        return QUrl::fromLocalFile(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    }

    QUrl timelineGapTestUrl() const {
        return QUrl::fromLocalFile(getTestDir().filePath(
                QStringLiteral("soundFileFormats/issue92_streamtick_gap.mp4")));
    }

    template<typename Provider>
    void setReadSampleProvider(
            SoundSourceMediaFoundation& source,
            Provider&& provider) {
        source.m_readSampleProvider = std::forward<Provider>(provider);
    }

    template<typename Observer>
    void setReadSampleObserver(
            SoundSourceMediaFoundation& source,
            Observer&& observer) {
        source.m_readSampleObserver = std::forward<Observer>(observer);
    }

    template<typename Observer>
    void setCurrentPositionObserver(
            SoundSourceMediaFoundation& source,
            Observer&& observer) {
        source.m_setCurrentPositionObserver = std::forward<Observer>(observer);
    }

    void clearCurrentPositionObserver(SoundSourceMediaFoundation& source) {
        source.m_setCurrentPositionObserver = {};
    }

    void expectRealReadSamplePath(const SoundSourceMediaFoundation& source) {
        EXPECT_FALSE(source.m_readSampleProvider);
        EXPECT_FALSE(source.m_setCurrentPositionProvider);
    }

    template<typename Provider>
    void setCurrentPositionProvider(
            SoundSourceMediaFoundation& source,
            Provider&& provider) {
        source.m_setCurrentPositionProvider = std::forward<Provider>(provider);
    }

    void setReadState(
            SoundSourceMediaFoundation& source,
            SINT currentFrameIndex,
            SINT streamTickFrameIndex,
            SINT streamGapEndFrameIndex) {
        source.m_currentFrameIndex = currentFrameIndex;
        source.m_streamTickFrameIndex = streamTickFrameIndex;
        source.m_streamGapEndFrameIndex = streamGapEndFrameIndex;
        source.m_sampleBuffer.clear();
    }

    struct ReadState {
        SINT currentFrameIndex;
        SINT streamTickFrameIndex;
        SINT streamGapEndFrameIndex;
    };

    void seekTo(SoundSourceMediaFoundation& source, SINT frameIndex) {
        source.seekSampleFrame(frameIndex);
    }

    ReadState readState(const SoundSourceMediaFoundation& source) const {
        return {
                source.m_currentFrameIndex,
                source.m_streamTickFrameIndex,
                source.m_streamGapEndFrameIndex};
    }

    SINT streamPositionToFrameIndexForTest(
            const SoundSourceMediaFoundation& source,
            LONGLONG streamPosition) const {
        return source.m_streamUnitConverter.toFrameIndex(streamPosition);
    }

    LONGLONG frameIndexToStreamPositionForTest(
            const SoundSourceMediaFoundation& source,
            SINT frameIndex) const {
        return source.m_streamUnitConverter.fromFrameIndex(frameIndex);
    }

    SINT readableSampleCount(const SoundSourceMediaFoundation& source) const {
        return source.m_sampleBuffer.readableLength();
    }

    SINT readableFrameCount(const SoundSourceMediaFoundation& source) const {
        return source.getSignalInfo().samples2frames(
                source.m_sampleBuffer.readableLength());
    }
};

TEST_F(SoundSourceMediaFoundationTest, StreamTickNullThenSamplePreservesShortGap) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());

    const SINT firstFrameIndex = source.frameIndexMin();
    setReadState(
            source,
            firstFrameIndex,
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), firstFrameIndex);
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, firstFrameIndex, 0});
    readSamples.push({S_OK, 0, firstFrameIndex + 4, 4});
    setReadSampleProvider(
            source,
            [&readSamples](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                return readSamples(
                        streamIndex,
                        controlFlags,
                        pFlags,
                        pTimestamp,
                        ppSample);
            });

    SampleBuffer firstBuffer(source.getSignalInfo().frames2samples(2));
    const auto firstRange = IndexRange::forward(firstFrameIndex, 2);
    const auto firstResult = source.readSampleFrames(
            WritableSampleFrames(firstRange, SampleBuffer::WritableSlice(firstBuffer)));
    ASSERT_EQ(firstRange, firstResult.frameIndexRange());
    const auto firstState = readState(source);
    EXPECT_EQ(firstFrameIndex + 2, firstState.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, firstState.streamTickFrameIndex);
    EXPECT_EQ(firstFrameIndex + 4, firstState.streamGapEndFrameIndex);
    EXPECT_EQ(2, readSamples.callCount());
    EXPECT_EQ(0, firstBuffer[0]);
    EXPECT_EQ(0, firstBuffer[1]);

    SampleBuffer secondBuffer(source.getSignalInfo().frames2samples(4));
    const auto secondRange = IndexRange::forward(firstFrameIndex + 2, 4);
    const auto secondResult = source.readSampleFrames(
            WritableSampleFrames(secondRange, SampleBuffer::WritableSlice(secondBuffer)));
    ASSERT_EQ(secondRange, secondResult.frameIndexRange());
    const auto secondState = readState(source);
    EXPECT_EQ(firstFrameIndex + 6, secondState.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, secondState.streamGapEndFrameIndex);
    EXPECT_EQ(
            source.getSignalInfo().frames2samples(2),
            readableSampleCount(source));
    EXPECT_EQ(0, secondBuffer[0]);
    EXPECT_EQ(0, secondBuffer[1]);
    EXPECT_EQ(1, secondBuffer[2]);
    EXPECT_EQ(2, secondBuffer[3]);
}

TEST_F(SoundSourceMediaFoundationTest, TerminalNullStatusesSettleUnknownPosition) {
    struct TerminalEvent {
        const char* name;
        HRESULT hr;
        DWORD flags;
    };
    const TerminalEvent terminalEvents[] = {
            {"failed HRESULT", E_FAIL, 0},
            {"stream error", S_OK, MF_SOURCE_READERF_ERROR},
            {"end of stream", S_OK, MF_SOURCE_READERF_ENDOFSTREAM},
            {"unrecognized null", S_OK, 0},
            {"media type changed", S_OK, MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED},
    };

    for (const auto& terminalEvent : terminalEvents) {
        TestSoundSourceMediaFoundation source(testUrl());
        ASSERT_EQ(
                AudioSource::OpenResult::Succeeded,
                source.open(AudioSource::OpenMode::Strict))
                << terminalEvent.name;

        setReadState(
                source,
                kUnknownFrameIndex,
                kUnknownFrameIndex,
                kUnknownFrameIndex);

        ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
        readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, 10, 0});
        readSamples.push({terminalEvent.hr, terminalEvent.flags, 12, 0});
        setReadSampleProvider(
                source,
                [&readSamples](
                        DWORD streamIndex,
                        DWORD controlFlags,
                        DWORD* pFlags,
                        LONGLONG* pTimestamp,
                        IMFSample** ppSample) {
                    return readSamples(
                            streamIndex,
                            controlFlags,
                            pFlags,
                            pTimestamp,
                            ppSample);
                });

        SampleBuffer outputBuffer(source.getSignalInfo().frames2samples(4));
        const auto requestedRange = IndexRange::forward(1000, 4);
        const auto result = source.readSampleFramesClamped(
                WritableSampleFrames(
                        requestedRange,
                        SampleBuffer::WritableSlice(outputBuffer)));
        EXPECT_EQ(IndexRange::forward(1000, 0), result.frameIndexRange())
                << terminalEvent.name;
        const auto state = readState(source);
        EXPECT_EQ(source.frameIndexMax(), state.currentFrameIndex)
                << terminalEvent.name;
        EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex)
                << terminalEvent.name;
        EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex)
                << terminalEvent.name;
        EXPECT_EQ(2, readSamples.callCount()) << terminalEvent.name;
    }
}

TEST_F(SoundSourceMediaFoundationTest, TerminalNullStatusesPreserveKnownPosition) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));

    const SINT currentFrameIndex = source.frameIndexMin();
    setReadState(
            source,
            currentFrameIndex,
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), currentFrameIndex);
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, currentFrameIndex, 0});
    readSamples.push({S_OK, MF_SOURCE_READERF_ENDOFSTREAM, currentFrameIndex, 0});
    setReadSampleProvider(
            source,
            [&readSamples](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                return readSamples(
                        streamIndex,
                        controlFlags,
                        pFlags,
                        pTimestamp,
                        ppSample);
            });

    SampleBuffer outputBuffer(source.getSignalInfo().frames2samples(4));
    const auto requestedRange = IndexRange::forward(currentFrameIndex, 4);
    const auto result = source.readSampleFramesClamped(
            WritableSampleFrames(
                    requestedRange,
                    SampleBuffer::WritableSlice(outputBuffer)));
    EXPECT_EQ(IndexRange::forward(currentFrameIndex, 0), result.frameIndexRange());
    const auto state = readState(source);
    EXPECT_EQ(currentFrameIndex, state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(2, readSamples.callCount());
}

TEST_F(SoundSourceMediaFoundationTest, FirstPostSeekTickSampleBeforeTargetReachesTarget) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));

    constexpr SINT kPrefetchFrames = 2112;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    const SINT firstSampleFrame = seekFrame - 8;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, seekFrame - 12, 0});
    readSamples.push({S_OK, 0, firstSampleFrame, kPrefetchFrames});
    readSamples.push({S_OK, 0, requestedFrame - 8, 8});
    SINT currentBeforeFirstSample = source.frameIndexMax();
    setReadSampleProvider(
            source,
            [this, &source, &readSamples, &currentBeforeFirstSample](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                const HRESULT hr = readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
                if (readSamples.callCount() == 2) {
                    currentBeforeFirstSample = readState(source).currentFrameIndex;
                }
                return hr;
            });
    SINT positionedAt = kUnknownFrameIndex;
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto state = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(kUnknownFrameIndex, currentBeforeFirstSample);
    EXPECT_EQ(requestedFrame, state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(3, readSamples.callCount());
}

TEST_F(
        SoundSourceMediaFoundationTest,
        FirstPostSeekReadOvershootingTargetIsRejected) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));

    constexpr SINT kPrefetchFrames = 2112;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    const SINT overshootingSampleFrame = seekFrame + 8;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, seekFrame + 4, 0});
    readSamples.push({S_OK, 0, overshootingSampleFrame, kPrefetchFrames});
    SINT currentBeforeFirstSample = source.frameIndexMax();
    setReadSampleProvider(
            source,
            [this, &source, &readSamples, &currentBeforeFirstSample](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                const HRESULT hr = readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
                if (readSamples.callCount() == 2) {
                    currentBeforeFirstSample = readState(source).currentFrameIndex;
                }
                return hr;
            });
    SINT positionedAt = kUnknownFrameIndex;
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto state = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(kUnknownFrameIndex, currentBeforeFirstSample);
    EXPECT_EQ(source.frameIndexMax(), state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(2, readSamples.callCount());
}

TEST_F(
        SoundSourceMediaFoundationTest,
        FirstPostSeekTickBracketedGapPreservesOutput) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());

    constexpr SINT kPrefetchFrames = 2112;
    constexpr SINT kGapLength = 6;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    const SINT tickFrame = requestedFrame - 4;
    const SINT nextSampleFrame = requestedFrame + kGapLength;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, tickFrame, 0});
    readSamples.push({S_OK, 0, nextSampleFrame, 4, 41});
    SINT currentBeforeFirstSample = source.frameIndexMax();
    setReadSampleProvider(
            source,
            [this, &source, &readSamples, &currentBeforeFirstSample](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                const HRESULT hr = readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
                if (readSamples.callCount() == 2) {
                    currentBeforeFirstSample = readState(source).currentFrameIndex;
                }
                return hr;
            });
    SINT positionedAt = kUnknownFrameIndex;
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto seekState = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(kUnknownFrameIndex, currentBeforeFirstSample);
    EXPECT_EQ(requestedFrame, seekState.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, seekState.streamTickFrameIndex);
    EXPECT_EQ(nextSampleFrame, seekState.streamGapEndFrameIndex);
    EXPECT_EQ(source.getSignalInfo().frames2samples(4), readableSampleCount(source));
    EXPECT_EQ(2, readSamples.callCount());

    SampleBuffer firstOutput(source.getSignalInfo().frames2samples(2));
    const auto firstRange = IndexRange::forward(requestedFrame, 2);
    const auto firstResult = source.readSampleFrames(
            WritableSampleFrames(firstRange, SampleBuffer::WritableSlice(firstOutput)));
    ASSERT_EQ(firstRange, firstResult.frameIndexRange());
    const auto firstReadState = readState(source);
    EXPECT_EQ(requestedFrame + 2, firstReadState.currentFrameIndex);
    EXPECT_EQ(nextSampleFrame, firstReadState.streamGapEndFrameIndex);
    EXPECT_EQ(2, readSamples.callCount());
    EXPECT_EQ(0, firstOutput[0]);
    EXPECT_EQ(0, firstOutput[1]);

    SampleBuffer secondOutput(source.getSignalInfo().frames2samples(kGapLength));
    const auto secondRange =
            IndexRange::forward(requestedFrame + 2, kGapLength);
    const auto secondResult = source.readSampleFrames(
            WritableSampleFrames(secondRange, SampleBuffer::WritableSlice(secondOutput)));
    ASSERT_EQ(secondRange, secondResult.frameIndexRange());
    const auto secondReadState = readState(source);
    EXPECT_EQ(nextSampleFrame + 2, secondReadState.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, secondReadState.streamGapEndFrameIndex);
    EXPECT_EQ(source.getSignalInfo().frames2samples(2), readableSampleCount(source));
    EXPECT_EQ(0, secondOutput[0]);
    EXPECT_EQ(0, secondOutput[1]);
    EXPECT_EQ(0, secondOutput[2]);
    EXPECT_EQ(0, secondOutput[3]);
    EXPECT_EQ(41, secondOutput[4]);
    EXPECT_EQ(42, secondOutput[5]);
    EXPECT_EQ(2, readSamples.callCount());
}

TEST_F(SoundSourceMediaFoundationTest, FirstPostSeekTickAfterTargetFailsClosed) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));

    constexpr SINT kPrefetchFrames = 2112;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, requestedFrame + 4, 0});
    readSamples.push({S_OK, 0, requestedFrame + 8, kPrefetchFrames});
    SINT currentBeforeFirstSample = source.frameIndexMax();
    setReadSampleProvider(
            source,
            [this, &source, &readSamples, &currentBeforeFirstSample](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                const HRESULT hr = readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
                if (readSamples.callCount() == 2) {
                    currentBeforeFirstSample = readState(source).currentFrameIndex;
                }
                return hr;
            });
    SINT positionedAt = kUnknownFrameIndex;
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto state = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(kUnknownFrameIndex, currentBeforeFirstSample);
    EXPECT_EQ(source.frameIndexMax(), state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(2, readSamples.callCount());
}

TEST_F(SoundSourceMediaFoundationTest, InvalidPostSeekTickCannotBracketTarget) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));

    constexpr SINT kPrefetchFrames = 2112;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    const SINT invalidTickFrame = source.frameIndexMin() - 4;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, invalidTickFrame, 0});
    readSamples.push({S_OK, 0, requestedFrame + 8, kPrefetchFrames});
    SINT currentBeforeFirstSample = source.frameIndexMax();
    setReadSampleProvider(
            source,
            [this, &source, &readSamples, &currentBeforeFirstSample](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                const HRESULT hr = readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
                if (readSamples.callCount() == 2) {
                    currentBeforeFirstSample = readState(source).currentFrameIndex;
                }
                return hr;
            });
    SINT positionedAt = kUnknownFrameIndex;
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto state = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(kUnknownFrameIndex, currentBeforeFirstSample);
    EXPECT_EQ(source.frameIndexMax(), state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(0, readableSampleCount(source));
    EXPECT_EQ(2, readSamples.callCount());
}

TEST_F(SoundSourceMediaFoundationTest, InvalidPostSeekSampleTimestampFailsClosed) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));

    constexpr SINT kPrefetchFrames = 2112;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    const SINT invalidSampleFrame = source.frameIndexMax() + 4;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, requestedFrame - 4, 0});
    readSamples.push({S_OK, 0, invalidSampleFrame, 4});
    SINT currentBeforeFirstSample = source.frameIndexMax();
    setReadSampleProvider(
            source,
            [this, &source, &readSamples, &currentBeforeFirstSample](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                const HRESULT hr = readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
                if (readSamples.callCount() == 2) {
                    currentBeforeFirstSample = readState(source).currentFrameIndex;
                }
                return hr;
            });
    SINT positionedAt = kUnknownFrameIndex;
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto state = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(kUnknownFrameIndex, currentBeforeFirstSample);
    EXPECT_EQ(source.frameIndexMax(), state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(0, readableSampleCount(source));
    EXPECT_EQ(2, readSamples.callCount());
}

TEST_F(SoundSourceMediaFoundationTest, FirstPostSeekTickAtTargetPreservesGap) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());

    constexpr SINT kPrefetchFrames = 2112;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    const SINT nextSampleFrame = requestedFrame + 2;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, requestedFrame, 0});
    readSamples.push({S_OK, 0, nextSampleFrame, 4, 61});
    SINT positionedAt = kUnknownFrameIndex;
    setReadSampleProvider(
            source,
            [&readSamples](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                return readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
            });
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto state = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(requestedFrame, state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(nextSampleFrame, state.streamGapEndFrameIndex);
    EXPECT_EQ(source.getSignalInfo().frames2samples(4), readableSampleCount(source));
    EXPECT_EQ(2, readSamples.callCount());

    SampleBuffer output(source.getSignalInfo().frames2samples(4));
    const auto requestedRange = IndexRange::forward(requestedFrame, 4);
    const auto result = source.readSampleFrames(
            WritableSampleFrames(requestedRange, SampleBuffer::WritableSlice(output)));
    ASSERT_EQ(requestedRange, result.frameIndexRange());
    EXPECT_EQ(0, output[0]);
    EXPECT_EQ(0, output[1]);
    EXPECT_EQ(61, output[2]);
    EXPECT_EQ(62, output[3]);
    EXPECT_EQ(kUnknownFrameIndex, readState(source).streamGapEndFrameIndex);
    EXPECT_EQ(2, readSamples.callCount());
}

TEST_F(
        SoundSourceMediaFoundationTest,
        FirstPostSeekSampleAtTargetPreservesAudio) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());

    constexpr SINT kPrefetchFrames = 2112;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, requestedFrame - 4, 0});
    readSamples.push({S_OK, 0, requestedFrame, 4, 71});
    SINT currentBeforeFirstSample = source.frameIndexMax();
    setReadSampleProvider(
            source,
            [this, &source, &readSamples, &currentBeforeFirstSample](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                const HRESULT hr = readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
                if (readSamples.callCount() == 2) {
                    currentBeforeFirstSample = readState(source).currentFrameIndex;
                }
                return hr;
            });
    SINT positionedAt = kUnknownFrameIndex;
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto state = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(kUnknownFrameIndex, currentBeforeFirstSample);
    EXPECT_EQ(requestedFrame, state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(source.getSignalInfo().frames2samples(4), readableSampleCount(source));
    EXPECT_EQ(2, readSamples.callCount());

    SampleBuffer output(source.getSignalInfo().frames2samples(2));
    const auto requestedRange = IndexRange::forward(requestedFrame, 2);
    const auto result = source.readSampleFrames(
            WritableSampleFrames(requestedRange, SampleBuffer::WritableSlice(output)));
    ASSERT_EQ(requestedRange, result.frameIndexRange());
    EXPECT_EQ(71, output[0]);
    EXPECT_EQ(72, output[1]);
    EXPECT_EQ(requestedFrame + 2, readState(source).currentFrameIndex);
    EXPECT_EQ(source.getSignalInfo().frames2samples(2), readableSampleCount(source));
    EXPECT_EQ(2, readSamples.callCount());
}

TEST_F(SoundSourceMediaFoundationTest, SeekGapEndingAtTargetDoesNotSilenceTarget) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());

    constexpr SINT kPrefetchFrames = 2112;
    const SINT requestedFrame =
            source.frameIndexMin() + 2 * kPrefetchFrames + 100;
    const SINT seekFrame = requestedFrame - kPrefetchFrames;
    setReadState(
            source,
            source.frameIndexMax(),
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, 0, seekFrame - 8, kPrefetchFrames});
    readSamples.push(
            {S_OK, MF_SOURCE_READERF_STREAMTICK, requestedFrame - 2, 0});
    readSamples.push({S_OK, 0, requestedFrame, 4, 71});
    SINT positionedAt = kUnknownFrameIndex;
    setReadSampleProvider(
            source,
            [&readSamples](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                return readSamples(
                        streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
            });
    setCurrentPositionProvider(
            source,
            [this, &source, &positionedAt](LONGLONG streamPosition) {
                positionedAt = streamPositionToFrameIndexForTest(source, streamPosition);
                return S_OK;
            });

    seekTo(source, requestedFrame);
    const auto seekState = readState(source);
    EXPECT_EQ(seekFrame, positionedAt);
    EXPECT_EQ(requestedFrame, seekState.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, seekState.streamGapEndFrameIndex);
    EXPECT_EQ(3, readSamples.callCount());

    SampleBuffer output(source.getSignalInfo().frames2samples(2));
    const auto requestedRange = IndexRange::forward(requestedFrame, 2);
    const auto result = source.readSampleFrames(
            WritableSampleFrames(requestedRange, SampleBuffer::WritableSlice(output)));
    ASSERT_EQ(requestedRange, result.frameIndexRange());
    EXPECT_EQ(71, output[0]);
    EXPECT_EQ(72, output[1]);
    EXPECT_EQ(kUnknownFrameIndex, readState(source).streamGapEndFrameIndex);
}

TEST_F(SoundSourceMediaFoundationTest, EarlierTickFillsFromKnownRequestedFrame) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));

    const SINT currentFrame = source.frameIndexMin() + 20;
    setReadState(source, currentFrame, kUnknownFrameIndex, kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, currentFrame - 4, 0});
    readSamples.push({S_OK, 0, currentFrame + 4, 4});
    setReadSampleProvider(
            source,
            [&readSamples](DWORD streamIndex, DWORD controlFlags, DWORD* pFlags, LONGLONG* pTimestamp, IMFSample** ppSample) {
                return readSamples(streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
            });

    SampleBuffer output(source.getSignalInfo().frames2samples(2));
    const auto requestedRange = IndexRange::forward(currentFrame, 2);
    const auto result = source.readSampleFramesClamped(
            WritableSampleFrames(requestedRange, SampleBuffer::WritableSlice(output)));
    EXPECT_EQ(requestedRange, result.frameIndexRange());
    const auto state = readState(source);
    EXPECT_EQ(currentFrame + 2, state.currentFrameIndex);
    EXPECT_EQ(currentFrame + 4, state.streamGapEndFrameIndex);
    EXPECT_EQ(2, readSamples.callCount());
    EXPECT_EQ(0, output[0]);
    EXPECT_EQ(0, output[1]);
}

TEST_F(SoundSourceMediaFoundationTest, SeekClearsPendingStreamGap) {
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));

    const SINT targetFrame = source.frameIndexMin() + 20;
    setReadState(
            source,
            source.frameIndexMax(),
            targetFrame - 4,
            targetFrame + 4);
    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, 0, targetFrame, 4});
    setReadSampleProvider(
            source,
            [&readSamples](
                    DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                return readSamples(
                        streamIndex,
                        controlFlags,
                        pFlags,
                        pTimestamp,
                        ppSample);
            });
    setCurrentPositionProvider(
            source,
            [](LONGLONG) {
                return S_OK;
            });
    seekTo(source, targetFrame);

    const auto state = readState(source);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(1, readSamples.callCount());
}

TEST_F(SoundSourceMediaFoundationTest, RecordsRealSourceReaderPostSeekEvents) {
    constexpr SINT kInitialDecodeFrames = 1024;
    constexpr SINT kMaxPostSeekDecodeFrames = 65536;
    constexpr std::size_t kMaxCapturedEvents = 128;
    constexpr std::size_t kMaxCapturedPositions = 4;
    constexpr DWORD kFlagsHandledBeforeStreamTick =
            MF_SOURCE_READERF_ERROR |
            MF_SOURCE_READERF_ENDOFSTREAM |
            MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED;
    constexpr char kExpectedFixtureSha256[] =
            "9ab2af723411f398001677c4244bf92a280b76f4f0985e1fa7cb7a8b73a44e24";

    // The workflow sets these after validating the checked-out source SHA and
    // tree. GITHUB_SHA is recorded separately because it identifies the
    // workflow dispatch ref, which can differ from the tested source commit.
    const QByteArray sourceCommitPin = qgetenv("MIXXX_TEST_SOURCE_SHA");
    const QByteArray sourceTreePin = qgetenv("MIXXX_TEST_SOURCE_TREE");
    const bool sourcePinProvided =
            !sourceCommitPin.isEmpty() || !sourceTreePin.isEmpty();
    const bool sourcePinsFormatValid =
            sourceCommitPin.size() == 40 &&
            sourceCommitPin == QByteArray::fromHex(sourceCommitPin).toHex() &&
            sourceTreePin.size() == 40 &&
            sourceTreePin == QByteArray::fromHex(sourceTreePin).toHex();
    if (sourcePinProvided && !sourcePinsFormatValid) {
        RecordProperty("source_pin_status", "INVALID_SUPPLIED");
        FAIL() << "MIXXX_TEST_SOURCE_SHA and MIXXX_TEST_SOURCE_TREE must be "
                  "supplied together as lowercase 40-character Git object IDs";
    }

    // Ordinary Windows CTest runs do not provide the diagnostic source pins.
    // Leave identity unproven there instead of using GITHUB_SHA, which names
    // the workflow dispatch ref. Valid supplied pins are format-checked here;
    // the diagnostic workflow must bind them to its verified source receipt.
    RecordProperty(
            "source_pin_status",
            sourcePinsFormatValid ? "PRESENT_FORMAT_VALID_UNVERIFIED"
                                  : "UNPROVEN_ABSENT");
    if (sourcePinsFormatValid) {
        RecordProperty("source_commit", sourceCommitPin.toStdString());
        RecordProperty("source_tree", sourceTreePin.toStdString());
    }

    const QUrl fixtureUrl = timelineGapTestUrl();
    QFile fixtureFile(fixtureUrl.toLocalFile());
    ASSERT_TRUE(fixtureFile.open(QIODevice::ReadOnly));
    const QByteArray fixtureBytes = fixtureFile.readAll();
    ASSERT_EQ(
            QByteArray(kExpectedFixtureSha256),
            QCryptographicHash::hash(fixtureBytes, QCryptographicHash::Sha256).toHex());

    TestSoundSourceMediaFoundation source(fixtureUrl);
    ASSERT_EQ(
            AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    expectRealReadSamplePath(source);

    // Confirm the actual reader can decode this fixture normally before the
    // optional producer observation. A missing STREAMTICK must not fail this
    // test or be reported as a producer success.
    SampleBuffer initialBuffer(
            source.getSignalInfo().frames2samples(kInitialDecodeFrames));
    const auto initialRange = IndexRange::forward(
            source.frameIndexMin(), kInitialDecodeFrames);
    const auto initialResult = source.readSampleFrames(
            WritableSampleFrames(
                    initialRange,
                    SampleBuffer::WritableSlice(initialBuffer)));
    ASSERT_EQ(initialRange, initialResult.frameIndexRange());

    std::array<RawReadSampleObservation, kMaxCapturedEvents> events{};
    std::size_t eventCount = 0;
    bool traceTruncated = false;
    std::array<SetCurrentPositionObservation, kMaxCapturedPositions>
            positionObservations{};
    std::size_t positionCount = 0;
    bool positionTraceTruncated = false;
    ReadSamplePhase phase = ReadSamplePhase::SeekPrefetch;
    setCurrentPositionObserver(
            source,
            [&positionObservations, &positionCount, &positionTraceTruncated](
                    LONGLONG requestedPosition,
                    HRESULT hr) {
                if (positionCount == positionObservations.size()) {
                    positionTraceTruncated = true;
                    return;
                }
                positionObservations[positionCount++] = {requestedPosition, hr};
            });
    setReadSampleObserver(
            source,
            [&events, &eventCount, &traceTruncated, &phase](
                    HRESULT hr,
                    DWORD flags,
                    LONGLONG timestamp,
                    bool hasSample) {
                if (eventCount == events.size()) {
                    traceTruncated = true;
                    return;
                }
                events[eventCount++] = {phase, hr, flags, timestamp, hasSample};
            });

    const SINT requestedTargetFrame = source.frameIndexMin() +
            3 * source.getSignalInfo().getSampleRate() / 2;
    const LONGLONG requestedTarget100ns =
            frameIndexToStreamPositionForTest(source, requestedTargetFrame);
    const ReadState preSeekState = readState(source);
    seekTo(source, requestedTargetFrame);
    const ReadState postSeekState = readState(source);
    clearCurrentPositionObserver(source);
    const bool onePositionCall =
            positionCount == 1 && !positionTraceTruncated;
    const bool nativePositionSucceeded =
            onePositionCall && SUCCEEDED(positionObservations[0].hr);
    const bool seekAligned =
            postSeekState.currentFrameIndex == requestedTargetFrame;
    const bool seekReady = nativePositionSucceeded && seekAligned;

    bool afterSeekReadAttempted = false;
    SINT afterSeekRequestedFrames = 0;
    SINT afterSeekReturnedFrames = 0;
    if (seekReady) {
        const SINT gapFrames =
                postSeekState.streamGapEndFrameIndex > requestedTargetFrame
                ? postSeekState.streamGapEndFrameIndex - requestedTargetFrame
                : 0;
        const SINT bufferedFrames = readableFrameCount(source);
        const long long desiredFrames = std::max<long long>(
                kInitialDecodeFrames,
                static_cast<long long>(gapFrames) + bufferedFrames +
                        kInitialDecodeFrames);
        afterSeekRequestedFrames = static_cast<SINT>(std::min<long long>(
                kMaxPostSeekDecodeFrames,
                desiredFrames));
        SampleBuffer afterSeekBuffer(
                source.getSignalInfo().frames2samples(afterSeekRequestedFrames));
        const auto afterSeekRange = IndexRange::forward(
                requestedTargetFrame,
                afterSeekRequestedFrames);
        phase = ReadSamplePhase::AfterSeekReturn;
        const auto afterSeekResult = source.readSampleFrames(
                WritableSampleFrames(
                        afterSeekRange,
                        SampleBuffer::WritableSlice(afterSeekBuffer)));
        afterSeekReadAttempted = true;
        afterSeekReturnedFrames = afterSeekResult.frameIndexRange().length();
    }

    std::ostringstream rawTrace;
    std::size_t seekPrefetchEventCount = 0;
    std::size_t afterSeekEventCount = 0;
    bool rawStreamTickSeen = false;
    bool afterSeekStreamTickSeen = false;
    for (std::size_t index = 0; index < eventCount; ++index) {
        const auto& event = events[index];
        if (index != 0) {
            rawTrace << ';';
        }
        rawTrace << index << ":phase=" << readSamplePhaseName(event.phase)
                 << ",hr=0x" << std::hex
                 << static_cast<unsigned long>(event.hr)
                 << ",flags=0x" << static_cast<unsigned long>(event.flags)
                 << std::dec << ",timestamp_100ns=" << event.timestamp
                 << ",sample=" << (event.hasSample ? 1 : 0);
        if (event.phase == ReadSamplePhase::SeekPrefetch) {
            ++seekPrefetchEventCount;
        } else if (event.phase == ReadSamplePhase::AfterSeekReturn) {
            ++afterSeekEventCount;
        }
        rawStreamTickSeen = rawStreamTickSeen ||
                (event.flags & MF_SOURCE_READERF_STREAMTICK) != 0;
        if (event.phase == ReadSamplePhase::AfterSeekReturn) {
            afterSeekStreamTickSeen = afterSeekStreamTickSeen ||
                    (event.flags & MF_SOURCE_READERF_STREAMTICK) != 0;
        }
    }

    std::ostringstream positionTrace;
    for (std::size_t index = 0; index < positionCount; ++index) {
        const auto& observation = positionObservations[index];
        if (index != 0) {
            positionTrace << ';';
        }
        positionTrace << index << ":position_100ns="
                      << observation.requestedPosition << ",hr=0x"
                      << std::hex << std::setw(8) << std::setfill('0')
                      << static_cast<unsigned long>(observation.hr)
                      << std::dec;
    }

    bool usableStreamTickSeen = false;
    bool targetBracketObserved = false;
    SINT pendingTickEventIndex = -1;
    SINT tickEventIndex = -1;
    SINT followingSampleEventIndex = -1;
    if (seekReady) {
        for (std::size_t index = 0; index < eventCount; ++index) {
            const auto& event = events[index];
            if (event.phase != ReadSamplePhase::AfterSeekReturn) {
                continue;
            }
            const SINT eventIndex = static_cast<SINT>(index);
            const bool hasStreamTick =
                    (event.flags & MF_SOURCE_READERF_STREAMTICK) != 0;

            // Match the production ReadSample branch order: failed reads and
            // higher-priority flags stop processing before a stream tick is used.
            if (FAILED(event.hr) ||
                    (event.flags & kFlagsHandledBeforeStreamTick) != 0) {
                break;
            }

            if (hasStreamTick) {
                const SINT tickFrame =
                        streamPositionToFrameIndexForTest(source, event.timestamp);
                if (tickFrame != kUnknownFrameIndex &&
                        source.isValidFrameIndex(tickFrame)) {
                    pendingTickEventIndex = eventIndex;
                    usableStreamTickSeen = true;
                } else {
                    // Production clears its pending tick on an invalid timestamp.
                    pendingTickEventIndex = -1;
                }
                // Production releases a sample returned with STREAMTICK and
                // continues; that sample cannot be the following decoded sample.
                continue;
            }

            if (!event.hasSample) {
                // Production stops when a non-tick result has no sample.
                break;
            }

            const SINT sampleFrame =
                    streamPositionToFrameIndexForTest(source, event.timestamp);
            if (sampleFrame == kUnknownFrameIndex ||
                    !source.isValidFrameIndex(sampleFrame)) {
                // Production aborts after a sample with an invalid timestamp.
                break;
            }

            if (pendingTickEventIndex >= 0) {
                const auto& tick = events[static_cast<std::size_t>(pendingTickEventIndex)];
                const SINT tickFrame = streamPositionToFrameIndexForTest(
                        source,
                        tick.timestamp);
                if (tickEventIndex < 0) {
                    tickEventIndex = pendingTickEventIndex;
                    followingSampleEventIndex = eventIndex;
                }
                if (tickFrame <= requestedTargetFrame &&
                        requestedTargetFrame < sampleFrame) {
                    tickEventIndex = pendingTickEventIndex;
                    followingSampleEventIndex = eventIndex;
                    targetBracketObserved = true;
                    break;
                }
            }

            // Production consumes the pending tick when it processes a sample.
            pendingTickEventIndex = -1;
        }
        if (tickEventIndex < 0 && pendingTickEventIndex >= 0) {
            tickEventIndex = pendingTickEventIndex;
        }
    }

    RecordProperty(
            "workflow_dispatch_sha",
            qEnvironmentVariable("GITHUB_SHA").toStdString());
    RecordProperty(
            "workflow_commit",
            qEnvironmentVariable("GITHUB_WORKFLOW_SHA").toStdString());
    RecordProperty("fixture_sha256", kExpectedFixtureSha256);
    RecordProperty("raw_read_sample_trace", rawTrace.str());
    RecordProperty("trace_event_count", std::to_string(eventCount));
    RecordProperty("trace_truncated", traceTruncated ? "true" : "false");
    RecordProperty("seek_prefetch_event_count", std::to_string(seekPrefetchEventCount));
    RecordProperty("after_seek_return_event_count", std::to_string(afterSeekEventCount));
    RecordProperty("pre_seek_cursor_frame", std::to_string(preSeekState.currentFrameIndex));
    RecordProperty("post_seek_cursor_frame", std::to_string(postSeekState.currentFrameIndex));
    RecordProperty("requested_target_frame", std::to_string(requestedTargetFrame));
    RecordProperty("requested_target_100ns", std::to_string(requestedTarget100ns));
    RecordProperty("set_current_position_call_count", std::to_string(positionCount));
    RecordProperty("set_current_position_trace", positionTrace.str());
    RecordProperty(
            "set_current_position_trace_truncated",
            positionTraceTruncated ? "true" : "false");
    RecordProperty(
            "set_current_position_succeeded",
            nativePositionSucceeded ? "true" : "false");
    RecordProperty("seek_aligned", seekAligned ? "true" : "false");
    RecordProperty("seek_result", seekReady ? "ALIGNED" : "UNPROVEN_SEEK_FAILED");
    RecordProperty(
            "after_seek_read_attempted",
            afterSeekReadAttempted ? "true" : "false");
    RecordProperty(
            "after_seek_requested_frames",
            std::to_string(afterSeekRequestedFrames));
    RecordProperty(
            "after_seek_returned_frames",
            std::to_string(afterSeekReturnedFrames));
    RecordProperty("raw_streamtick_seen", rawStreamTickSeen ? "true" : "false");
    RecordProperty(
            "after_seek_streamtick_seen",
            afterSeekStreamTickSeen ? "true" : "false");

    std::string producerResult;
    if (!seekReady) {
        producerResult = "UNPROVEN_SEEK_FAILED";
    } else if (traceTruncated) {
        producerResult = "UNPROVEN_TRACE_TRUNCATED";
    } else if (targetBracketObserved) {
        producerResult = "OBSERVED_TICK_SAMPLE_BRACKETING_TARGET";
    } else if (!afterSeekStreamTickSeen) {
        producerResult = "UNPROVEN_NO_STREAMTICK";
    } else if (!usableStreamTickSeen) {
        producerResult = "UNPROVEN_NO_USABLE_STREAMTICK";
    } else if (followingSampleEventIndex < 0) {
        producerResult = "UNPROVEN_NO_FOLLOWING_SAMPLE";
    } else {
        producerResult = "UNPROVEN_TARGET_NOT_BRACKETED";
    }

    if (tickEventIndex >= 0) {
        const auto& tick = events[static_cast<std::size_t>(tickEventIndex)];
        const SINT tickFrame =
                streamPositionToFrameIndexForTest(source, tick.timestamp);
        RecordProperty("streamtick_flags", std::to_string(tick.flags));
        RecordProperty("streamtick_timestamp_100ns", std::to_string(tick.timestamp));
        RecordProperty("streamtick_frame", std::to_string(tickFrame));
        RecordProperty(
                "streamtick_phase",
                readSamplePhaseName(tick.phase));
        if (followingSampleEventIndex >= 0) {
            const auto& sample = events[static_cast<std::size_t>(followingSampleEventIndex)];
            const SINT sampleFrame =
                    streamPositionToFrameIndexForTest(source, sample.timestamp);
            RecordProperty(
                    "following_sample_flags",
                    std::to_string(sample.flags));
            RecordProperty(
                    "following_sample_timestamp_100ns",
                    std::to_string(sample.timestamp));
            RecordProperty("following_sample_frame", std::to_string(sampleFrame));
            RecordProperty(
                    "following_sample_phase",
                    readSamplePhaseName(sample.phase));
        }
    }
    RecordProperty(
            "producer_proof",
            targetBracketObserved && !traceTruncated && seekReady ? "true" : "false");
    RecordProperty("producer_result", producerResult);
}
} // namespace mixxx
