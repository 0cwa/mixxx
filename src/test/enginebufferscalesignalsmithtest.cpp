#ifdef __SIGNALSMITH__

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "engine/bufferscalers/enginebufferscalesignalsmith.h"
#include "engine/readaheadmanager.h"
#include "test/mixxxtest.h"
#include "util/sample.h"

namespace {

constexpr SINT kSampleRate = 48000;
constexpr SINT kChannels = 2;
constexpr SINT kOutputFrames = 960;
constexpr SINT kOutputSamples = kOutputFrames * kChannels;

class DeterministicReadAheadManager final : public ReadAheadManager {
  public:
    using ReadAheadManager::getNextSamplesWithRetry;

    SINT getNextSamples(double rate,
            CSAMPLE* pBuffer,
            SINT requestedSamples,
            mixxx::audio::ChannelCount channelCount) override {
        EXPECT_EQ(channelCount, mixxx::audio::ChannelCount::stereo());
        m_rates.push_back(rate);
        m_requestedSamples += requestedSamples;

        const SINT availableSamples = std::min(
                requestedSamples,
                std::max<SINT>(0, m_availableSamples - m_returnedSamples));
        for (SINT sample = 0; sample < availableSamples; ++sample) {
            const SINT frame = (m_returnedSamples + sample) / kChannels;
            const SINT channel = (m_returnedSamples + sample) % kChannels;
            const double phase = 2.0 * 3.14159265358979323846 *
                    m_toneFrequencyHz * static_cast<double>(frame) /
                    m_toneSampleRate;
            pBuffer[sample] = static_cast<CSAMPLE>(
                    0.15 * std::sin(phase + 0.17 * channel));
        }
        m_returnedSamples += availableSamples;
        return availableSamples;
    }

    NextSamplesResult getNextSamplesWithRetry(double rate,
            CSAMPLE* pBuffer,
            SINT requestedSamples,
            mixxx::audio::ChannelCount channelCount) override {
        m_retryRequestedSamples.push_back(requestedSamples);
        const int retryCall = m_retryReadCallCount++;
        if (retryCall == m_retryPendingCall) {
            return {0, true};
        }
        if (m_retryPendingCalls > 0) {
            --m_retryPendingCalls;
            return {0, true};
        }
        return {getNextSamples(rate, pBuffer, requestedSamples, channelCount), false};
    }

    void setAvailableSamples(SINT samples) {
        m_availableSamples = samples;
    }

    void setTone(double frequencyHz, double sampleRate) {
        m_toneFrequencyHz = frequencyHz;
        m_toneSampleRate = sampleRate;
    }

    void resetStats() {
        m_rates.clear();
        m_requestedSamples = 0;
        m_returnedSamples = 0;
        m_retryReadCallCount = 0;
        m_retryRequestedSamples.clear();
        m_retryPendingCall = -1;
    }

    void setRetryPendingCalls(SINT calls) {
        m_retryPendingCalls = calls;
    }

    void setRetryPendingCall(int call) {
        m_retryPendingCall = call;
    }

    SINT requestedSamples() const {
        return m_requestedSamples;
    }

    SINT returnedSamples() const {
        return m_returnedSamples;
    }

    const std::vector<double>& rates() const {
        return m_rates;
    }

    const std::vector<SINT>& retryRequestedSamples() const {
        return m_retryRequestedSamples;
    }

  private:
    std::vector<double> m_rates;
    SINT m_availableSamples = std::numeric_limits<SINT>::max();
    SINT m_requestedSamples = 0;
    SINT m_returnedSamples = 0;
    double m_toneFrequencyHz = 440.0;
    double m_toneSampleRate = kSampleRate;
    SINT m_retryPendingCalls = 0;
    int m_retryReadCallCount = 0;
    int m_retryPendingCall = -1;
    std::vector<SINT> m_retryRequestedSamples;
};

struct ScaleRun {
    double returnedSourceFrames = 0.0;
    SINT requestedSamples = 0;
    SINT returnedSamples = 0;
    std::vector<CSAMPLE> output;
};

void setRate(EngineBufferScaleSignalSmith* pScaler, double rate) {
    double tempoRatio = rate;
    double pitchRatio = 1.0;
    pScaler->setScaleParameters(1.0, &tempoRatio, &pitchRatio);
}

ScaleRun run(EngineBufferScaleSignalSmith* pScaler,
        DeterministicReadAheadManager* pReadAhead,
        SINT outputFrames = kOutputFrames) {
    ScaleRun result;
    result.output.resize(outputFrames * kChannels, 123.0f);
    result.returnedSourceFrames = pScaler->scaleBuffer(
            result.output.data(), result.output.size());
    result.requestedSamples = pReadAhead->requestedSamples();
    result.returnedSamples = pReadAhead->returnedSamples();
    return result;
}

bool allFinite(const std::vector<CSAMPLE>& samples) {
    return std::all_of(samples.begin(), samples.end(), [](CSAMPLE sample) {
        return std::isfinite(sample);
    });
}

double estimateFrequency(const std::vector<CSAMPLE>& interleavedSamples,
        SINT sampleRate,
        SINT channel = 0) {
    std::vector<double> risingZeroCrossings;
    const SINT frameCount = interleavedSamples.size() / kChannels;
    for (SINT frame = 1; frame < frameCount; ++frame) {
        const double previous = interleavedSamples[(frame - 1) * kChannels + channel];
        const double current = interleavedSamples[frame * kChannels + channel];
        if (previous <= 0.0 && current > 0.0) {
            const double fraction = -previous / (current - previous);
            risingZeroCrossings.push_back(static_cast<double>(frame - 1) + fraction);
        }
    }
    if (risingZeroCrossings.size() < 2) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double span = risingZeroCrossings.back() - risingZeroCrossings.front();
    return static_cast<double>(sampleRate) *
            static_cast<double>(risingZeroCrossings.size() - 1) / span;
}

double rms(const std::vector<CSAMPLE>& interleavedSamples) {
    double sumSquares = 0.0;
    const SINT frameCount = interleavedSamples.size() / kChannels;
    for (SINT frame = 0; frame < frameCount; ++frame) {
        const double sample = interleavedSamples[frame * kChannels];
        sumSquares += sample * sample;
    }
    return std::sqrt(sumSquares / static_cast<double>(frameCount));
}

double maxAdjacentDifference(const std::vector<CSAMPLE>& interleavedSamples) {
    double maxDifference = 0.0;
    const SINT frameCount = interleavedSamples.size() / kChannels;
    for (SINT frame = 1; frame < frameCount; ++frame) {
        maxDifference = std::max(maxDifference,
                std::fabs(static_cast<double>(
                        interleavedSamples[frame * kChannels] -
                        interleavedSamples[(frame - 1) * kChannels])));
    }
    return maxDifference;
}

std::vector<CSAMPLE> collectTone(EngineBufferScaleSignalSmith* pScaler,
        DeterministicReadAheadManager* pReadAhead,
        int callbackCount) {
    std::vector<CSAMPLE> output;
    output.reserve(callbackCount * kOutputSamples);
    for (int callback = 0; callback < callbackCount; ++callback) {
        const ScaleRun runResult = run(pScaler, pReadAhead);
        output.insert(output.end(), runResult.output.begin(), runResult.output.end());
    }
    return output;
}

} // namespace

TEST(EngineBufferScaleSignalSmithTest, RetriesTransientUnavailableInput) {
    DeterministicReadAheadManager readAhead;
    EngineBufferScaleSignalSmith scaler(&readAhead);
    scaler.setSignal(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::ChannelCount::stereo());
    setRate(&scaler, 1.0);
    readAhead.setRetryPendingCalls(1);

    std::vector<CSAMPLE> output(kOutputSamples, 123.0f);
    EXPECT_DOUBLE_EQ(0.0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
        return sample == 0.0f;
    }));
    EXPECT_EQ(0, readAhead.requestedSamples());

    const auto resumed = run(&scaler, &readAhead);
    EXPECT_GT(resumed.returnedSourceFrames, 0.0);
    EXPECT_EQ(resumed.requestedSamples, resumed.returnedSamples);
    EXPECT_TRUE(allFinite(resumed.output));
}

TEST(EngineBufferScaleSignalSmithTest, RetryReusesPendingFractionalInputRequest) {
    DeterministicReadAheadManager readAhead;
    EngineBufferScaleSignalSmith scaler(&readAhead);
    scaler.setSignal(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::ChannelCount::stereo());
    setRate(&scaler, 1.003);
    readAhead.setRetryPendingCall(1);

    std::vector<CSAMPLE> output(kOutputSamples, 123.0f);
    EXPECT_DOUBLE_EQ(0.0, scaler.scaleBuffer(output.data(), output.size()));
    ASSERT_EQ(2u, readAhead.retryRequestedSamples().size());
    const SINT pendingRequest = readAhead.retryRequestedSamples().back();

    const auto resumed = run(&scaler, &readAhead);
    ASSERT_EQ(3u, readAhead.retryRequestedSamples().size());
    EXPECT_EQ(pendingRequest, readAhead.retryRequestedSamples().back());
    EXPECT_GT(resumed.returnedSourceFrames, 0.0);
    EXPECT_TRUE(allFinite(resumed.output));
}

TEST(EngineBufferScaleSignalSmithTest, RetryReusesPendingLatencyCorrectionRequest) {
    DeterministicReadAheadManager readAhead;
    EngineBufferScaleSignalSmith scaler(&readAhead);
    scaler.setSignal(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::ChannelCount::stereo());
    setRate(&scaler, 1.0);
    ASSERT_GT(run(&scaler, &readAhead).returnedSourceFrames, 0.0);

    readAhead.resetStats();
    setRate(&scaler, 3.0);
    readAhead.setRetryPendingCall(0);

    std::vector<CSAMPLE> output(kOutputSamples, 123.0f);
    EXPECT_DOUBLE_EQ(0.0, scaler.scaleBuffer(output.data(), output.size()));
    ASSERT_EQ(1u, readAhead.retryRequestedSamples().size());
    const SINT pendingRequest = readAhead.retryRequestedSamples().back();

    const auto resumed = run(&scaler, &readAhead);
    ASSERT_EQ(2u, readAhead.retryRequestedSamples().size());
    EXPECT_EQ(pendingRequest, readAhead.retryRequestedSamples().back());
    EXPECT_GT(resumed.returnedSourceFrames, 0.0);
    EXPECT_TRUE(allFinite(resumed.output));
}

TEST(EngineBufferScaleSignalSmithTest, PrerollRateChangeFractionalAndRepeatable) {
    DeterministicReadAheadManager firstReadAhead;
    EngineBufferScaleSignalSmith firstScaler(&firstReadAhead);
    firstScaler.setSignal(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::ChannelCount::stereo());
    firstReadAhead.setAvailableSamples(std::numeric_limits<SINT>::max());

    setRate(&firstScaler, 1.0);
    const ScaleRun startup = run(&firstScaler, &firstReadAhead);
    ASSERT_GT(startup.returnedSourceFrames, 0.0);
    ASSERT_GT(startup.requestedSamples, kOutputSamples)
            << "the first positive-rate callback must include SignalSmith preroll";
    ASSERT_EQ(startup.requestedSamples, startup.returnedSamples);
    ASSERT_TRUE(allFinite(startup.output));

    firstReadAhead.resetStats();
    setRate(&firstScaler, 3.0);
    const ScaleRun fast = run(&firstScaler, &firstReadAhead);
    EXPECT_DOUBLE_EQ(3.0 * kOutputFrames, fast.returnedSourceFrames);
    EXPECT_EQ(fast.requestedSamples, fast.returnedSamples);
    EXPECT_TRUE(allFinite(fast.output));
    EXPECT_FALSE(firstReadAhead.rates().empty());
    EXPECT_GT(firstReadAhead.rates().front(), 0.0);

    firstReadAhead.resetStats();
    setRate(&firstScaler, 1.003);
    const ScaleRun fractional = run(&firstScaler, &firstReadAhead);
    EXPECT_DOUBLE_EQ(1.003 * kOutputFrames, fractional.returnedSourceFrames);
    EXPECT_EQ(fractional.requestedSamples, fractional.returnedSamples);
    EXPECT_TRUE(allFinite(fractional.output));

    DeterministicReadAheadManager secondReadAhead;
    EngineBufferScaleSignalSmith secondScaler(&secondReadAhead);
    secondScaler.setSignal(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::ChannelCount::stereo());
    setRate(&secondScaler, 1.0);
    const ScaleRun startupRepeat = run(&secondScaler, &secondReadAhead);
    secondReadAhead.resetStats();
    setRate(&secondScaler, 3.0);
    const ScaleRun fastRepeat = run(&secondScaler, &secondReadAhead);
    secondReadAhead.resetStats();
    setRate(&secondScaler, 1.003);
    const ScaleRun fractionalRepeat = run(&secondScaler, &secondReadAhead);

    ASSERT_EQ(startup.output.size(), startupRepeat.output.size());
    ASSERT_EQ(fast.output.size(), fastRepeat.output.size());
    ASSERT_EQ(fractional.output.size(), fractionalRepeat.output.size());
    for (std::size_t i = 0; i < startup.output.size(); ++i) {
        EXPECT_NEAR(startup.output[i], startupRepeat.output[i], 1e-6);
    }
    for (std::size_t i = 0; i < fast.output.size(); ++i) {
        EXPECT_NEAR(fast.output[i], fastRepeat.output[i], 1e-6);
    }
    for (std::size_t i = 0; i < fractional.output.size(); ++i) {
        EXPECT_NEAR(fractional.output[i], fractionalRepeat.output[i], 1e-6);
    }
}

TEST(EngineBufferScaleSignalSmithTest, ResetReversePauseAndEofRemainFinite) {
    DeterministicReadAheadManager readAhead;
    EngineBufferScaleSignalSmith scaler(&readAhead);
    scaler.setSignal(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::ChannelCount::stereo());
    std::vector<CSAMPLE> output(kOutputSamples, 123.0f);

    setRate(&scaler, -1.0);
    const double reverseSourceFrames = scaler.scaleBuffer(
            output.data(), output.size());
    EXPECT_DOUBLE_EQ(kOutputFrames, reverseSourceFrames);
    EXPECT_TRUE(allFinite(output));
    ASSERT_FALSE(readAhead.rates().empty());
    EXPECT_LT(readAhead.rates().front(), 0.0);

    readAhead.resetStats();
    setRate(&scaler, 0.0);
    std::fill(output.begin(), output.end(), 123.0f);
    EXPECT_DOUBLE_EQ(0.0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_EQ(0, readAhead.requestedSamples());
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
        return sample == 0.0f;
    }));

    readAhead.resetStats();
    scaler.clear();
    setRate(&scaler, 1.0);
    readAhead.setAvailableSamples(kOutputSamples / 2);
    std::fill(output.begin(), output.end(), 123.0f);
    const double eofSourceFrames = scaler.scaleBuffer(
            output.data(), output.size());
    EXPECT_DOUBLE_EQ(kOutputFrames, eofSourceFrames);
    EXPECT_TRUE(allFinite(output));
    EXPECT_LE(readAhead.returnedSamples(), readAhead.requestedSamples());
    EXPECT_EQ(readAhead.returnedSamples(), kOutputSamples / 2);
}

TEST(EngineBufferScaleSignalSmithTest, TonePitchAndLatencyWindows) {
    constexpr double kToneHz = 440.0;
    constexpr double kToneAmplitude = 0.15;
    constexpr double kSourceSampleRate = 44100.0;
    constexpr SINT kOutputSampleRate = 48000;
    constexpr double kBaseRate = kSourceSampleRate / kOutputSampleRate;
    constexpr double kTempoRatio = 0.73;
    const double kSemitoneRatio = std::pow(2.0, 1.0 / 12.0);

    struct ToneCase {
        double baseRate;
        double tempoRatio;
        double pitchRatio;
        double sourceSampleRate;
        double expectedFrequency;
    };
    const ToneCase toneCases[] = {
            // Non-unity source/output sample-rate conversion with unity pitch.
            {kBaseRate, kTempoRatio, 1.0, kSourceSampleRate, kToneHz},
            // Combined sample-rate conversion and one-semitone pitch shift.
            {kBaseRate,
                    kTempoRatio,
                    kSemitoneRatio,
                    kSourceSampleRate,
                    kToneHz * kSemitoneRatio},
            // One-semitone pitch shift without sample-rate conversion.
            {1.0, kTempoRatio, kSemitoneRatio, kOutputSampleRate, kToneHz * kSemitoneRatio},
            // Tempo-only change, preserving the original pitch.
            {1.0, kTempoRatio, 1.0, kOutputSampleRate, kToneHz}};

    for (const auto& toneCase : toneCases) {
        DeterministicReadAheadManager readAhead;
        readAhead.setTone(kToneHz, toneCase.sourceSampleRate);
        EngineBufferScaleSignalSmith scaler(&readAhead);
        scaler.setSignal(mixxx::audio::SampleRate(kOutputSampleRate),
                mixxx::audio::ChannelCount::stereo());

        double tempoRatio = toneCase.tempoRatio;
        double pitchRatio = toneCase.pitchRatio;
        scaler.setScaleParameters(toneCase.baseRate, &tempoRatio, &pitchRatio);
        const auto output = collectTone(&scaler, &readAhead, 40);

        const auto firstWindow = std::vector<CSAMPLE>(
                output.begin(), output.begin() + kOutputSamples);
        const auto steadyWindow = std::vector<CSAMPLE>(
                output.end() - 8 * kOutputSamples, output.end());
        const double firstFrequency = estimateFrequency(
                firstWindow, kOutputSampleRate);
        const double steadyFrequency = estimateFrequency(
                steadyWindow, kOutputSampleRate);
        const double firstWindowAmplitude = rms(firstWindow);
        const double firstWindowMaxStep = maxAdjacentDifference(firstWindow);
        const double firstToSecondWindowJump = std::fabs(static_cast<double>(
                output[kOutputSamples] - output[kOutputSamples - kChannels]));
        GTEST_LOG_(INFO) << "Signalsmith tone case baseRate=" << toneCase.baseRate
                         << " tempoRatio=" << tempoRatio
                         << " pitchRatio=" << pitchRatio
                         << " firstWindowHz=" << firstFrequency
                         << " steadyStateHz=" << steadyFrequency
                         << " firstWindowRms=" << firstWindowAmplitude
                         << " firstWindowMaxStep=" << firstWindowMaxStep
                         << " firstToSecondWindowJump=" << firstToSecondWindowJump
                         << " firstReadRate=" << readAhead.rates().front();
        EXPECT_TRUE(std::isfinite(firstFrequency));
        EXPECT_TRUE(std::isfinite(steadyFrequency));
        // The first callback includes Signalsmith's latency correction. Allow
        // a broad frequency tolerance for that warmup window while rejecting
        // a grossly wrong or missing signal.
        EXPECT_NEAR(toneCase.expectedFrequency, firstFrequency, kToneHz * 0.2);
        EXPECT_GT(firstWindowAmplitude, kToneAmplitude * 0.1);
        EXPECT_LT(firstWindowAmplitude, kToneAmplitude * 1.5);
        // Bound both intra-window and callback-boundary jumps relative to the
        // known test-tone amplitude instead of asserting a specific warmup
        // waveform.
        EXPECT_LT(firstWindowMaxStep, kToneAmplitude * 0.5);
        EXPECT_LT(firstToSecondWindowJump, kToneAmplitude * 0.5);
        EXPECT_DOUBLE_EQ(toneCase.baseRate * kTempoRatio, readAhead.rates().front());
        EXPECT_NEAR(toneCase.expectedFrequency,
                steadyFrequency,
                toneCase.expectedFrequency * 0.005);
    }
}

#endif // __SIGNALSMITH__
