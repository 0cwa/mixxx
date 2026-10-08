#include "sources/soundsourcemediafoundation.h"

#include <gtest/gtest.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <utility>

#include "engine/cachingreader/cachingreaderchunk.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxtest.h"
#include "track/track.h"
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
        SINT frameCount) {
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
        pSamples[i] = static_cast<CSAMPLE>(i + 1);
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
    HRESULT hr;
    DWORD flags;
    SINT frameIndex;
    SINT frameCount;
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
            *ppSample = createSample(m_signalInfo, event.frameCount);
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

    ReadState readState(const SoundSourceMediaFoundation& source) const {
        return {
                source.m_currentFrameIndex,
                source.m_streamTickFrameIndex,
                source.m_streamGapEndFrameIndex};
    }

    SINT readableSampleCount(const SoundSourceMediaFoundation& source) const {
        return source.m_sampleBuffer.readableLength();
    }
};

// AI-generated Windows diagnostic test support begins.
class MediaFoundationProviderDiagnosticTest : public ::MixxxTest {
  protected:
    void SetUp() override {
        if (qEnvironmentVariableIntValue("MIXXX_HEADLESS_MF_DIAGNOSTICS") != 1) {
            GTEST_SKIP() << "Explicit headless Media Foundation diagnostic opt-in required";
        }
    }
    QUrl testUrl() const {
        return QUrl::fromLocalFile(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    }

    SoundSourceProviderPointer registeredProvider() {
        if (!SoundSourceProxy::isFileSuffixSupported(QStringLiteral("wav"))) {
            if (!SoundSourceProxy::registerProviders()) {
                return {};
            }
        }
        for (const auto& registration :
                SoundSourceProxy::allProviderRegistrationsForUrl(testUrl())) {
            auto provider = registration.getProvider();
            if (std::dynamic_pointer_cast<SoundSourceProviderMediaFoundation>(provider)) {
                return provider;
            }
        }
        return {};
    }

    void checkSeekReads(const AudioSourcePointer& source) {
        constexpr SINT kFrames = 64;
        const auto channels = source->getSignalInfo().getChannelCount();
        ASSERT_GT(channels.value(), 0);
        ASSERT_GT(source->frameIndexRange().length(), 5 * CachingReaderChunk::kFrames);
        SampleBuffer reference(source->getSignalInfo().frames2samples(kFrames));
        SampleBuffer actual(source->getSignalInfo().frames2samples(kFrames));
        const SINT first = source->frameIndexMin();
        const auto firstRange = IndexRange::forward(first, kFrames);
        ASSERT_EQ(firstRange,
                source->readSampleFrames(
                              WritableSampleFrames(firstRange,
                                      SampleBuffer::WritableSlice(reference)))
                        .frameIndexRange());
        const auto forwardRange = IndexRange::forward(
                first + 4 * CachingReaderChunk::kFrames, kFrames);
        ASSERT_EQ(forwardRange,
                source->readSampleFrames(
                              WritableSampleFrames(forwardRange,
                                      SampleBuffer::WritableSlice(actual)))
                        .frameIndexRange());
        ASSERT_EQ(firstRange,
                source->readSampleFrames(
                              WritableSampleFrames(firstRange,
                                      SampleBuffer::WritableSlice(actual)))
                        .frameIndexRange());
        for (SINT sample = 0; sample < reference.size(); ++sample) {
            ASSERT_NEAR(reference[sample], actual[sample], 0.0001f) << sample;
        }
        RecordProperty("provider", "Microsoft Media Foundation");
        RecordProperty("completed_seek_reads", "initial,forward,backward");
    }
};

TEST_F(MediaFoundationProviderDiagnosticTest, DirectSourceForwardBackwardReads) {
    auto source = std::make_shared<SoundSourceMediaFoundation>(testUrl());
    ASSERT_EQ(AudioSource::OpenResult::Succeeded,
            source->open(AudioSource::OpenMode::Strict));
    checkSeekReads(source);
}

TEST_F(MediaFoundationProviderDiagnosticTest, ForcedProxyForwardBackwardReads) {
    const auto provider = registeredProvider();
    ASSERT_TRUE(provider) << "Registered Media Foundation WAV provider is required";
    auto track = Track::newTemporary(testUrl().toLocalFile());
    SoundSourceProxy proxy(track, provider);
    const auto source = proxy.openAudioSource(AudioSource::OpenParams());
    ASSERT_TRUE(source) << "Media Foundation must open the local WAV fixture";
    ASSERT_EQ(provider, proxy.getProvider()) << "Provider fallback is forbidden";
    checkSeekReads(source);
}

TEST_F(MediaFoundationProviderDiagnosticTest, ForcedProxyCachingReaderChunkSeeks) {
    const auto provider = registeredProvider();
    ASSERT_TRUE(provider) << "Registered Media Foundation WAV provider is required";
    auto track = Track::newTemporary(testUrl().toLocalFile());
    SoundSourceProxy proxy(track, provider);
    const auto source = proxy.openAudioSource(AudioSource::OpenParams());
    ASSERT_TRUE(source) << "Media Foundation must open the local WAV fixture";
    ASSERT_EQ(provider, proxy.getProvider()) << "Provider fallback is forbidden";
    constexpr auto channels = audio::ChannelCount::stereo();
    const SINT sampleCount = CachingReaderChunk::frames2samples(
            CachingReaderChunk::kFrames, channels);
    SampleBuffer chunkSamples(sampleCount);
    SampleBuffer temporarySamples(sampleCount);
    SampleBuffer reference(sampleCount);
    SampleBuffer actual(sampleCount);
    CachingReaderChunkForOwner chunk{SampleBuffer::WritableSlice(chunkSamples)};
    ASSERT_GT(source->frameIndexRange().length(), 5 * CachingReaderChunk::kFrames);
    bool haveReference = false;
    for (SINT index : {0, 4, 0}) {
        chunk.init(index);
        const auto requested = chunk.frameIndexRange(source);
        ASSERT_EQ(requested,
                chunk.bufferSampleFrames(source, SampleBuffer::WritableSlice(temporarySamples)));
        ASSERT_EQ(requested, chunk.readBufferedSampleFrames(actual.data(), channels, requested));
        if (index == 0) {
            if (!haveReference) {
                std::copy(actual.data(), actual.data() + sampleCount, reference.data());
                haveReference = true;
            } else {
                for (SINT sample = 0; sample < sampleCount; ++sample) {
                    ASSERT_NEAR(reference[sample], actual[sample], 0.0001f) << sample;
                }
            }
        }
        chunk.free();
    }
    ASSERT_TRUE(haveReference);
    RecordProperty("provider", "Microsoft Media Foundation");
    RecordProperty("completed_chunk_indices", "0,4,0");
}
// End AI-generated Windows diagnostic test support.

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

// AI-generated public-caller late-tick diagnostic begins.
TEST_F(SoundSourceMediaFoundationTest,
        HeadlessLatePostSeekTickMustReachRequestedFrame) {
    if (qEnvironmentVariableIntValue("MIXXX_HEADLESS_MF_DIAGNOSTICS") != 1) {
        GTEST_SKIP() << "Explicit headless Media Foundation diagnostic opt-in required";
    }
    TestSoundSourceMediaFoundation source(testUrl());
    ASSERT_EQ(AudioSource::OpenResult::Succeeded,
            source.open(AudioSource::OpenMode::Strict));
    ASSERT_EQ(1, source.getSignalInfo().getChannelCount().value());

    const SINT firstFrameIndex = source.frameIndexMin();
    const SINT targetFrameIndex = firstFrameIndex + 4;
    ASSERT_LT(targetFrameIndex + 32, source.frameIndexMax());
    setReadState(source,
            targetFrameIndex + 1,
            kUnknownFrameIndex,
            kUnknownFrameIndex);

    ReadSampleQueue readSamples(source.getSignalInfo(), firstFrameIndex);
    readSamples.push({S_OK, MF_SOURCE_READERF_STREAMTICK, firstFrameIndex + 2, 0});
    readSamples.push({S_OK, 0, firstFrameIndex + 3, 32});
    setReadSampleProvider(source,
            [&readSamples](DWORD streamIndex,
                    DWORD controlFlags,
                    DWORD* pFlags,
                    LONGLONG* pTimestamp,
                    IMFSample** ppSample) {
                return readSamples(streamIndex,
                        controlFlags,
                        pFlags,
                        pTimestamp,
                        ppSample);
            });

    SampleBuffer output(source.getSignalInfo().frames2samples(1));
    output[0] = -999;
    const auto requestedRange = IndexRange::forward(targetFrameIndex, 1);
    const auto result = source.readSampleFrames(
            WritableSampleFrames(requestedRange, SampleBuffer::WritableSlice(output)));
    const auto state = readState(source);
    EXPECT_EQ(2, readSamples.callCount());
    EXPECT_EQ(requestedRange, result.frameIndexRange());
    EXPECT_EQ(targetFrameIndex + 1, state.currentFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamTickFrameIndex);
    EXPECT_EQ(kUnknownFrameIndex, state.streamGapEndFrameIndex);
    EXPECT_EQ(2, output[0]);
}
// End AI-generated public-caller late-tick diagnostic.

} // namespace mixxx
