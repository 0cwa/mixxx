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
            pBuffer[sample] = static_cast<CSAMPLE>(
                    0.15 * std::sin(0.031 * frame + 0.17 * channel));
        }
        m_returnedSamples += availableSamples;
        return availableSamples;
    }

    NextSamplesResult getNextSamplesWithRetry(double rate,
            CSAMPLE* pBuffer,
            SINT requestedSamples,
            mixxx::audio::ChannelCount channelCount) override {
        if (m_retryPendingCalls > 0) {
            --m_retryPendingCalls;
            return {0, true};
        }
        return {getNextSamples(rate, pBuffer, requestedSamples, channelCount), false};
    }

    void setAvailableSamples(SINT samples) {
        m_availableSamples = samples;
    }

    void resetStats() {
        m_rates.clear();
        m_requestedSamples = 0;
        m_returnedSamples = 0;
    }

    void setRetryPendingCalls(SINT calls) {
        m_retryPendingCalls = calls;
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

  private:
    std::vector<double> m_rates;
    SINT m_availableSamples = std::numeric_limits<SINT>::max();
    SINT m_requestedSamples = 0;
    SINT m_returnedSamples = 0;
    SINT m_retryPendingCalls = 0;
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

#endif // __SIGNALSMITH__
