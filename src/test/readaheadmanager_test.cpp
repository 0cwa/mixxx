#include "engine/readaheadmanager.h"

#include <gtest/gtest.h>

#include <QScopedPointer>
#include <QtDebug>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "control/controlobject.h"
#ifdef __BUNGEE__
#include "engine/bufferscalers/enginebufferscalebungee.h"
#endif
#include "engine/cachingreader/cachingreader.h"
#include "engine/cachingreader/cachingreaderchunk.h"
#include "engine/cachingreader/cachingreaderworker.h"
#include "engine/controls/cuecontrol.h"
#include "engine/controls/loopingcontrol.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "util/assert.h"
#include "util/defs.h"
#include "util/fifo.h"
#include "util/sample.h"

namespace {
const QString kGroup = "[test]";
} // namespace

class StubReader : public CachingReader {
  public:
    explicit StubReader(
            mixxx::audio::ChannelCount maxSupportedChannel =
                    mixxx::audio::ChannelCount::stereo())
            : CachingReader(kGroup, UserSettingsPointer(), maxSupportedChannel) {
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
        for (SINT i = 0; i < numSamples; ++i) {
            buffer[i] = static_cast<CSAMPLE>(startSample + i + 1);
        }
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
        for (SINT i = 0; i < numSamples; ++i) {
            buffer[i] = static_cast<CSAMPLE>(startSample + i + 1);
        }
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

class MaxStemRequestRetryReader final : public CachingReader {
  public:
    MaxStemRequestRetryReader()
            : CachingReader(
                      kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stem()) {
    }

  protected:
    RetryReadResult readWithRetryHook(SINT,
            SINT numSamples,
            bool,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount) override {
        SampleUtil::fill(buffer, 0.25f, numSamples);
        return {CachingReader::ReadResult::AVAILABLE, false};
    }
};

class CachingReaderStatusQueueTest : public ::testing::Test {
  protected:
    static int maxStatusUpdatesPerCallback() {
        return CachingReader::kMaxStatusUpdatesPerCallback;
    }

    static void enqueueTrackUnloadedUpdates(
            CachingReader& reader, int count) {
        auto update = ReaderStatusUpdate::trackUnloaded();
        for (int i = 0; i < count; ++i) {
            ASSERT_EQ(1, reader.m_readerStatusUpdateFIFO.write(&update, 1));
        }
    }

    static int pendingUpdates(const CachingReader& reader) {
        return reader.m_readerStatusUpdateFIFO.readAvailable();
    }

    static int consumedUpdates(const CachingReader& reader) {
        return reader.m_diagnosticStatusConsumed.loadAcquire();
    }

    static void processPendingStatusUpdates(CachingReader& reader) {
        reader.processPendingStatusUpdates();
    }
};

TEST_F(CachingReaderStatusQueueTest, ProcessLeavesUpdatesBeyondCallbackBudgetQueued) {
    CachingReader reader(
            kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo());
    const int budget = maxStatusUpdatesPerCallback();
    const int queued = budget * 2 + 1;
    enqueueTrackUnloadedUpdates(reader, queued);

    ASSERT_EQ(queued, pendingUpdates(reader));
    reader.process();
    EXPECT_EQ(queued - budget, pendingUpdates(reader));
    EXPECT_EQ(budget, consumedUpdates(reader));

    processPendingStatusUpdates(reader);
    EXPECT_EQ(queued - budget, pendingUpdates(reader));
    EXPECT_EQ(budget, consumedUpdates(reader));

    reader.process();
    EXPECT_EQ(1, pendingUpdates(reader));
    EXPECT_EQ(budget * 2, consumedUpdates(reader));
}

TEST_F(CachingReaderStatusQueueTest, ReadDoesNotResetCallbackBudget) {
    CachingReader reader(
            kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo());
    const int budget = maxStatusUpdatesPerCallback();
    reader.m_state.storeRelease(CachingReader::STATE_TRACK_LOADED);
    reader.m_readableFrameIndexRange = mixxx::IndexRange::forward(0, 1);
    for (int i = 0; i < budget * 2; ++i) {
        auto* const pChunk = reader.allocateChunk(i);
        ASSERT_NE(nullptr, pChunk);
        pChunk->giveToWorker();
        auto update = ReaderStatusUpdate();
        update.init(CHUNK_READ_DISCARDED, pChunk, mixxx::IndexRange());
        ASSERT_EQ(1, reader.m_readerStatusUpdateFIFO.write(&update, 1));
    }

    reader.process();
    std::array<CSAMPLE, 2> buffer{};
    EXPECT_EQ(CachingReader::ReadResult::UNAVAILABLE,
            reader.read(
                    0, buffer.size(), false, buffer.data(), mixxx::audio::ChannelCount::stereo()));
    EXPECT_EQ(budget, consumedUpdates(reader));
    EXPECT_EQ(budget, pendingUpdates(reader));
}

TEST_F(CachingReaderStatusQueueTest,
        ProcessDiscardsChunkResultWhileTrackIsUnloading) {
    CachingReader reader(
            kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo());
    reader.m_state.storeRelease(CachingReader::STATE_TRACK_UNLOADING);

    auto* const pChunk = reader.allocateChunk(0);
    ASSERT_NE(nullptr, pChunk);
    pChunk->giveToWorker();
    auto chunkUpdate = ReaderStatusUpdate();
    chunkUpdate.init(
            CHUNK_READ_SUCCESS, pChunk, mixxx::IndexRange::forward(0, 1));
    ASSERT_EQ(1, reader.m_readerStatusUpdateFIFO.write(&chunkUpdate, 1));

    auto unloadUpdate = ReaderStatusUpdate::trackUnloaded();
    ASSERT_EQ(1, reader.m_readerStatusUpdateFIFO.write(&unloadUpdate, 1));

    reader.process();

    EXPECT_EQ(CachingReader::STATE_IDLE, reader.m_state.loadAcquire());
    EXPECT_EQ(CachingReaderChunkForOwner::FREE, pChunk->getState());
    EXPECT_EQ(-1, pChunk->getIndex());
    EXPECT_EQ(nullptr, reader.lookupChunk(0));
    EXPECT_EQ(nullptr, reader.m_mruCachingReaderChunk);
    EXPECT_EQ(nullptr, reader.m_lruCachingReaderChunk);
}

TEST_F(CachingReaderStatusQueueTest,
        TeardownDrainReclaimsQueuedChunksWithoutStateTransitions) {
    CachingReader reader(
            kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo());
    auto* const pChunk = reader.m_chunks.front();
    pChunk->init(0);
    pChunk->giveToWorker();
    auto update = ReaderStatusUpdate();
    update.init(CHUNK_READ_SUCCESS, pChunk, mixxx::IndexRange::forward(0, 1));
    ASSERT_EQ(1, reader.m_readerStatusUpdateFIFO.write(&update, 1));

    reader.discardPendingStatusUpdates();
    EXPECT_EQ(0, pendingUpdates(reader));
    EXPECT_EQ(CachingReaderChunkForOwner::FREE, pChunk->getState());
    EXPECT_EQ(-1, pChunk->getIndex());
    EXPECT_EQ(CachingReader::STATE_IDLE, reader.m_state.loadAcquire());
}

class CachingReaderWorkerTest : public ::testing::Test {
  protected:
    static bool publishStatus(
            CachingReaderWorker& worker, const ReaderStatusUpdate& update) {
        return worker.publishStatus(update);
    }
};

TEST_F(CachingReaderWorkerTest,
        ShutdownPublicationFailureSuppressesLoadFailureSignal) {
    FIFO<CachingReaderChunkReadRequest> requestFIFO(1);
    FIFO<ReaderStatusUpdate> statusFIFO(1);
    auto queuedUpdate = ReaderStatusUpdate::trackUnloaded();
    ASSERT_EQ(1, statusFIFO.write(&queuedUpdate, 1));

    CachingReaderWorker worker(
            kGroup,
            &requestFIFO,
            &statusFIFO,
            mixxx::audio::ChannelCount::stereo());
    std::atomic<int> trackLoadingSignals{0};
    std::atomic<int> trackLoadFailedSignals{0};
    QObject::connect(
            &worker,
            &CachingReaderWorker::trackLoading,
            &worker,
            [&trackLoadingSignals] {
                trackLoadingSignals.fetch_add(1, std::memory_order_relaxed);
            },
            Qt::DirectConnection);
    QObject::connect(
            &worker,
            &CachingReaderWorker::trackLoadFailed,
            &worker,
            [&trackLoadFailedSignals](TrackPointer, const QString&) {
                trackLoadFailedSignals.fetch_add(1, std::memory_order_relaxed);
            },
            Qt::DirectConnection);
    const auto pTrack = Track::newTemporary(
            QStringLiteral("/definitely/missing/mixxx-test-file.wav"));
    std::atomic<bool> completed{false};
    std::atomic<bool> published{true};
    std::thread loader([&] {
#ifdef __STEM__
        published.store(
                worker.loadTrack(pTrack, mixxx::StemChannelSelection{}),
                std::memory_order_release);
#else
        published.store(worker.loadTrack(pTrack), std::memory_order_release);
#endif
        completed.store(true, std::memory_order_release);
    });

    const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool reachedPublishingStatus = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.diagnosticState() ==
                CachingReaderWorker::DiagnosticState::PublishingStatus) {
            reachedPublishingStatus = true;
            break;
        }
        std::this_thread::yield();
    }
    worker.quitWait();
    loader.join();

    ASSERT_TRUE(reachedPublishingStatus);
    EXPECT_TRUE(completed.load(std::memory_order_acquire));
    EXPECT_FALSE(published.load(std::memory_order_acquire));
    EXPECT_EQ(1, trackLoadingSignals.load(std::memory_order_relaxed));
    EXPECT_EQ(0, trackLoadFailedSignals.load(std::memory_order_relaxed));
}

TEST_F(CachingReaderWorkerTest,
        ShutdownReclaimsStatusChunkWhenStatusQueueIsFull) {
    FIFO<CachingReaderChunkReadRequest> requestFIFO(1);
    FIFO<ReaderStatusUpdate> statusFIFO(1);
    auto queuedUpdate = ReaderStatusUpdate::trackUnloaded();
    ASSERT_EQ(1, statusFIFO.write(&queuedUpdate, 1));

    CachingReaderWorker worker(
            kGroup,
            &requestFIFO,
            &statusFIFO,
            mixxx::audio::ChannelCount::stereo());
    mixxx::SampleBuffer chunkBuffer(1);
    CachingReaderChunkForOwner chunk{
            mixxx::SampleBuffer::WritableSlice(chunkBuffer)};
    chunk.init(0);
    chunk.giveToWorker();
    auto pendingUpdate = ReaderStatusUpdate();
    pendingUpdate.init(
            CHUNK_READ_SUCCESS, &chunk, mixxx::IndexRange::forward(0, 1));

    std::atomic<bool> published{true};
    std::thread publisher([&] {
        published.store(publishStatus(worker, pendingUpdate),
                std::memory_order_release);
    });
    worker.quitWait();
    publisher.join();

    EXPECT_FALSE(published.load(std::memory_order_acquire));
    EXPECT_EQ(CachingReaderChunkForOwner::READY, chunk.getState());
    ReaderStatusUpdate preservedUpdate;
    ASSERT_EQ(1, statusFIFO.read(&preservedUpdate, 1));
    EXPECT_EQ(TRACK_UNLOADED, preservedUpdate.status);
}

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

TEST(CachingReaderRetryTest, AcceptsMaximumStemRequest) {
    constexpr auto kChannelCount = mixxx::audio::ChannelCount::stem();
    constexpr SINT kSampleCount = CachingReaderChunk::frames2samples(
            static_cast<SINT>(MAX_BUFFER_LEN), kChannelCount);
    std::vector<CSAMPLE> buffer(kSampleCount, -1.0f);

    MaxStemRequestRetryReader reader;
    EXPECT_EQ(CachingReader::ReadResult::AVAILABLE,
            reader.readWithRetry(0,
                    kSampleCount,
                    false,
                    buffer.data(),
                    kChannelCount));
    EXPECT_TRUE(std::all_of(buffer.begin(), buffer.end(), [](CSAMPLE sample) {
        return sample == 0.25f;
    }));
}

TEST_F(CachingReaderStatusQueueTest, ProcessRecyclesChunkIntoFixedFreePool) {
    CachingReader reader(
            kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo());
    const int initialFreeChunkCount = reader.m_freeChunkCount;
    auto* const pChunk = reader.allocateChunk(0);
    ASSERT_NE(nullptr, pChunk);
    pChunk->giveToWorker();

    auto update = ReaderStatusUpdate();
    update.init(CHUNK_READ_INVALID, pChunk, mixxx::IndexRange());
    ASSERT_EQ(1, reader.m_readerStatusUpdateFIFO.write(&update, 1));
    reader.m_state.storeRelease(CachingReader::STATE_TRACK_LOADED);

    reader.process();

    EXPECT_EQ(initialFreeChunkCount, reader.m_freeChunkCount);
    EXPECT_EQ(CachingReaderChunkForOwner::FREE, pChunk->getState());
}

TEST_F(CachingReaderStatusQueueTest, RecyclesChunksAcrossFreePoolWraparound) {
    CachingReader reader(
            kGroup, UserSettingsPointer(), mixxx::audio::ChannelCount::stereo());
    reader.m_state.storeRelease(CachingReader::STATE_TRACK_LOADED);
    const int poolSize = static_cast<int>(reader.m_chunks.size());
    const int budget = maxStatusUpdatesPerCallback();
    const int queued = poolSize - 1;
    std::vector<CachingReaderChunkForOwner*> allocatedChunks;
    allocatedChunks.reserve(queued);

    for (int i = 0; i < queued; ++i) {
        auto* const pChunk = reader.allocateChunk(i);
        ASSERT_NE(nullptr, pChunk);
        pChunk->giveToWorker();
        auto update = ReaderStatusUpdate();
        update.init(CHUNK_READ_DISCARDED, pChunk, mixxx::IndexRange());
        ASSERT_EQ(1, reader.m_readerStatusUpdateFIFO.write(&update, 1));
        allocatedChunks.push_back(pChunk);
    }

    EXPECT_EQ(1, reader.m_freeChunkCount);
    EXPECT_EQ(poolSize - 1, reader.m_freeChunkStart);

    reader.process();
    EXPECT_EQ(queued - budget, pendingUpdates(reader));
    EXPECT_EQ(budget, consumedUpdates(reader));
    while (pendingUpdates(reader) > 0) {
        reader.process();
    }

    EXPECT_EQ(poolSize, reader.m_freeChunkCount);
    EXPECT_EQ(poolSize - 1, reader.m_freeChunkStart);

    auto* const firstFreeChunk = reader.m_freeChunks[reader.m_freeChunkStart];
    EXPECT_EQ(firstFreeChunk, reader.allocateChunk(poolSize));
    for (int i = 0; i < queued; ++i) {
        EXPECT_EQ(allocatedChunks[i], reader.allocateChunk(poolSize + i + 1));
    }

    reader.freeChunk(firstFreeChunk);
    for (auto* const pChunk : allocatedChunks) {
        reader.freeChunk(pChunk);
    }
    EXPECT_EQ(poolSize, reader.m_freeChunkCount);
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

    void pushSampleValues(double trigger,
            double target,
            mixxx::audio::ChannelCount channelCount) {
        m_triggerReturnValues.push_back(
                mixxx::audio::FramePos::fromSamplePos(trigger, channelCount));
        m_targetReturnValues.push_back(
                mixxx::audio::FramePos::fromSamplePos(target, channelCount));
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

TEST_F(ReadAheadManagerTest, CrossfadeHandlesMaximumInterleavedRequest) {
    constexpr auto kChannelCount = mixxx::audio::ChannelCount::stem();
    constexpr SINT kRequestSamples =
            static_cast<SINT>(MAX_BUFFER_LEN) * kChannelCount;
    constexpr SINT kCrossFadeSamples = kRequestSamples - kChannelCount;
    constexpr SINT kTargetSample = kRequestSamples * 2;
    constexpr SINT kTrackSamples = kRequestSamples * 3;
    constexpr SINT kCrossFadeReadPosition = kRequestSamples + kChannelCount;
    constexpr CSAMPLE kSentinel = -1.0f;

    static_assert(kRequestSamples % kChannelCount == 0);
    std::vector<CSAMPLE> output(kRequestSamples, kSentinel);

    // Use the same maximum channel contract as a production stem reader.
    m_pReader.reset(new StubReader(kChannelCount));
    m_pReadAheadManager.reset(new ReadAheadManager(m_pReader.data(),
            m_pLoopControl.data(),
            m_pCueControl.data()));
    m_pLoopControl->setFrameInfo(mixxx::audio::kStartFramePos,
            mixxx::audio::FramePos::fromSamplePos(kTrackSamples, kChannelCount),
            mixxx::audio::SampleRate(44100));
    m_pReadAheadManager->notifySeek(0);
    m_pLoopControl->pushSampleValues(kCrossFadeSamples,
            kTargetSample,
            kChannelCount);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);

    EXPECT_EQ(kCrossFadeSamples,
            m_pReadAheadManager->getNextSamples(
                    1.0, output.data(), kRequestSamples, kChannelCount));

    ASSERT_EQ(2, m_pReader->readStartSamples().size());
    EXPECT_EQ(0, m_pReader->readStartSamples()[0]);
    EXPECT_EQ(kCrossFadeReadPosition, m_pReader->readStartSamples()[1]);

    const SINT crossFadeFrames = kCrossFadeSamples / kChannelCount;
    const CSAMPLE_GAIN crossIncrement =
            CSAMPLE_GAIN_ONE / CSAMPLE_GAIN(crossFadeFrames);
    for (SINT frame = 0; frame < crossFadeFrames; ++frame) {
        const CSAMPLE_GAIN crossMix = crossIncrement * frame;
        for (SINT channel = 0; channel < kChannelCount; ++channel) {
            const SINT sample = frame * kChannelCount + channel;
            const CSAMPLE expected =
                    static_cast<CSAMPLE>(sample + 1) *
                            (CSAMPLE_GAIN_ONE - crossMix) +
                    static_cast<CSAMPLE>(kCrossFadeReadPosition + sample + 1) *
                            crossMix;
            EXPECT_FLOAT_EQ(expected, output[sample]);
        }
    }
    EXPECT_TRUE(std::all_of(output.begin() + kCrossFadeSamples,
            output.end(),
            [](CSAMPLE sample) { return sample == kSentinel; }));
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
    ReadAheadManager::RetryState scalerRetryState;
    ReadAheadManager::RetryState inactiveScalerRetryState;

    const auto unavailableResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0,
            output.data(),
            output.size(),
            mixxx::audio::ChannelCount::stereo(),
            scalerRetryState);
    EXPECT_EQ(0, unavailableResult.samplesRead);
    EXPECT_TRUE(unavailableResult.retryPending);
    EXPECT_TRUE(scalerRetryState.active);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
        return sample == 0.0f;
    }));
    EXPECT_EQ(1, m_pLoopControl->queryCount());
    EXPECT_EQ(1, m_pCueControl->queryCount());

    // Resetting an inactive scaler must not cancel the active scaler's
    // stateful retry plan when both scalers share this manager.
    inactiveScalerRetryState.active = true;
    m_pReadAheadManager->cancelPendingRetry(inactiveScalerRetryState);
    EXPECT_FALSE(inactiveScalerRetryState.active);
    EXPECT_TRUE(scalerRetryState.active);

    m_pReader->setReadAvailable(true);
    const auto availableResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0,
            output.data(),
            output.size(),
            mixxx::audio::ChannelCount::stereo(),
            scalerRetryState);

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

TEST_F(ReadAheadManagerTest, ReadAheadLogPreservesLongAlternatingContinuity) {
    constexpr int kSegments = 512;
    constexpr SINT kSamplesPerSegment = 10;
    m_pReadAheadManager->notifySeek(0);
    for (int i = 0; i < kSegments; ++i) {
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    }

    for (int i = 0; i < kSegments; ++i) {
        const double rate = i % 2 == 0 ? 1.0 : -1.0;
        EXPECT_EQ(kSamplesPerSegment,
                m_pReadAheadManager->getNextSamples(
                        rate,
                        m_pBuffer,
                        kSamplesPerSegment,
                        mixxx::audio::ChannelCount::stereo()));
    }

    for (int i = 0; i < kSegments; ++i) {
        const double expectedPosition = i % 2 == 0 ? 10.0 : 0.0;
        EXPECT_DOUBLE_EQ(expectedPosition,
                m_pReadAheadManager->getFilePlaypositionFromLog(
                        -1.0, kSamplesPerSegment));
    }
    EXPECT_DOUBLE_EQ(0.0, m_pReadAheadManager->getPlaypos());
}

TEST_F(ReadAheadManagerTest, ReadAheadLogOverflowRecoversAfterDualOccupancy) {
    constexpr SINT kSamplesPerSegment = 10;
    constexpr int kSegments =
            static_cast<int>(ReadAheadManager::kMaxReadAheadLogEntries) + 2;
    m_pReadAheadManager->notifySeek(0);
    for (int i = 0; i < kSegments; ++i) {
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    }

    for (int i = 0; i < kSegments; ++i) {
        SampleUtil::fill(m_pBuffer, 1.0f, kSamplesPerSegment);
        const auto samplesRead = m_pReadAheadManager->getNextSamples(
                i % 2 == 0 ? 1.0 : -1.0,
                m_pBuffer,
                kSamplesPerSegment,
                mixxx::audio::ChannelCount::stereo());
        EXPECT_EQ(kSamplesPerSegment, samplesRead);
    }

    EXPECT_EQ(kSegments,
            m_pReader->readStartSamples().size());
    EXPECT_DOUBLE_EQ(0.0, m_pReadAheadManager->getPlaypos());

    // The main log and both spill entries are occupied here. A third
    // non-contiguous request is a bounded empty read, not a cache retry: no
    // reader call or cursor movement is allowed, and the destination must not
    // retain stale samples.
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    SampleUtil::fill(m_pBuffer, -1.0f, kSamplesPerSegment);
    const auto blockedResult = m_pReadAheadManager->getNextSamplesWithRetry(
            1.0,
            m_pBuffer,
            kSamplesPerSegment,
            mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(0, blockedResult.samplesRead);
    EXPECT_FALSE(blockedResult.retryPending);
    EXPECT_EQ(kSegments, m_pReader->readStartSamples().size());
    EXPECT_DOUBLE_EQ(0.0, m_pReadAheadManager->getPlaypos());
    for (SINT sample = 0; sample < kSamplesPerSegment; ++sample) {
        EXPECT_FLOAT_EQ(0.0f, m_pBuffer[sample]);
    }

    // Repeating the same full-queue request must remain bounded and must not
    // turn the capacity condition into a permanent caller-owned retry.
    m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
    m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    SampleUtil::fill(m_pBuffer, -1.0f, kSamplesPerSegment);
    const auto repeatedBlockedResult =
            m_pReadAheadManager->getNextSamplesWithRetry(
                    1.0,
                    m_pBuffer,
                    kSamplesPerSegment,
                    mixxx::audio::ChannelCount::stereo());
    EXPECT_EQ(0, repeatedBlockedResult.samplesRead);
    EXPECT_FALSE(repeatedBlockedResult.retryPending);
    EXPECT_EQ(kSegments, m_pReader->readStartSamples().size());
    EXPECT_TRUE(std::all_of(m_pBuffer,
            m_pBuffer + kSamplesPerSegment,
            [](CSAMPLE sample) { return sample == 0.0f; }));

    // This is the same public position-consumption path EngineBuffer uses
    // after a positive output buffer; it releases one mapping without any
    // test-only queue manipulation. Each following third read must recover,
    // preserve the alternating source order, and write fresh reader data.
    double filePlayposition = -1.0;
    for (int i = 0; i < 4; ++i) {
        filePlayposition = m_pReadAheadManager->getFilePlaypositionFromLog(
                filePlayposition, kSamplesPerSegment);
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
        const double rate = i % 2 == 0 ? 1.0 : -1.0;
        SampleUtil::fill(m_pBuffer, -1.0f, kSamplesPerSegment);
        const auto recoveredResult =
                m_pReadAheadManager->getNextSamplesWithRetry(
                        rate,
                        m_pBuffer,
                        kSamplesPerSegment,
                        mixxx::audio::ChannelCount::stereo());
        EXPECT_EQ(kSamplesPerSegment, recoveredResult.samplesRead);
        EXPECT_FALSE(recoveredResult.retryPending);
        EXPECT_EQ(kSegments + i + 1,
                m_pReader->readStartSamples().size());
        const double expectedFilePlayposition =
                i % 2 == 0 ? kSamplesPerSegment : 0.0;
        EXPECT_DOUBLE_EQ(expectedFilePlayposition, filePlayposition);
        EXPECT_DOUBLE_EQ(expectedFilePlayposition,
                m_pReadAheadManager->getPlaypos());

        const SINT expectedReadStartSample =
                i % 2 == 0 ? 0 : kSamplesPerSegment;
        EXPECT_EQ(expectedReadStartSample,
                m_pReader->readStartSamples().back());
        for (SINT sample = 0; sample < kSamplesPerSegment; ++sample) {
            EXPECT_FLOAT_EQ(
                    static_cast<CSAMPLE>(expectedReadStartSample + sample + 1),
                    m_pBuffer[sample]);
        }
    }
}

#ifdef __BUNGEE__
TEST_F(ReadAheadManagerTest, ReadAheadLogOverflowRecoversThroughBungeeConsumer) {
    constexpr SINT kSamplesPerSegment = 10;
    constexpr int kSegments =
            static_cast<int>(ReadAheadManager::kMaxReadAheadLogEntries) + 2;
    constexpr SINT kOutputFrames = 1024;
    constexpr auto kChannelCount = mixxx::audio::ChannelCount::stereo();

    m_pReadAheadManager->notifySeek(0);
    for (int i = 0; i < kSegments; ++i) {
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    }

    for (int i = 0; i < kSegments; ++i) {
        EXPECT_EQ(kSamplesPerSegment,
                m_pReadAheadManager->getNextSamples(
                        i % 2 == 0 ? 1.0 : -1.0,
                        m_pBuffer,
                        kSamplesPerSegment,
                        kChannelCount));
    }
    ASSERT_EQ(kSegments, m_pReader->readStartSamples().size());

    // Keep the controls available for the real scaler calls below. The
    // scaler may need more than one grain to fill an output callback.
    for (int i = 0; i < 128; ++i) {
        m_pLoopControl->pushValues(kNoTrigger, kNoTrigger);
        m_pCueControl->pushValues(kNoTrigger, kNoTrigger);
    }

    EngineBufferScaleBungee scaler(m_pReadAheadManager.data());
    scaler.setSignal(mixxx::audio::SampleRate(44100), kChannelCount);
    double tempoRatio = 1.0;
    double pitchRatio = 1.0;
    scaler.setScaleParameters(1.0, &tempoRatio, &pitchRatio);

    std::array<CSAMPLE, kOutputFrames * 2> output;
    mixxx::audio::FramePos filePosition =
            mixxx::audio::FramePos::fromSamplePos(0, kChannelCount);
    const int readCallsBeforeCapacityRecovery =
            m_pReader->readStartSamples().size();

    auto renderAndAccount = [&]() {
        SampleUtil::fill(output.data(), -1.0f, static_cast<SINT>(output.size()));
        const double framesRead = scaler.scaleBuffer(
                output.data(), static_cast<SINT>(output.size()));
        EXPECT_GT(framesRead, 0.0);
        EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
            return sample != -1.0f;
        }));

        // This is the exact public accounting call made by EngineBuffer. It
        // consumes the source mappings represented by the positive scaler
        // result and, in turn, frees capacity for the next read.
        filePosition = m_pReadAheadManager->getFilePlaypositionFromLog(
                filePosition, framesRead, kChannelCount);
        return framesRead;
    };

    renderAndAccount();
    EXPECT_EQ(readCallsBeforeCapacityRecovery,
            m_pReader->readStartSamples().size());

    renderAndAccount();
    EXPECT_GT(m_pReader->readStartSamples().size(),
            readCallsBeforeCapacityRecovery);
}
#endif

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
