#include "engine/readaheadmanager.h"

#include <gtest/gtest.h>

#include <QScopedPointer>
#include <QtDebug>
#include <algorithm>
#include <array>

#include "control/controlobject.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/cachingreader/cachingreaderchunk.h"
#include "engine/controls/cuecontrol.h"
#include "engine/controls/loopingcontrol.h"
#include "test/mixxxtest.h"
#include "util/assert.h"
#include "util/defs.h"
#include "util/sample.h"

namespace {
const QString kGroup = "[test]";
} // namespace

class StubReader : public CachingReader {
  public:
    StubReader()
            : CachingReader(kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo()) {
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        Q_UNUSED(reverse);
        Q_UNUSED(channelCount);
        m_readStartSamples.push_back(startSample);
        if (!m_readAvailable) {
            return CachingReader::ReadResult::UNAVAILABLE;
        }
        SampleUtil::clear(buffer, numSamples);
        return CachingReader::ReadResult::AVAILABLE;
    }

    void setReadAvailable(bool available) {
        m_readAvailable = available;
    }

    const QList<SINT>& readStartSamples() const {
        return m_readStartSamples;
    }

  protected:
    RetryReadResult readWithRetryHook(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        Q_UNUSED(reverse);
        Q_UNUSED(channelCount);
        m_readStartSamples.push_back(startSample);
        if (!m_readAvailable) {
            return {CachingReader::ReadResult::UNAVAILABLE, true};
        }
        SampleUtil::clear(buffer, numSamples);
        return {CachingReader::ReadResult::AVAILABLE, false};
    }

  private:
    bool m_readAvailable{true};
    QList<SINT> m_readStartSamples;
};

class ChunkBoundaryRetryReader : public CachingReader {
  public:
    ChunkBoundaryRetryReader()
            : CachingReader(
                      kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo()) {
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        Q_UNUSED(reverse);
        Q_UNUSED(channelCount);
        m_readStartSamples.push_back(startSample);

        const SINT boundarySample = CachingReaderChunk::frames2samples(
                CachingReaderChunk::kFrames,
                mixxx::audio::ChannelCount::stereo());
        if (!m_laterChunkReady) {
            const SINT prefixSamples = std::clamp(
                    boundarySample - startSample, SINT{0}, numSamples);
            SampleUtil::fill(buffer, 0.25f, prefixSamples);
            return CachingReader::ReadResult::PARTIALLY_AVAILABLE;
        }

        for (SINT i = 0; i < numSamples; ++i) {
            buffer[i] = sampleForAbsolutePosition(startSample + i);
        }
        return CachingReader::ReadResult::AVAILABLE;
    }

    void setLaterChunkReady() {
        m_laterChunkReady = true;
    }

    const QList<SINT>& readStartSamples() const {
        return m_readStartSamples;
    }

    static CSAMPLE sampleForAbsolutePosition(SINT sample) {
        return static_cast<CSAMPLE>((sample % 101) / 100.0f);
    }

  protected:
    RetryReadResult readWithRetryHook(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        const auto result = read(
                startSample, numSamples, reverse, buffer, channelCount);
        return {result, !m_laterChunkReady};
    }

  private:
    bool m_laterChunkReady{false};
    QList<SINT> m_readStartSamples;
};

class LegacyPartialReader : public CachingReader {
  public:
    LegacyPartialReader()
            : CachingReader(
                      kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo()) {
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        Q_UNUSED(startSample);
        Q_UNUSED(reverse);
        Q_UNUSED(channelCount);
        SampleUtil::clear(buffer, numSamples);
        return CachingReader::ReadResult::PARTIALLY_AVAILABLE;
    }
};

TEST(CachingReaderRetryTest,
        LateChunkMissAcrossChunkBoundaryLeavesDestinationUntouched) {
    constexpr auto kChannelCount = mixxx::audio::ChannelCount::stereo();
    constexpr SINT kStartFrame = CachingReaderChunk::kFrames - 2;
    constexpr SINT kFrameCount = 4;
    constexpr SINT kSampleCount = kFrameCount * 2;
    constexpr CSAMPLE kSentinel = -0.75f;
    const SINT startSample = CachingReaderChunk::frames2samples(
            kStartFrame, kChannelCount);

    ChunkBoundaryRetryReader reader;
    std::array<CSAMPLE, kSampleCount> buffer;
    buffer.fill(kSentinel);

    EXPECT_EQ(CachingReader::ReadResult::UNAVAILABLE,
            reader.readWithRetry(startSample,
                    kSampleCount,
                    false,
                    buffer.data(),
                    kChannelCount));
    for (const auto sample : buffer) {
        EXPECT_FLOAT_EQ(kSentinel, sample);
    }

    reader.setLaterChunkReady();
    EXPECT_EQ(CachingReader::ReadResult::AVAILABLE,
            reader.readWithRetry(startSample,
                    kSampleCount,
                    false,
                    buffer.data(),
                    kChannelCount));
    for (SINT i = 0; i < kSampleCount; ++i) {
        EXPECT_FLOAT_EQ(
                ChunkBoundaryRetryReader::sampleForAbsolutePosition(startSample + i),
                buffer[i]);
    }
    ASSERT_EQ(2, reader.readStartSamples().size());
    EXPECT_EQ(startSample, reader.readStartSamples()[0]);
    EXPECT_EQ(startSample, reader.readStartSamples()[1]);
}

TEST(CachingReaderRetryTest, LegacyPartialReadIsAcceptedAsIntentionalPadding) {
    constexpr auto kChannelCount = mixxx::audio::ChannelCount::stereo();
    std::array<CSAMPLE, 8> buffer;
    buffer.fill(1.0f);

    LegacyPartialReader reader;
    EXPECT_EQ(CachingReader::ReadResult::PARTIALLY_AVAILABLE,
            reader.readWithRetry(0,
                    static_cast<SINT>(buffer.size()),
                    false,
                    buffer.data(),
                    kChannelCount));
    EXPECT_TRUE(std::all_of(buffer.begin(), buffer.end(), [](CSAMPLE sample) {
        return sample == 0.0f;
    }));
}

class StubLoopControl : public LoopingControl {
  public:
    StubLoopControl()
            : LoopingControl(kGroup, UserSettingsPointer()) {
    }

    void pushValues(double trigger, double target) {
        m_triggerReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(trigger));
        m_targetReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(target));
    }

    int queryCount() const {
        return m_queryCount;
    }

    int pendingPlanCount() const {
        return m_triggerReturnValues.size();
    }

    mixxx::audio::FramePos nextTrigger(bool reverse,
            mixxx::audio::FramePos currentPosition,
            mixxx::audio::FramePos* pTargetPosition) override {
        Q_UNUSED(reverse);
        Q_UNUSED(currentPosition);
        Q_UNUSED(pTargetPosition);
        ++m_queryCount;
        RELEASE_ASSERT(!m_targetReturnValues.isEmpty());
        *pTargetPosition = m_targetReturnValues.takeFirst();
        RELEASE_ASSERT(!m_triggerReturnValues.isEmpty());
        return m_triggerReturnValues.takeFirst();
    }

  protected:
    QList<mixxx::audio::FramePos> m_triggerReturnValues;
    QList<mixxx::audio::FramePos> m_targetReturnValues;
    int m_queryCount{0};
};

class StubCueControl : public CueControl {
  public:
    StubCueControl()
            : CueControl(kGroup, UserSettingsPointer()) {
    }

    void pushValues(double trigger, double target) {
        m_triggerReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(trigger));

        m_targetReturnValues.push_back(
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(target));
    }

    int queryCount() const {
        return m_queryCount;
    }

    int pendingPlanCount() const {
        return m_triggerReturnValues.size();
    }

    mixxx::audio::FramePos nextTrigger(bool,
            mixxx::audio::FramePos,
            mixxx::audio::FramePos* pTargetPosition,
            mixxx::audio::FrameDiff_t) override {
        ++m_queryCount;
        RELEASE_ASSERT(!m_targetReturnValues.isEmpty());
        *pTargetPosition = m_targetReturnValues.takeFirst();
        RELEASE_ASSERT(!m_triggerReturnValues.isEmpty());
        return m_triggerReturnValues.takeFirst();
    }

  protected:
    QList<mixxx::audio::FramePos> m_triggerReturnValues;
    QList<mixxx::audio::FramePos> m_targetReturnValues;
    int m_queryCount{0};
};

class ReadAheadManagerTest : public MixxxTest {
  public:
    ReadAheadManagerTest()
            : m_beatClosestCO(ConfigKey(kGroup, "beat_closest")),
              m_beatNextCO(ConfigKey(kGroup, "beat_next")),
              m_beatPrevCO(ConfigKey(kGroup, "beat_prev")),
              m_playCO(ConfigKey(kGroup, "play")),
              m_stopCO(ConfigKey(kGroup, "stop")),
              m_vinylControlCO(ConfigKey(kGroup, "vinylcontrol_enabled")),
              m_vinylControlModeCO(ConfigKey(kGroup, "vinylcontrol_mode")),
              m_passthroughCO(ConfigKey(kGroup, "passthrough")),
              m_indicator250msCO(ConfigKey("[App]", "indicator_250ms")),
              m_indicator500msCO(ConfigKey("[App]", "indicator_500ms")),
              m_quantizeCO(ConfigKey(kGroup, "quantize")),
              m_repeatCO(ConfigKey(kGroup, "repeat")),
              m_slipEnabledCO(ConfigKey(kGroup, "slip_enabled")),
              m_trackSamplesCO(ConfigKey(kGroup, "track_samples")),
              m_pBuffer(SampleUtil::alloc(MAX_BUFFER_LEN)) {
    }

  protected:
    void SetUp() override {
        SampleUtil::clear(m_pBuffer, MAX_BUFFER_LEN);
        m_pReader.reset(new StubReader());
        m_pLoopControl.reset(new StubLoopControl());
        m_pCueControl.reset(new StubCueControl());
        m_pReadAheadManager.reset(new ReadAheadManager(m_pReader.data(),
                m_pLoopControl.data(),
                m_pCueControl.data()));
    }

    ControlObject m_beatClosestCO;
    ControlObject m_beatNextCO;
    ControlObject m_beatPrevCO;
    ControlObject m_playCO;
    ControlObject m_stopCO;
    ControlObject m_vinylControlCO;
    ControlObject m_vinylControlModeCO;
    ControlObject m_passthroughCO;
    ControlObject m_indicator250msCO;
    ControlObject m_indicator500msCO;
    ControlObject m_quantizeCO;
    ControlObject m_repeatCO;
    ControlObject m_slipEnabledCO;
    ControlObject m_trackSamplesCO;
    CSAMPLE* m_pBuffer;
    QScopedPointer<StubReader> m_pReader;
    QScopedPointer<StubLoopControl> m_pLoopControl;
    QScopedPointer<StubCueControl> m_pCueControl;
    QScopedPointer<ReadAheadManager> m_pReadAheadManager;
};

TEST_F(ReadAheadManagerTest, SavedJump) {
    m_pReadAheadManager->notifySeek(0.5);

    for (int i = 0; i < 2; i++) {
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    }

    m_pCueControl->pushValues(20, 6);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);

    EXPECT_EQ(20,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 30, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(6.5, m_pReadAheadManager->getPlaypos(), 1);
    EXPECT_EQ(80,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 80, mixxx::audio::ChannelCount::stereo()));

    EXPECT_NEAR(86.5, m_pReadAheadManager->getPlaypos(), 1);
}

TEST_F(ReadAheadManagerTest, RetryableCacheMissDoesNotAdvanceReadAheadPosition) {
    m_pReadAheadManager->notifySeek(0);
    m_pReader->setReadAvailable(false);
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);

    const auto unavailableResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, m_pBuffer, 10, mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(0, unavailableResult.samplesRead);
    EXPECT_TRUE(unavailableResult.retryPending);
    EXPECT_DOUBLE_EQ(0.0, m_pReadAheadManager->getPlaypos());

    m_pReader->setReadAvailable(true);

    const auto availableResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, m_pBuffer, 10, mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(10, availableResult.samplesRead);
    EXPECT_FALSE(availableResult.retryPending);
    EXPECT_DOUBLE_EQ(10.0, m_pReadAheadManager->getPlaypos());
    ASSERT_EQ(2, m_pReader->readStartSamples().size());
    EXPECT_EQ(0, m_pReader->readStartSamples()[0]);
    EXPECT_EQ(0, m_pReader->readStartSamples()[1]);
    EXPECT_EQ(1, m_pLoopControl->queryCount());
    EXPECT_EQ(1, m_pCueControl->queryCount());
}

TEST_F(ReadAheadManagerTest, RetryableCacheMissRetainsStatefulTriggerPlan) {
    m_pReadAheadManager->notifySeek(0);
    m_pReader->setReadAvailable(false);
    m_pLoopControl->pushValues(8, 2);
    m_pCueControl->pushValues(6, 4);
    // These plans must remain untouched while the first request is retried.
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    std::array<CSAMPLE, 10> output;
    output.fill(1.0f);

    const auto unavailableResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, output.data(), output.size(), mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(0, unavailableResult.samplesRead);
    EXPECT_TRUE(unavailableResult.retryPending);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
        return sample == 0.0f;
    }));
    EXPECT_EQ(1, m_pLoopControl->queryCount());
    EXPECT_EQ(1, m_pCueControl->queryCount());

    m_pReader->setReadAvailable(true);
    const auto availableResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, output.data(), output.size(), mixxx::audio::ChannelCount::stereo());

    EXPECT_EQ(6, availableResult.samplesRead);
    EXPECT_FALSE(availableResult.retryPending);
    EXPECT_DOUBLE_EQ(4.0, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(1, m_pLoopControl->queryCount());
    EXPECT_EQ(1, m_pCueControl->queryCount());
    EXPECT_EQ(1, m_pLoopControl->pendingPlanCount());
    EXPECT_EQ(1, m_pCueControl->pendingPlanCount());
    ASSERT_GE(m_pReader->readStartSamples().size(), 2);
    EXPECT_EQ(0, m_pReader->readStartSamples()[0]);
    EXPECT_EQ(0, m_pReader->readStartSamples()[1]);
}

TEST_F(ReadAheadManagerTest, NotifySeekCancelsPendingTriggerPlan) {
    m_pReadAheadManager->notifySeek(0);
    m_pReader->setReadAvailable(false);
    m_pLoopControl->pushValues(8, 2);
    m_pCueControl->pushValues(6, 4);
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);

    const auto unavailableResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, m_pBuffer, 10, mixxx::audio::ChannelCount::stereo());
    ASSERT_TRUE(unavailableResult.retryPending);

    m_pReadAheadManager->notifySeek(20);
    m_pReader->setReadAvailable(true);
    const auto availableResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, m_pBuffer, 10, mixxx::audio::ChannelCount::stereo());

    EXPECT_EQ(10, availableResult.samplesRead);
    EXPECT_FALSE(availableResult.retryPending);
    EXPECT_DOUBLE_EQ(30.0, m_pReadAheadManager->getPlaypos());
    EXPECT_EQ(2, m_pLoopControl->queryCount());
    EXPECT_EQ(2, m_pCueControl->queryCount());
    EXPECT_EQ(0, m_pLoopControl->pendingPlanCount());
    EXPECT_EQ(0, m_pCueControl->pendingPlanCount());
    ASSERT_GE(m_pReader->readStartSamples().size(), 2);
    EXPECT_EQ(0, m_pReader->readStartSamples()[0]);
    EXPECT_EQ(20, m_pReader->readStartSamples()[1]);
}

TEST_F(ReadAheadManagerTest, TriggerOnJumpOrLoop) {
    m_pReadAheadManager->notifySeek(0);

    // The jump trigger is located before the loop end
    m_pLoopControl->pushValues(50, 10);
    m_pCueControl->pushValues(40, 20);

    EXPECT_EQ(40,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(20, m_pReadAheadManager->getPlaypos(), 1);

    m_pReadAheadManager->notifySeek(0);

    // The jump trigger is located after the loop end
    m_pLoopControl->pushValues(50, 40);
    m_pCueControl->pushValues(60, 30);

    EXPECT_EQ(50,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    EXPECT_NEAR(40, m_pReadAheadManager->getPlaypos(), 1);
}

TEST_F(ReadAheadManagerTest, FractionalFrameLoop) {
    // If we are in reverse, a loop is enabled, and the current playposition
    // is before of the loop, we should seek to the out point of the loop.
    m_pReadAheadManager->notifySeek(0.5);
    // Trigger value means, the sample that triggers the loop (loop in) and the
    // sample we should seek to.
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, 3.3);
    m_pLoopControl->pushValues(20.2, kNoTrigger);

    for (int i = 0; i < 6; i++) {
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    }

    // read from start to loop trigger, overshoot 0.3
    EXPECT_EQ(20,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 100, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(18,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 80, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(16,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 62, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(18,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 46, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(16,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 28, mixxx::audio::ChannelCount::stereo()));
    // read loop
    EXPECT_EQ(12,
            m_pReadAheadManager->getNextSamples(
                    1.0, m_pBuffer, 12, mixxx::audio::ChannelCount::stereo()));

    // start 0.5 to 20.2 = 19.7
    // loop 3.3 to 20.2 = 16.9
    // 100 - 19,7 - 4 * 16,9 = 12,7
    // 12.7 + 3.3 = 16

    // The rounding error must not exceed a half frame (one samples in stereo)
    EXPECT_NEAR(16, m_pReadAheadManager->getPlaypos(), 1);
}
