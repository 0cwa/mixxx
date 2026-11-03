#include "engine/readaheadmanager.h"

#include <gtest/gtest.h>

#include <QScopedPointer>
#include <QtDebug>
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <vector>

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
    explicit StubReader(mixxx::audio::ChannelCount channelCount =
                                mixxx::audio::ChannelCount::stereo())
            : CachingReader(kGroup, UserSettingsPointer(), channelCount) {
    }

    void pushReadResults(std::initializer_list<CachingReader::ReadResult> results) {
        m_readResults.insert(m_readResults.end(), results.begin(), results.end());
    }

    const std::vector<SINT>& readStarts() const {
        return m_readStarts;
    }

    const std::vector<SINT>& readSizes() const {
        return m_readSizes;
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        Q_UNUSED(channelCount);
        Q_UNUSED(reverse);
        m_readStarts.push_back(startSample);
        m_readSizes.push_back(numSamples);
        SampleUtil::clear(buffer, numSamples);
        if (m_nextResult < m_readResults.size()) {
            return m_readResults[m_nextResult++];
        }
        return CachingReader::ReadResult::AVAILABLE;
    }

  private:
    std::vector<CachingReader::ReadResult> m_readResults;
    std::size_t m_nextResult{0};
    std::vector<SINT> m_readStarts;
    std::vector<SINT> m_readSizes;
};

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

    void pushFrameValues(mixxx::audio::FramePos trigger, mixxx::audio::FramePos target) {
        m_triggerReturnValues.push_back(trigger);
        m_targetReturnValues.push_back(target);
    }

    mixxx::audio::FramePos nextTrigger(bool reverse,
            mixxx::audio::FramePos currentPosition,
            mixxx::audio::FramePos* pTargetPosition) override {
        Q_UNUSED(reverse);
        Q_UNUSED(currentPosition);
        Q_UNUSED(pTargetPosition);
        RELEASE_ASSERT(!m_targetReturnValues.isEmpty());
        *pTargetPosition = m_targetReturnValues.takeFirst();
        RELEASE_ASSERT(!m_triggerReturnValues.isEmpty());
        return m_triggerReturnValues.takeFirst();
    }

  protected:
    QList<mixxx::audio::FramePos> m_triggerReturnValues;
    QList<mixxx::audio::FramePos> m_targetReturnValues;
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

    mixxx::audio::FramePos nextTrigger(bool,
            mixxx::audio::FramePos,
            mixxx::audio::FramePos* pTargetPosition,
            mixxx::audio::FrameDiff_t) override {
        RELEASE_ASSERT(!m_targetReturnValues.isEmpty());
        *pTargetPosition = m_targetReturnValues.takeFirst();
        RELEASE_ASSERT(!m_triggerReturnValues.isEmpty());
        return m_triggerReturnValues.takeFirst();
    }

  protected:
    QList<mixxx::audio::FramePos> m_triggerReturnValues;
    QList<mixxx::audio::FramePos> m_targetReturnValues;
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

    ~ReadAheadManagerTest() override {
        SampleUtil::free(m_pBuffer);
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

TEST_F(ReadAheadManagerTest, CrossfadeHandlesHighRateStemLoop) {
    constexpr auto kChannelCount = mixxx::audio::ChannelCount::stem();
    constexpr SINT kCurrentFrame = 75000;
    constexpr SINT kLoopEndFrame = 100000;
    constexpr SINT kLoopStartFrame = 50000;
    constexpr SINT kOutputFrames = 8192;
    constexpr SINT kRate = 4;
    constexpr SINT kRequestSamples = kOutputFrames * kRate * kChannelCount;
    constexpr SINT kReadSamples = (kLoopEndFrame - kCurrentFrame) * kChannelCount;
    constexpr CSAMPLE kSentinel = -1.0f;
    std::vector<CSAMPLE> output(kRequestSamples, kSentinel);

    auto reader = std::make_unique<StubReader>(kChannelCount);
    auto readAhead = std::make_unique<ReadAheadManager>(reader.get(),
            m_pLoopControl.data(),
            m_pCueControl.data());
    m_pLoopControl->setFrameInfo(mixxx::audio::kStartFramePos,
            mixxx::audio::FramePos::fromSamplePos(200000 * kChannelCount, kChannelCount),
            mixxx::audio::SampleRate(48000));
    readAhead->notifySeek(kCurrentFrame * kChannelCount);
    m_pLoopControl->pushFrameValues(
            mixxx::audio::FramePos::fromSamplePos(
                    kLoopEndFrame * kChannelCount, kChannelCount),
            mixxx::audio::FramePos::fromSamplePos(
                    kLoopStartFrame * kChannelCount, kChannelCount));
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);

    EXPECT_EQ(kReadSamples,
            readAhead->getNextSamples(
                    kRate, output.data(), kRequestSamples, kChannelCount));
    EXPECT_EQ(kLoopStartFrame * kChannelCount, readAhead->getPlaypos());
    EXPECT_TRUE(std::all_of(output.begin(),
            output.begin() + kReadSamples,
            [](CSAMPLE sample) { return sample == 0.0f; }));
    EXPECT_TRUE(std::all_of(output.begin() + kReadSamples,
            output.end(),
            [](CSAMPLE sample) { return sample == kSentinel; }));
}

TEST_F(ReadAheadManagerTest, RetryReadPreservesDestinationOnPartialCacheMiss) {
    constexpr CSAMPLE kSentinel = -1.0f;
    std::vector<CSAMPLE> output(16, kSentinel);
    m_pReader->pushReadResults({CachingReader::ReadResult::PARTIALLY_UNAVAILABLE});

    EXPECT_EQ(CachingReader::ReadResult::UNAVAILABLE,
            m_pReader->readWithRetry(
                    0, output.size(), false, output.data(), mixxx::audio::ChannelCount::stereo()));
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
        return sample == kSentinel;
    }));

    m_pReader->pushReadResults({CachingReader::ReadResult::PARTIALLY_AVAILABLE});
    EXPECT_EQ(CachingReader::ReadResult::PARTIALLY_AVAILABLE,
            m_pReader->readWithRetry(
                    0, output.size(), false, output.data(), mixxx::audio::ChannelCount::stereo()));
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
        return sample == 0.0f;
    }));
}

TEST_F(ReadAheadManagerTest, RetryKeepsLoopTriggerAndReadPosition) {
    constexpr auto kChannels = mixxx::audio::ChannelCount::stereo();
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->pushReadResults({CachingReader::ReadResult::PARTIALLY_UNAVAILABLE,
            CachingReader::ReadResult::AVAILABLE});
    std::vector<CSAMPLE> output(16, -1.0f);

    const auto missed = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, output.data(), output.size(), kChannels);
    EXPECT_EQ(0, missed.samplesRead);
    EXPECT_TRUE(missed.retryPending);
    EXPECT_DOUBLE_EQ(0, m_pReadAheadManager->getPlaypos());

    const auto retried = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, output.data(), output.size(), kChannels);
    EXPECT_EQ(8, retried.samplesRead);
    EXPECT_FALSE(retried.retryPending);
    EXPECT_DOUBLE_EQ(4, m_pReadAheadManager->getPlaypos());
    ASSERT_EQ(2, m_pReader->readSizes().size());
    EXPECT_EQ(8, m_pReader->readSizes()[0]);
    EXPECT_EQ(8, m_pReader->readSizes()[1]);
}

TEST_F(ReadAheadManagerTest, RetryHintsUnavailableCrossfadeRange) {
    constexpr auto kChannels = mixxx::audio::ChannelCount::stereo();
    m_pLoopControl->setFrameInfo(mixxx::audio::kStartFramePos,
            mixxx::audio::FramePos::fromSamplePos(200 * kChannels, kChannels),
            mixxx::audio::SampleRate(48000));
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(8, 4);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->pushReadResults({CachingReader::ReadResult::AVAILABLE,
            CachingReader::ReadResult::PARTIALLY_UNAVAILABLE,
            CachingReader::ReadResult::AVAILABLE,
            CachingReader::ReadResult::AVAILABLE});
    std::vector<CSAMPLE> output(16, -1.0f);

    const auto missed = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, output.data(), output.size(), kChannels);
    EXPECT_TRUE(missed.retryPending);
    EXPECT_DOUBLE_EQ(0, m_pReadAheadManager->getPlaypos());
    HintVector hints;
    m_pReadAheadManager->hintReader(1.0, &hints, kChannels);
    ASSERT_GE(hints.size(), 2);
    EXPECT_EQ(0, hints[0].frame);
    EXPECT_EQ(2, hints[0].frameCount);
    EXPECT_EQ(Hint::Type::CurrentPosition, hints[0].type);

    const auto retried = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, output.data(), output.size(), kChannels);
    EXPECT_EQ(8, retried.samplesRead);
    EXPECT_FALSE(retried.retryPending);
    EXPECT_DOUBLE_EQ(4, m_pReadAheadManager->getPlaypos());
    HintVector retryHints;
    m_pReadAheadManager->hintReader(1.0, &retryHints, kChannels);
    ASSERT_FALSE(retryHints.empty());
    EXPECT_EQ(2, retryHints[0].frame);
}

TEST_F(ReadAheadManagerTest, RetryHintsEntireLargePrimaryRead) {
    constexpr auto kChannels = mixxx::audio::ChannelCount::stereo();
    constexpr SINT kRequestFrames = 4 * CachingReaderChunk::kFrames;
    constexpr SINT kRequestSamples = kRequestFrames * kChannels;
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->pushReadResults({CachingReader::ReadResult::PARTIALLY_UNAVAILABLE,
            CachingReader::ReadResult::AVAILABLE});
    std::vector<CSAMPLE> output(kRequestSamples, -1.0f);

    const auto missed = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, output.data(), kRequestSamples, kChannels);
    EXPECT_EQ(0, missed.samplesRead);
    EXPECT_TRUE(missed.retryPending);
    EXPECT_DOUBLE_EQ(0, m_pReadAheadManager->getPlaypos());

    HintVector hints;
    m_pReadAheadManager->hintReader(1.0, &hints, kChannels);
    ASSERT_GE(hints.size(), 2);
    EXPECT_EQ(0, hints[0].frame);
    EXPECT_EQ(kRequestFrames, hints[0].frameCount);
    EXPECT_EQ(Hint::Type::CurrentPosition, hints[0].type);

    const auto retried = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0, output.data(), kRequestSamples, kChannels);
    EXPECT_EQ(kRequestSamples, retried.samplesRead);
    EXPECT_FALSE(retried.retryPending);
    EXPECT_EQ(kRequestSamples, m_pReadAheadManager->getPlaypos());
    ASSERT_EQ(2, m_pReader->readSizes().size());
    EXPECT_EQ(kRequestSamples, m_pReader->readSizes()[0]);
    EXPECT_EQ(kRequestSamples, m_pReader->readSizes()[1]);
}

TEST_F(ReadAheadManagerTest, ReverseRetryHintsEntireLargePrimaryRead) {
    constexpr auto kChannels = mixxx::audio::ChannelCount::stereo();
    constexpr SINT kRequestFrames = 4 * CachingReaderChunk::kFrames;
    constexpr SINT kRequestSamples = kRequestFrames * kChannels;
    constexpr SINT kCurrentFrame = 6 * CachingReaderChunk::kFrames;
    m_pReadAheadManager->notifySeek(kCurrentFrame * kChannels);
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->pushReadResults({CachingReader::ReadResult::PARTIALLY_UNAVAILABLE,
            CachingReader::ReadResult::AVAILABLE});
    std::vector<CSAMPLE> output(kRequestSamples, -1.0f);

    const auto missed = m_pReadAheadManager->getNextSamplesWithRetry(
            -1.0, output.data(), kRequestSamples, kChannels);
    EXPECT_EQ(0, missed.samplesRead);
    EXPECT_TRUE(missed.retryPending);
    EXPECT_EQ(kCurrentFrame * kChannels, m_pReadAheadManager->getPlaypos());

    HintVector hints;
    m_pReadAheadManager->hintReader(-1.0, &hints, kChannels);
    ASSERT_GE(hints.size(), 2);
    EXPECT_EQ(kCurrentFrame - kRequestFrames, hints[0].frame);
    EXPECT_EQ(kRequestFrames, hints[0].frameCount);
    EXPECT_EQ(Hint::Type::CurrentPosition, hints[0].type);

    const auto retried = m_pReadAheadManager->getNextSamplesWithRetry(
            -1.0, output.data(), kRequestSamples, kChannels);
    EXPECT_EQ(kRequestSamples, retried.samplesRead);
    EXPECT_FALSE(retried.retryPending);
    EXPECT_EQ((kCurrentFrame - kRequestFrames) * kChannels,
            m_pReadAheadManager->getPlaypos());
    ASSERT_EQ(2, m_pReader->readStarts().size());
    EXPECT_EQ(kCurrentFrame * kChannels, m_pReader->readStarts()[0]);
    EXPECT_EQ(kCurrentFrame * kChannels, m_pReader->readStarts()[1]);
}

TEST_F(ReadAheadManagerTest, ReverseLoopRetryClipsCrossfadeAtTrackEnd) {
    constexpr auto kChannels = mixxx::audio::ChannelCount::stem();
    constexpr SINT kCurrentFrame = 12;
    constexpr SINT kTriggerFrame = 8;
    constexpr SINT kTargetFrame = 199;
    constexpr SINT kTrackEndFrame = 200;
    constexpr SINT kRequestFrames = 8;
    constexpr SINT kRequestSamples = kRequestFrames * kChannels;
    constexpr SINT kReadSamples = (kCurrentFrame - kTriggerFrame) * kChannels;

    m_pLoopControl->setFrameInfo(mixxx::audio::kStartFramePos,
            mixxx::audio::FramePos::fromSamplePos(
                    kTrackEndFrame * kChannels, kChannels),
            mixxx::audio::SampleRate(48000));
    m_pReadAheadManager->notifySeek(kCurrentFrame * kChannels);
    m_pLoopControl->pushFrameValues(
            mixxx::audio::FramePos::fromSamplePos(
                    kTriggerFrame * kChannels, kChannels),
            mixxx::audio::FramePos::fromSamplePos(
                    kTargetFrame * kChannels, kChannels));
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    m_pReader->pushReadResults({CachingReader::ReadResult::AVAILABLE,
            CachingReader::ReadResult::PARTIALLY_UNAVAILABLE,
            CachingReader::ReadResult::AVAILABLE,
            CachingReader::ReadResult::AVAILABLE});
    std::vector<CSAMPLE> output(kRequestSamples, -1.0f);

    const auto missed = m_pReadAheadManager->getNextSamplesWithRetry(
            -1.0, output.data(), kRequestSamples, kChannels);
    EXPECT_EQ(0, missed.samplesRead);
    EXPECT_TRUE(missed.retryPending);
    EXPECT_EQ(kCurrentFrame * kChannels, m_pReadAheadManager->getPlaypos());

    HintVector hints;
    m_pReadAheadManager->hintReader(-1.0, &hints, kChannels);
    ASSERT_FALSE(hints.empty());
    EXPECT_EQ(kTargetFrame, hints[0].frame);
    EXPECT_EQ(1, hints[0].frameCount);
    EXPECT_EQ(Hint::Type::CurrentPosition, hints[0].type);

    const auto retried = m_pReadAheadManager->getNextSamplesWithRetry(
            -1.0, output.data(), kRequestSamples, kChannels);
    EXPECT_EQ(kReadSamples, retried.samplesRead);
    EXPECT_FALSE(retried.retryPending);
    EXPECT_EQ(kTargetFrame * kChannels, m_pReadAheadManager->getPlaypos());
    ASSERT_EQ(4, m_pReader->readStarts().size());
    EXPECT_EQ(kCurrentFrame * kChannels, m_pReader->readStarts()[0]);
    EXPECT_EQ(kTrackEndFrame * kChannels, m_pReader->readStarts()[1]);
    EXPECT_EQ(kCurrentFrame * kChannels, m_pReader->readStarts()[2]);
    EXPECT_EQ(kTrackEndFrame * kChannels, m_pReader->readStarts()[3]);
    EXPECT_EQ(kReadSamples, m_pReader->readSizes()[0]);
    EXPECT_EQ(kChannels, m_pReader->readSizes()[1]);
    EXPECT_EQ(kReadSamples, m_pReader->readSizes()[2]);
    EXPECT_EQ(kChannels, m_pReader->readSizes()[3]);
}

TEST_F(ReadAheadManagerTest, ReverseFractionalLoopPreservesOvershootDirection) {
    constexpr auto kChannels = mixxx::audio::ChannelCount::stereo();
    m_pReadAheadManager->notifySeek(30.5);
    m_pLoopControl->pushValues(20.2, 40.3);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);

    EXPECT_EQ(12,
            m_pReadAheadManager->getNextSamples(
                    -1.0, m_pBuffer, 32, kChannels));
    EXPECT_NEAR(38.6, m_pReadAheadManager->getPlaypos(), 0.01);
}

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
