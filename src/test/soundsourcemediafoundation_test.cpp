#include "sources/soundsourcemediafoundation.h"

#include <gtest/gtest.h>
#include <mfapi.h>

#include <cmath>
#include <deque>
#include <iomanip>
#include <tuple>
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

IMFSample* createSampleWithTwoBuffers(
        const audio::SignalInfo& signalInfo,
        SINT frameCount,
        CSAMPLE firstSampleValue) {
    const SINT firstFrameCount = frameCount / 2;
    IMFSample* pSample = createSample(signalInfo, firstFrameCount, firstSampleValue);
    IMFSample* pSecondSample = createSample(signalInfo,
            frameCount - firstFrameCount,
            firstSampleValue + static_cast<CSAMPLE>(signalInfo.frames2samples(firstFrameCount)));
    if (!pSample || !pSecondSample) {
        if (pSample) {
            pSample->Release();
        }
        if (pSecondSample) {
            pSecondSample->Release();
        }
        return nullptr;
    }
    IMFMediaBuffer* pSecondBuffer = nullptr;
    HRESULT hr = pSecondSample->GetBufferByIndex(0, &pSecondBuffer);
    if (SUCCEEDED(hr)) {
        hr = pSample->AddBuffer(pSecondBuffer);
        pSecondBuffer->Release();
    }
    pSecondSample->Release();
    if (FAILED(hr)) {
        ADD_FAILURE() << "Creating a two-buffer sample failed: " << std::hex << hr;
        pSample->Release();
        return nullptr;
    }
    return pSample;
}

enum class SampleReadFailure {
    BufferCount,
    TotalLength,
    FirstBuffer,
    LaterBuffer,
    FirstLock,
    LaterLock,
};

struct ReadSampleEvent {
    ReadSampleEvent(
            HRESULT hrIn,
            DWORD flagsIn,
            SINT frameIndexIn,
            SINT frameCountIn,
            CSAMPLE firstSampleValueIn = 1,
            bool twoBuffersIn = false)
            : hr(hrIn),
              flags(flagsIn),
              frameIndex(frameIndexIn),
              frameCount(frameCountIn),
              firstSampleValue(firstSampleValueIn),
              twoBuffers(twoBuffersIn) {
    }

    HRESULT hr;
    DWORD flags;
    SINT frameIndex;
    SINT frameCount;
    CSAMPLE firstSampleValue;
    bool twoBuffers;
};

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
            *ppSample = event.twoBuffers
                    ? createSampleWithTwoBuffers(m_signalInfo,
                              event.frameCount,
                              event.firstSampleValue)
                    : createSample(m_signalInfo,
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

    template<typename Provider>
    void setReadSampleProvider(
            SoundSourceMediaFoundation& source,
            Provider&& provider) {
        source.m_readSampleProvider = std::forward<Provider>(provider);
    }

    template<typename Provider>
    void setCurrentPositionProvider(
            SoundSourceMediaFoundation& source,
            Provider&& provider) {
        source.m_setCurrentPositionProvider = std::forward<Provider>(provider);
    }

    SINT frameIndexFromStreamPosition(
            const SoundSourceMediaFoundation& source,
            LONGLONG streamPosition) const {
        return source.m_streamUnitConverter.toFrameIndex(streamPosition);
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

    SINT readableSampleCount(const SoundSourceMediaFoundation& source) const {
        return source.m_sampleBuffer.readableLength();
    }

    bool readerIsLive(const SoundSourceMediaFoundation& source) const {
        return source.m_pSourceReader != nullptr;
    }

    void injectSampleReadFailure(
            SoundSourceMediaFoundation& source,
            SampleReadFailure failure) {
        source.m_getBufferCountProvider = [failure](IMFSample* pSample, DWORD* pCount) {
            return failure == SampleReadFailure::BufferCount
                    ? E_FAIL
                    : pSample->GetBufferCount(pCount);
        };
        source.m_getTotalLengthProvider = [failure](IMFSample* pSample, DWORD* pLength) {
            return failure == SampleReadFailure::TotalLength
                    ? E_FAIL
                    : pSample->GetTotalLength(pLength);
        };
        source.m_getBufferByIndexProvider = [failure](IMFSample* pSample,
                                                    DWORD index,
                                                    IMFMediaBuffer** ppBuffer) {
            if ((failure == SampleReadFailure::FirstBuffer && index == 0) ||
                    (failure == SampleReadFailure::LaterBuffer && index == 1)) {
                *ppBuffer = nullptr;
                return E_FAIL;
            }
            return pSample->GetBufferByIndex(index, ppBuffer);
        };
        source.m_lockBufferProvider = [failure, callCount = 0](
                                              IMFMediaBuffer* pBuffer,
                                              BYTE** ppData,
                                              DWORD* pLength) mutable {
            const int index = callCount++;
            if ((failure == SampleReadFailure::FirstLock && index == 0) ||
                    (failure == SampleReadFailure::LaterLock && index == 1)) {
                *ppData = nullptr;
                *pLength = 0;
                return E_FAIL;
            }
            return pBuffer->Lock(ppData, nullptr, pLength);
        };
    }

    void clearSampleReadFailure(SoundSourceMediaFoundation& source) {
        source.m_getBufferCountProvider = {};
        source.m_getTotalLengthProvider = {};
        source.m_getBufferByIndexProvider = {};
        source.m_lockBufferProvider = {};
    }

    void expectPublicReadFailureShrinksRange(bool partialOutput) {
        TestSoundSourceMediaFoundation source(testUrl());
        ASSERT_EQ(AudioSource::OpenResult::Succeeded,
                source.open(AudioSource::OpenMode::Strict));
        ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());
        const auto originalFrameRange = source.frameIndexRange();
        const SINT requestedFrame = source.frameIndexMin() + 10000;
        const SINT unreadableFrame = requestedFrame + (partialOutput ? 2 : 0);
        setReadState(source,
                partialOutput ? requestedFrame : originalFrameRange.end(),
                kUnknownFrameIndex,
                kUnknownFrameIndex);
        ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
        readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, requestedFrame - 1000, 0});
        readSamples.push({S_OK, 0, requestedFrame + 2, 4, 41});
        setReadSampleProvider(source,
                [&readSamples](DWORD streamIndex,
                        DWORD controlFlags,
                        DWORD* pFlags,
                        LONGLONG* pTimestamp,
                        IMFSample** ppSample) {
                    return readSamples(streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
                });
        int positionCalls = 0;
        setCurrentPositionProvider(source,
                [this, &source, &positionCalls, requestedFrame](LONGLONG streamPosition) {
                    ++positionCalls;
                    EXPECT_EQ(requestedFrame - 2112,
                            frameIndexFromStreamPosition(source, streamPosition));
                    return S_OK;
                });
        injectSampleReadFailure(source, SampleReadFailure::TotalLength);
        SampleBuffer output(4);
        for (SINT i = 0; i < 4; ++i) {
            output[i] = -777;
        }
        const auto failedResult = source.readSampleFrames(WritableSampleFrames(
                IndexRange::forward(requestedFrame, 4), SampleBuffer::WritableSlice(output)));
        if (partialOutput) {
            EXPECT_EQ(IndexRange::forward(requestedFrame, 2), failedResult.frameIndexRange());
            EXPECT_EQ(0, output[0]);
            EXPECT_EQ(0, output[1]);
        } else {
            EXPECT_TRUE(failedResult.frameIndexRange().empty());
            EXPECT_EQ(-777, output[0]);
            EXPECT_EQ(-777, output[1]);
        }
        EXPECT_EQ(-777, output[2]);
        EXPECT_EQ(-777, output[3]);
        EXPECT_EQ(IndexRange::between(originalFrameRange.start(), unreadableFrame),
                source.frameIndexRange());
        // The decoder's old abort sentinel is distinct from the shrunken bound.
        EXPECT_EQ(originalFrameRange.end(), readState(source).currentFrameIndex);
        EXPECT_EQ(kUnknownFrameIndex, readState(source).streamTickFrameIndex);
        EXPECT_EQ(kUnknownFrameIndex, readState(source).streamGapEndFrameIndex);
        EXPECT_EQ(0, readableSampleCount(source));
        EXPECT_TRUE(readerIsLive(source));
        EXPECT_EQ(partialOutput ? 0 : 1, positionCalls);
        EXPECT_EQ(2, readSamples.callCount());

        clearSampleReadFailure(source);
        const auto retryResult = source.readSampleFrames(WritableSampleFrames(
                IndexRange::forward(unreadableFrame, 4), SampleBuffer::WritableSlice(output)));
        EXPECT_TRUE(retryResult.frameIndexRange().empty());
        EXPECT_EQ(unreadableFrame, source.frameIndexMax());
        EXPECT_EQ(partialOutput ? 0 : 1, positionCalls);
        EXPECT_EQ(2, readSamples.callCount());
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
                positionedAt = frameIndexFromStreamPosition(source, streamPosition);
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
    setReadSampleProvider(source,
            [&readSamples](DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
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
    seekTo(source, targetFrame);

    const auto state = readState(source);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
}

TEST_F(SoundSourceMediaFoundationTest, PublicEmptyFailureMarksFollowingFramesUnreadable) {
    expectPublicReadFailureShrinksRange(false);
}

TEST_F(SoundSourceMediaFoundationTest, PublicPartialFailureMarksFollowingFramesUnreadable) {
    expectPublicReadFailureShrinksRange(true);
}

class SoundSourceMediaFoundationSampleFailureTest
        : public SoundSourceMediaFoundationTest,
          public ::testing::WithParamInterface<std::tuple<SampleReadFailure, bool>> {
};

TEST_P(SoundSourceMediaFoundationSampleFailureTest, FailedAnchorCannotCompleteSeek) {
    const auto [failure, exactTarget] = GetParam();
    const bool metadataFailure = failure == SampleReadFailure::BufferCount ||
            failure == SampleReadFailure::TotalLength;
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());

    const auto originalFrameRange = source.frameIndexRange();
    const SINT requestedFrame = source.frameIndexMin() + 10000;
    const SINT tickFrame = requestedFrame - 1000;
    const SINT failedSampleFrame = requestedFrame + (exactTarget ? 0 : 2);
    setReadState(source, source.frameIndexMax(), kUnknownFrameIndex, kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, tickFrame, 0});
    readSamples.push({S_OK, 0, failedSampleFrame, 4, 41, true});
    // After discarding A, B must keep its own timestamp, never A's position.
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, tickFrame, 0});
    readSamples.push({S_OK, 0, requestedFrame + 6, 4, 81});
    setReadSampleProvider(source,
            [&readSamples](DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                return readSamples(streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
            });
    int positionCalls = 0;
    setCurrentPositionProvider(source,
            [this, &source, &positionCalls, requestedFrame](LONGLONG streamPosition) {
                ++positionCalls;
                EXPECT_EQ(requestedFrame - 2112,
                        frameIndexFromStreamPosition(source, streamPosition));
                return S_OK;
            });
    injectSampleReadFailure(source, failure);

    SampleBuffer output(4);
    for (SINT i = 0; i < 4; ++i) {
        output[i] = -777;
    }
    const auto requestedRange = IndexRange::forward(requestedFrame, 4);
    const auto failedResult = source.readSampleFramesClamped(
            WritableSampleFrames(requestedRange, SampleBuffer::WritableSlice(output)));
    EXPECT_TRUE(failedResult.frameIndexRange().empty());
    EXPECT_EQ(1, positionCalls);
    EXPECT_EQ(2, readSamples.callCount());
    EXPECT_EQ(originalFrameRange, source.frameIndexRange());
    EXPECT_EQ(originalFrameRange.end(), readState(source).currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, readState(source).streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, readState(source).streamGapEndFrameIndex);
    EXPECT_EQ(0, readableSampleCount(source));
    EXPECT_EQ(metadataFailure, readerIsLive(source));
    for (SINT i = 0; i < 4; ++i) {
        EXPECT_EQ(-777, output[i]);
    }

    clearSampleReadFailure(source);
    const auto retryResult = source.readSampleFramesClamped(
            WritableSampleFrames(requestedRange, SampleBuffer::WritableSlice(output)));
    if (!metadataFailure) {
        // Buffer access failures keep the existing dead-reader policy.
        EXPECT_TRUE(retryResult.frameIndexRange().empty());
        EXPECT_EQ(1, positionCalls);
        EXPECT_EQ(2, readSamples.callCount());
        EXPECT_EQ(0, readableSampleCount(source));
        EXPECT_EQ(kUnknownFrameIndex, readState(source).streamGapEndFrameIndex);
        for (SINT i = 0; i < 4; ++i) {
            EXPECT_EQ(-777, output[i]);
        }
        return;
    }
    ASSERT_EQ(requestedRange, retryResult.frameIndexRange());
    EXPECT_EQ(2, positionCalls);
    EXPECT_EQ(4, readSamples.callCount());
    EXPECT_EQ(requestedFrame + 6, readState(source).streamGapEndFrameIndex);
    for (SINT i = 0; i < 4; ++i) {
        EXPECT_EQ(0, output[i]);
    }

    const auto nextRange = IndexRange::forward(requestedFrame + 4, 4);
    const auto nextResult = source.readSampleFramesClamped(
            WritableSampleFrames(nextRange, SampleBuffer::WritableSlice(output)));
    ASSERT_EQ(nextRange, nextResult.frameIndexRange());
    EXPECT_EQ(0, output[0]);
    EXPECT_EQ(0, output[1]);
    EXPECT_EQ(81, output[2]);
    EXPECT_EQ(82, output[3]);
    EXPECT_EQ(requestedFrame + 8, readState(source).currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, readState(source).streamGapEndFrameIndex);
    EXPECT_EQ(2, readableSampleCount(source));
    EXPECT_EQ(2, positionCalls);
    EXPECT_EQ(4, readSamples.callCount());
}

INSTANTIATE_TEST_SUITE_P(ExtractionFailures,
        SoundSourceMediaFoundationSampleFailureTest,
        ::testing::Combine(
                ::testing::Values(SampleReadFailure::BufferCount,
                        SampleReadFailure::TotalLength,
                        SampleReadFailure::FirstBuffer,
                        SampleReadFailure::LaterBuffer,
                        SampleReadFailure::FirstLock,
                        SampleReadFailure::LaterLock),
                ::testing::Bool()));

class SoundSourceMediaFoundationBufferedFailureTest
        : public SoundSourceMediaFoundationTest,
          public ::testing::WithParamInterface<SampleReadFailure> {
};

TEST_P(SoundSourceMediaFoundationBufferedFailureTest,
        PreserveOutputPrefixButDiscardBufferedSuffix) {
    const auto failure = GetParam();
    const bool metadataFailure = failure == SampleReadFailure::BufferCount ||
            failure == SampleReadFailure::TotalLength;
    const bool laterBufferFailure = failure == SampleReadFailure::LaterBuffer ||
            failure == SampleReadFailure::LaterLock;
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());
    const auto originalFrameRange = source.frameIndexRange();
    const SINT requestedFrame = source.frameIndexMin() + 10000;
    const SINT returnedFrames = laterBufferFailure ? 3 : 2;
    const SINT retryFrame = requestedFrame + returnedFrames;
    setReadState(source, requestedFrame, kUnknownFrameIndex, kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), source.frameIndexMin());
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, requestedFrame, 0});
    readSamples.push({S_OK, 0, requestedFrame + 2, 4, 41, true});
    readSamples.push({S_OK, 0, retryFrame, 4, 81});
    setReadSampleProvider(source,
            [&readSamples](DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                return readSamples(streamIndex, controlFlags, pFlags, pTimestamp, ppSample);
            });
    int positionCalls = 0;
    setCurrentPositionProvider(source,
            [this, &source, &positionCalls, retryFrame](LONGLONG streamPosition) {
                ++positionCalls;
                EXPECT_EQ(retryFrame - 2112,
                        frameIndexFromStreamPosition(source, streamPosition));
                return S_OK;
            });
    injectSampleReadFailure(source, failure);
    SampleBuffer output(3);
    for (SINT i = 0; i < 3; ++i) {
        output[i] = -777;
    }
    const auto failedResult = source.readSampleFramesClamped(WritableSampleFrames(
            IndexRange::forward(requestedFrame, 3), SampleBuffer::WritableSlice(output)));
    EXPECT_EQ(IndexRange::forward(requestedFrame, returnedFrames), failedResult.frameIndexRange());
    EXPECT_EQ(0, output[0]);
    EXPECT_EQ(0, output[1]);
    EXPECT_EQ(laterBufferFailure ? 41 : -777, output[2]);
    EXPECT_EQ(originalFrameRange, source.frameIndexRange());
    EXPECT_EQ(originalFrameRange.end(), readState(source).currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, readState(source).streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, readState(source).streamGapEndFrameIndex);
    EXPECT_EQ(0, readableSampleCount(source));
    EXPECT_EQ(metadataFailure, readerIsLive(source));
    EXPECT_EQ(0, positionCalls);
    EXPECT_EQ(2, readSamples.callCount());

    clearSampleReadFailure(source);
    const auto retryRange = IndexRange::forward(retryFrame, 3);
    const auto retryResult = source.readSampleFramesClamped(
            WritableSampleFrames(retryRange, SampleBuffer::WritableSlice(output)));
    if (metadataFailure) {
        ASSERT_EQ(retryRange, retryResult.frameIndexRange());
        EXPECT_EQ(1, positionCalls);
        EXPECT_EQ(3, readSamples.callCount());
        EXPECT_EQ(81, output[0]);
        EXPECT_EQ(82, output[1]);
        EXPECT_EQ(83, output[2]);
    } else {
        EXPECT_TRUE(retryResult.frameIndexRange().empty());
        EXPECT_EQ(0, positionCalls);
        EXPECT_EQ(2, readSamples.callCount());
        EXPECT_EQ(0, readableSampleCount(source));
    }
}

INSTANTIATE_TEST_SUITE_P(ExtractionFailures,
        SoundSourceMediaFoundationBufferedFailureTest,
        ::testing::Values(SampleReadFailure::BufferCount,
                SampleReadFailure::TotalLength,
                SampleReadFailure::FirstBuffer,
                SampleReadFailure::LaterBuffer,
                SampleReadFailure::FirstLock,
                SampleReadFailure::LaterLock));

} // namespace mixxx
