#include "engine/controls/seek30control.h"

#include <gtest/gtest.h>

#include <atomic>
#include <initializer_list>
#include <thread>
#include <vector>

#include "test/signalpathtest.h"

namespace {
constexpr double kSampleRate = 44100.0;
constexpr double kTrackDurationSeconds = 10.0;

mixxx::audio::FramePos frameAtSeconds(double seconds) {
    return mixxx::audio::FramePos(seconds * kSampleRate);
}

int memoryCueCount(const TrackPointer& pTrack) {
    int count = 0;
    for (const auto& pCue : pTrack->getCuePoints()) {
        if (pCue && pCue->getType() == mixxx::CueType::Memory) {
            ++count;
        }
    }
    return count;
}
} // namespace

class Seek30ControlTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        m_pSeek30Control = m_pChannel1->getEngineBuffer()->m_pSeek30Control;
    }

    TrackPointer loadTrackWithCues(std::initializer_list<double> cueSeconds) {
        TrackPointer pTrack = m_pMixerDeck1->loadFakeTrack(false, 0.0);
        for (const double cueSecond : cueSeconds) {
            const auto position = frameAtSeconds(cueSecond);
            pTrack->createAndAddCue(
                    mixxx::CueType::Memory,
                    Cue::kNoHotCue,
                    position,
                    position);
        }

        // The fake loader publishes the borrowed Track* synchronously. Give
        // EngineBuffer one callback to consume that publication and rebuild
        // Seek30's audio-thread-only cache.
        ProcessBuffer();
        return pTrack;
    }

    void setCurrentPosition(double seconds) {
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(
                frameAtSeconds(seconds), EngineBuffer::SEEK_EXACT);
        // Allow the playposition control and EngineControl frame info to catch
        // up after the queued seek.
        for (int i = 0; i < 4; ++i) {
            ProcessBuffer();
        }
    }

    void processOnce() {
        ProcessBuffer();
    }

    Seek30Control* m_pSeek30Control{nullptr};
};

TEST_F(Seek30ControlTest, ProcessConsumesBorrowedTrackAndLatestSeek) {
    TrackPointer pTrack = loadTrackWithCues({2.0, 9.0});
    setCurrentPosition(8.0);

    // Both direct callbacks publish POD only. The previous request is the
    // latest value and therefore the one consumed by process().
    m_pSeek30Control->slotSeek30(1.0);
    m_pSeek30Control->slotSeek30Prev(1.0);
    processOnce();

    EXPECT_EQ(mixxx::audio::FramePos(frameAtSeconds(2.0)),
            m_pChannel1->getEngineBuffer()->queuedSeekPosition());
    EXPECT_EQ(2, memoryCueCount(pTrack));
}

TEST_F(Seek30ControlTest, StaleGenerationDropsCreateAndCoalescesClear) {
    TrackPointer pOldTrack = loadTrackWithCues({1.0});

    // Publish a command for the old generation, then replace the owner before
    // the audio path consumes it.
    m_pSeek30Control->createAtCurrent(1.0);
    m_pSeek30Control->clearAll(1.0);
    pOldTrack.reset();

    TrackPointer pNewTrack = m_pMixerDeck1->loadFakeTrack(false, 0.0);
    pNewTrack->createAndAddCue(
            mixxx::CueType::Memory,
            Cue::kNoHotCue,
            frameAtSeconds(4.0),
            frameAtSeconds(4.0));
    processOnce();

    EXPECT_EQ(1, memoryCueCount(pNewTrack));

    // Repeated clear requests collapse to one pending bit and still clear
    // every memory cue when they belong to the current generation.
    for (int i = 0; i < 64; ++i) {
        m_pSeek30Control->clearAll(1.0);
    }
    processOnce();
    EXPECT_EQ(0, memoryCueCount(pNewTrack));
}

TEST_F(Seek30ControlTest, ConcurrentScalarProducersAreConsumedOnProcessThread) {
    TrackPointer pTrack = loadTrackWithCues({1.0, 3.0, 5.0, 7.0});
    setCurrentPosition(1.0);

    constexpr int kProducerCount = 4;
    constexpr int kRequestsPerProducer = 128;
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([this, producer, &start]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int request = 0; request < kRequestsPerProducer; ++request) {
                switch ((producer + request) % 5) {
                case 0:
                    m_pSeek30Control->slotSeek30(1.0);
                    break;
                case 1:
                    m_pSeek30Control->slotSeek30Prev(1.0);
                    break;
                case 2:
                    m_pSeek30Control->createAtCurrent(1.0);
                    break;
                case 3:
                    m_pSeek30Control->clearAll(1.0);
                    break;
                case 4:
                    m_pSeek30Control->clearNearest(1.0);
                    break;
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (int callback = 0; callback < kRequestsPerProducer; ++callback) {
        processOnce();
    }
    for (auto& producer : producers) {
        producer.join();
    }
    processOnce();

    EXPECT_LE(memoryCueCount(pTrack), 4);
}
