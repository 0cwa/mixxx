#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <vector>

#include "engine/bufferscalers/enginebufferscalesignalsmith.h"
#include "engine/readaheadmanager.h"
#include "test/mixxxtest.h"
#include "util/defs.h"

namespace {

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
