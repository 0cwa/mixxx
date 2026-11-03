#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "engine/bufferscalers/enginebufferscalesignalsmith.h"
#include "engine/readaheadmanager.h"
#include "signalsmith-stretch.h"
#include "test/mixxxtest.h"
#include "util/defs.h"
#include "util/sample.h"

namespace {

class ScriptedReader final : public ReadAheadManager {
  public:
    SINT getNextSamples(double rate,
            CSAMPLE* buffer,
            SINT requested,
            mixxx::audio::ChannelCount) override {
        lastRate = rate;
        if (++calls > callLimit) {
            throw std::runtime_error("Scaler exceeded bounded reader-call limit");
        }
        SINT count = requested;
        if (!reads.empty()) {
            count = std::min(count, reads.front());
            reads.erase(reads.begin());
        }
        for (SINT i = 0; i < count; ++i) {
            buffer[i] = samples.empty() ? 0.0f : samples[position++ % samples.size()];
        }
        consumed += count;
        return count;
    }

    NextSamplesResult getNextSamplesWithRetry(double rate,
            CSAMPLE* buffer,
            SINT requested,
            mixxx::audio::ChannelCount channelCount) override {
        if (retrySteps.empty()) {
            return {getNextSamples(rate, buffer, requested, channelCount), false};
        }
        lastRate = rate;
        if (++calls > callLimit) {
            throw std::runtime_error("Scaler exceeded bounded reader-call limit");
        }
        requests.push_back(requested);
        auto step = retrySteps.front();
        retrySteps.erase(retrySteps.begin());
        const SINT count = step.samplesRead < 0
                ? requested
                : std::min(requested, step.samplesRead);
        for (SINT i = 0; i < count; ++i) {
            buffer[i] = samples.empty() ? 0.0f : samples[position++ % samples.size()];
        }
        consumed += count;
        return {count, step.retryPending};
    }

    struct RetryStep {
        SINT samplesRead;
        bool retryPending;
    };

    std::vector<CSAMPLE> samples;
    std::vector<SINT> reads;
    std::vector<SINT> requests;
    std::vector<RetryStep> retrySteps;
    SINT position{0};
    SINT consumed{0};
    SINT calls{0};
    SINT callLimit{100};
    double lastRate{0};
};

class EngineBufferScaleSignalSmithTest : public MixxxTest {
  protected:
    void initialize(EngineBufferScaleSignalSmith* scaler,
            mixxx::audio::ChannelCount channels = mixxx::audio::ChannelCount::stereo(),
            double tempo = 1.0,
            double pitch = 1.0) {
        scaler->setSignal(mixxx::audio::SampleRate(44100), channels);
        scaler->setScaleParameters(1.0, &tempo, &pitch);
    }

    void fillTone(ScriptedReader* reader, SINT channels) {
        for (int frame = 0; frame < 8192; ++frame) {
            const CSAMPLE sample = static_cast<CSAMPLE>(0.4 * std::sin(2.0 * M_PI * frame / 48.0));
            for (SINT channel = 0; channel < channels; ++channel) {
                reader->samples.push_back(sample);
            }
        }
    }

    void compare(const std::vector<CSAMPLE>& actual, const std::vector<CSAMPLE>& expected) {
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            EXPECT_NEAR(actual[i], expected[i], 1e-5) << i;
        }
    }
};

TEST_F(EngineBufferScaleSignalSmithTest, InstancesAndRecreatedScalerKeepIndependentOutput) {
    ScriptedReader readers[3];
    auto channels = {mixxx::audio::ChannelCount::stereo(), mixxx::audio::ChannelCount::stem()};
    for (auto channelCount : channels) {
        for (auto& reader : readers) {
            reader.samples.clear();
            reader.position = 0;
            fillTone(&reader, channelCount);
        }
        EngineBufferScaleSignalSmith first(&readers[0]);
        EngineBufferScaleSignalSmith second(&readers[1]);
        initialize(&first, channelCount);
        initialize(&second, channelCount);
        std::vector<CSAMPLE> output(512 * channelCount);
        std::vector<CSAMPLE> expected(output.size());
        first.scaleBuffer(output.data(), output.size());
        second.scaleBuffer(expected.data(), expected.size());
        compare(output, expected);
        auto survivor = std::make_unique<EngineBufferScaleSignalSmith>(&readers[2]);
        initialize(survivor.get(), channelCount);
        std::vector<CSAMPLE> survivorOutput(output.size());
        survivor->scaleBuffer(survivorOutput.data(), survivorOutput.size());
        compare(survivorOutput, expected);
        survivor.reset();
        readers[2].position = 0;
        survivor = std::make_unique<EngineBufferScaleSignalSmith>(&readers[2]);
        initialize(survivor.get(), channelCount);
        std::vector<CSAMPLE> recreated(output.size());
        survivor->scaleBuffer(recreated.data(), recreated.size());
        compare(recreated, expected);
    }
}

TEST_F(EngineBufferScaleSignalSmithTest, PitchShiftProcessesMonoInput) {
    ScriptedReader reader;
    fillTone(&reader, 1);
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler, mixxx::audio::ChannelCount::mono(), 1.0, 1.5);
    std::vector<CSAMPLE> warmup(512);
    scaler.scaleBuffer(warmup.data(), warmup.size());
    const SINT inputStart = reader.position;
    std::vector<CSAMPLE> output(512);
    scaler.scaleBuffer(output.data(), output.size());
    bool changed = false;
    for (size_t i = 0; i < output.size(); ++i) {
        EXPECT_TRUE(std::isfinite(output[i]));
        changed |= std::abs(output[i] -
                           reader.samples[(inputStart + i) %
                                   reader.samples.size()]) > 1e-3;
    }
    EXPECT_TRUE(changed);
}

TEST_F(EngineBufferScaleSignalSmithTest, PartialAndZeroPrerollMatchesContiguousReference) {
    ScriptedReader partial;
    ScriptedReader contiguous;
    fillTone(&partial, 2);
    fillTone(&contiguous, 2);
    EngineBufferScaleSignalSmith scaler(&partial);
    EngineBufferScaleSignalSmith reference(&contiguous);
    initialize(&scaler);
    initialize(&reference);
    partial.reads = {6, 0};
    std::vector<CSAMPLE> actual(1024);
    std::vector<CSAMPLE> expected(1024);
    EXPECT_EQ(scaler.scaleBuffer(actual.data(), actual.size()), 512.0);
    reference.scaleBuffer(expected.data(), expected.size());
    EXPECT_EQ(partial.consumed, contiguous.consumed);
    compare(actual, expected);
}

TEST_F(EngineBufferScaleSignalSmithTest, CacheRetryPreservesPartialInputPrefix) {
    ScriptedReader recovering;
    ScriptedReader referenceReader;
    fillTone(&recovering, 2);
    referenceReader.samples = recovering.samples;
    EngineBufferScaleSignalSmith recoveringScaler(&recovering);
    EngineBufferScaleSignalSmith referenceScaler(&referenceReader);
    initialize(&recoveringScaler);
    initialize(&referenceScaler);
    std::vector<CSAMPLE> warmup(128), recoveredOutput(128), expectedOutput(128);
    recoveringScaler.scaleBuffer(warmup.data(), warmup.size());
    referenceScaler.scaleBuffer(warmup.data(), warmup.size());

    recovering.retrySteps = {{8, false}, {0, true}, {-1, false}};
    EXPECT_DOUBLE_EQ(0,
            recoveringScaler.scaleBuffer(
                    recoveredOutput.data(), recoveredOutput.size()));
    EXPECT_TRUE(std::all_of(recoveredOutput.begin(), recoveredOutput.end(), [](CSAMPLE sample) {
        return sample == 0.0f;
    }));

    EXPECT_DOUBLE_EQ(64,
            recoveringScaler.scaleBuffer(
                    recoveredOutput.data(), recoveredOutput.size()));
    EXPECT_DOUBLE_EQ(64, referenceScaler.scaleBuffer(expectedOutput.data(), expectedOutput.size()));
    compare(recoveredOutput, expectedOutput);
    EXPECT_EQ(referenceReader.consumed, recovering.consumed);
    ASSERT_EQ(3, recovering.requests.size());
    EXPECT_EQ(128, recovering.requests[0]);
    EXPECT_EQ(120, recovering.requests[1]);
    EXPECT_EQ(120, recovering.requests[2]);
}

TEST_F(EngineBufferScaleSignalSmithTest,
        CompletesPendingRetryBeforeApplyingInvalidParameters) {
    ScriptedReader recovering;
    ScriptedReader referenceReader;
    fillTone(&recovering, 2);
    referenceReader.samples = recovering.samples;
    EngineBufferScaleSignalSmith recoveringScaler(&recovering);
    EngineBufferScaleSignalSmith referenceScaler(&referenceReader);
    initialize(&recoveringScaler);
    initialize(&referenceScaler);
    std::vector<CSAMPLE> warmup(128), recoveredOutput(128), expectedOutput(128);
    recoveringScaler.scaleBuffer(warmup.data(), warmup.size());
    referenceScaler.scaleBuffer(warmup.data(), warmup.size());

    recovering.retrySteps = {{8, false}, {0, true}, {-1, false}};
    EXPECT_DOUBLE_EQ(0,
            recoveringScaler.scaleBuffer(
                    recoveredOutput.data(), recoveredOutput.size()));
    EXPECT_TRUE(std::all_of(recoveredOutput.begin(),
            recoveredOutput.end(),
            [](CSAMPLE sample) { return sample == 0.0f; }));

    double tempo = std::numeric_limits<double>::quiet_NaN();
    double pitch = 1;
    recoveringScaler.setScaleParameters(1, &tempo, &pitch);
    EXPECT_DOUBLE_EQ(0, tempo);
    EXPECT_DOUBLE_EQ(1, pitch);

    EXPECT_DOUBLE_EQ(64,
            recoveringScaler.scaleBuffer(
                    recoveredOutput.data(), recoveredOutput.size()));
    EXPECT_DOUBLE_EQ(64,
            referenceScaler.scaleBuffer(expectedOutput.data(), expectedOutput.size()));
    compare(recoveredOutput, expectedOutput);

    const SINT callsAfterPendingScale = recovering.calls;
    std::fill(recoveredOutput.begin(), recoveredOutput.end(), 0.75f);
    EXPECT_DOUBLE_EQ(0,
            recoveringScaler.scaleBuffer(
                    recoveredOutput.data(), recoveredOutput.size()));
    EXPECT_EQ(callsAfterPendingScale, recovering.calls);
    EXPECT_TRUE(std::all_of(recoveredOutput.begin(),
            recoveredOutput.end(),
            [](CSAMPLE sample) { return sample == 0.0f; }));
}

TEST_F(EngineBufferScaleSignalSmithTest,
        ClearAppliesDeferredParametersFromPendingRetry) {
    ScriptedReader reader;
    fillTone(&reader, 2);
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);
    std::vector<CSAMPLE> warmup(128), output(128);
    scaler.scaleBuffer(warmup.data(), warmup.size());

    reader.retrySteps = {{8, false}, {0, true}};
    EXPECT_DOUBLE_EQ(0, scaler.scaleBuffer(output.data(), output.size()));

    double tempo = 0.5;
    double pitch = 1;
    scaler.setScaleParameters(1, &tempo, &pitch);
    scaler.clear();
    reader.retrySteps.clear();

    EXPECT_DOUBLE_EQ(32, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_DOUBLE_EQ(0.5, reader.lastRate);

    reader.retrySteps = {{8, false}, {0, true}};
    EXPECT_DOUBLE_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    double invalidTempo = std::numeric_limits<double>::quiet_NaN();
    double invalidPitch = 1;
    scaler.setScaleParameters(1, &invalidTempo, &invalidPitch);
    scaler.clear();
    reader.retrySteps.clear();

    const SINT callsBeforeInvalidScale = reader.calls;
    std::fill(output.begin(), output.end(), 0.75f);
    EXPECT_DOUBLE_EQ(0, scaler.scaleBuffer(output.data(), output.size()));
    EXPECT_EQ(callsBeforeInvalidScale, reader.calls);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
        return sample == 0.0f;
    }));
}

TEST_F(EngineBufferScaleSignalSmithTest, RepeatedZeroReadsPadPrerollAndProcessing) {
    for (auto preset : {EngineBufferScaleSignalSmith::Preset::Default,
                 EngineBufferScaleSignalSmith::Preset::Cheaper}) {
        for (bool preroll : {false, true}) {
            ScriptedReader partial;
            ScriptedReader padded;
            fillTone(&partial, 2);
            padded.samples = partial.samples;
            EngineBufferScaleSignalSmith scaler(&partial);
            EngineBufferScaleSignalSmith reference(&padded);
            scaler.setPreset(preset);
            reference.setPreset(preset);
            initialize(&scaler);
            initialize(&reference);
            std::vector<CSAMPLE> actual(1024), expected(1024);
            if (!preroll) {
                scaler.scaleBuffer(actual.data(), actual.size());
                reference.scaleBuffer(expected.data(), expected.size());
            }
            signalsmith::stretch::SignalsmithStretch<float> stretch;
            if (preset == EngineBufferScaleSignalSmith::Preset::Cheaper) {
                stretch.presetCheaper(2, 44100);
            } else {
                stretch.presetDefault(2, 44100);
            }
            const SINT requested = preroll
                    ? (stretch.inputLatency() + stretch.outputLatency()) * 2
                    : actual.size();
            padded.samples.insert(padded.samples.begin() + padded.position + 6,
                    requested - 6,
                    0.0f);
            partial.reads = {6, 0, 0};
            partial.callLimit = partial.calls + (preroll ? 4 : 3);
            EXPECT_EQ(scaler.scaleBuffer(actual.data(), actual.size()), 512.0);
            reference.scaleBuffer(expected.data(), expected.size());
            compare(actual, expected);
            EXPECT_EQ(partial.calls, partial.callLimit);
        }
    }
}

TEST_F(EngineBufferScaleSignalSmithTest, TempoChangeLimitsLatencyCorrectionPerCallback) {
    ScriptedReader reader;
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);
    std::vector<CSAMPLE> output(128);
    scaler.scaleBuffer(output.data(), output.size());

    double tempo = 2.0;
    double pitch = 1.0;
    scaler.setScaleParameters(1.0, &tempo, &pitch);
    const SINT before = reader.consumed;
    scaler.scaleBuffer(output.data(), output.size());
    const SINT outputFrames = output.size() / 2;
    EXPECT_LE(reader.consumed - before, (tempo + 1) * outputFrames * 2);
}

TEST_F(EngineBufferScaleSignalSmithTest,
        FractionalDemandSurvivesLatencyReductionAcrossTinyCallbacks) {
    ScriptedReader reader;
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);
    std::vector<CSAMPLE> warmup(1024);
    scaler.scaleBuffer(warmup.data(), warmup.size());
    const SINT before = reader.consumed;
    double tempo = 0.125;
    double pitch = 1.0;
    scaler.setScaleParameters(1.0, &tempo, &pitch);
    reader.callLimit = reader.calls + 24000;
    CSAMPLE output[2];
    double reported = 0;
    for (int i = 0; i < 24000; ++i) {
        reported += scaler.scaleBuffer(output, 2);
    }
    signalsmith::stretch::SignalsmithStretch<float> reference;
    reference.presetDefault(2, 44100);
    const SINT latencyReduction = static_cast<SINT>(
                                          std::round(tempo * reference.outputLatency())) -
            reference.outputLatency();
    EXPECT_DOUBLE_EQ(reported, 3000.0);
    EXPECT_EQ(reader.consumed - before, (3000 + latencyReduction) * 2);
    EXPECT_LE(reader.calls, reader.callLimit);
}

TEST_F(EngineBufferScaleSignalSmithTest, ZeroTempoHoldsAudioAndReverseConsumesPositively) {
    ScriptedReader reader;
    fillTone(&reader, 2);
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);
    std::vector<CSAMPLE> output(1024);
    scaler.scaleBuffer(output.data(), output.size());
    double tempo = 0.0;
    double pitch = 1.0;
    scaler.setScaleParameters(1.0, &tempo, &pitch);
    const SINT before = reader.consumed;
    scaler.scaleBuffer(output.data(), output.size());
    EXPECT_EQ(reader.consumed, before);
    EXPECT_TRUE(std::any_of(output.begin(), output.end(), [](CSAMPLE x) {
        return std::abs(x) > 1e-4;
    }));
    tempo = -0.75;
    scaler.setScaleParameters(1.0, &tempo, &pitch);
    scaler.clear();
    EXPECT_EQ(scaler.scaleBuffer(output.data(), output.size()), 384.0);
    EXPECT_LT(reader.lastRate, 0.0);
    const SINT consumed = reader.consumed;
    EXPECT_EQ(scaler.scaleBuffer(output.data(), output.size()), 384.0);
    EXPECT_EQ(reader.consumed - consumed, 768);
}

class CountingReader final : public ReadAheadManager {
  public:
    SINT getNextSamples(double rate,
            CSAMPLE* buffer,
            SINT requested,
            mixxx::audio::ChannelCount) override {
        ++calls;
        lastRate = rate;
        std::fill_n(buffer, requested, 0.0f);
        return requested;
    }

    SINT calls{0};
    double lastRate{0};
};

class EngineBufferScaleSignalSmithSafetyTest : public MixxxTest {
  protected:
    void initialize(EngineBufferScaleSignalSmith* scaler) {
        scaler->setSignal(
                mixxx::audio::SampleRate(44100),
                mixxx::audio::ChannelCount::stereo());
        double tempo = 1;
        double pitch = 1;
        scaler->setScaleParameters(1, &tempo, &pitch);
    }
};

TEST_F(EngineBufferScaleSignalSmithSafetyTest,
        ClampsFiniteRatesRejectsNonfiniteParametersAndRecovers) {
    CountingReader reader;
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);

    double tempo = -std::numeric_limits<double>::max();
    double pitch = 0;
    scaler.setScaleParameters(2, &tempo, &pitch);
    EXPECT_DOUBLE_EQ(-MAX_SEEK_SPEED / 2, tempo);
    EXPECT_DOUBLE_EQ(1.0 / MAX_SEEK_SPEED / 2, pitch);
    CSAMPLE output[2] = {};
    EXPECT_DOUBLE_EQ(MAX_SEEK_SPEED, scaler.scaleBuffer(output, 2));
    EXPECT_LT(reader.lastRate, 0);

    const SINT highDemandFrames =
            static_cast<SINT>(MAX_BUFFER_LEN / MAX_SEEK_SPEED) + 1;
    std::vector<CSAMPLE> highDemand(highDemandFrames * 2, 0.75f);
    const SINT callsBeforeHighDemand = reader.calls;
    EXPECT_DOUBLE_EQ(0,
            scaler.scaleBuffer(highDemand.data(), static_cast<SINT>(highDemand.size())));
    EXPECT_EQ(callsBeforeHighDemand, reader.calls);
    EXPECT_TRUE(std::all_of(highDemand.begin(), highDemand.end(), [](CSAMPLE sample) {
        return sample == 0;
    }));

    const auto expectInvalidParametersSilenced = [&](double baseRate,
                                                         double invalidTempo,
                                                         double invalidPitch) {
        scaler.setScaleParameters(baseRate, &invalidTempo, &invalidPitch);
        EXPECT_DOUBLE_EQ(0, invalidTempo);
        EXPECT_DOUBLE_EQ(1, invalidPitch);
        const SINT callsBefore = reader.calls;
        std::fill_n(output, 2, 0.75f);
        EXPECT_DOUBLE_EQ(0, scaler.scaleBuffer(output, 2));
        EXPECT_EQ(callsBefore, reader.calls);
        EXPECT_EQ(0, output[0]);
        EXPECT_EQ(0, output[1]);
    };
    expectInvalidParametersSilenced(
            1, std::numeric_limits<double>::quiet_NaN(), 1);
    expectInvalidParametersSilenced(
            1, 1, std::numeric_limits<double>::infinity());
    expectInvalidParametersSilenced(
            std::numeric_limits<double>::infinity(), 1, 1);

    tempo = pitch = 1;
    scaler.setScaleParameters(1, &tempo, &pitch);
    EXPECT_DOUBLE_EQ(1, scaler.scaleBuffer(output, 2));
    EXPECT_GT(reader.calls, 0);
}

TEST_F(EngineBufferScaleSignalSmithSafetyTest,
        RejectsMalformedAndOverCapacityOutputWithoutReading) {
    CountingReader reader;
    EngineBufferScaleSignalSmith scaler(&reader);
    initialize(&scaler);

    CSAMPLE malformed[3] = {0.75f, 0.75f, 0.75f};
    EXPECT_DOUBLE_EQ(0, scaler.scaleBuffer(malformed, 3));
    EXPECT_EQ(reader.calls, 0);
    EXPECT_TRUE(std::all_of(std::begin(malformed),
            std::end(malformed),
            [](CSAMPLE sample) { return sample == 0; }));
    EXPECT_DOUBLE_EQ(0, scaler.scaleBuffer(nullptr, 2));
    EXPECT_DOUBLE_EQ(0, scaler.scaleBuffer(malformed, -1));
    EXPECT_EQ(reader.calls, 0);

    std::vector<CSAMPLE> tooLarge((MAX_BUFFER_LEN + 1) * 2 + 2, 0.75f);
    EXPECT_DOUBLE_EQ(0,
            scaler.scaleBuffer(tooLarge.data() + 1,
                    static_cast<SINT>(tooLarge.size() - 2)));
    EXPECT_EQ(reader.calls, 0);
    EXPECT_EQ(0.75f, tooLarge.front());
    EXPECT_EQ(0.75f, tooLarge.back());
    EXPECT_TRUE(std::all_of(tooLarge.begin() + 1,
            tooLarge.end() - 1,
            [](CSAMPLE sample) { return sample == 0; }));

    scaler.setSignal(
            mixxx::audio::SampleRate(44100),
            mixxx::audio::ChannelCount(9));
    CSAMPLE unsupported[9];
    std::fill_n(unsupported, 9, 0.75f);
    EXPECT_DOUBLE_EQ(0, scaler.scaleBuffer(unsupported, 9));
    EXPECT_EQ(reader.calls, 0);
    EXPECT_TRUE(std::all_of(std::begin(unsupported),
            std::end(unsupported),
            [](CSAMPLE sample) { return sample == 0; }));
}

} // namespace
