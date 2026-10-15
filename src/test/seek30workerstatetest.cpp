#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <thread>

#include "engine/controls/seek30workerstate.h"
#include "track/track.h"

namespace {
constexpr double kSampleRate = 44100.0;

mixxx::audio::FramePos frameAtSeconds(double seconds) {
    return mixxx::audio::FramePos(seconds * kSampleRate);
}

mixxx::audio::FramePos frameAtFrames(double frames) {
    return mixxx::audio::FramePos(frames);
}

TrackPointer trackWithMemoryCues(std::initializer_list<double> cueSeconds) {
    TrackPointer pTrack = Track::newTemporary();
    for (const double cueSecond : cueSeconds) {
        const auto position = frameAtSeconds(cueSecond);
        pTrack->createAndAddCue(
                mixxx::CueType::Memory,
                Cue::kNoHotCue,
                position,
                position);
    }
    return pTrack;
}

int memoryCueCount(const TrackPointer& pTrack) {
    int count = 0;
    const QList<CuePointer> cuePoints = pTrack->getCuePoints();
    for (const auto& pCue : cuePoints) {
        if (pCue && pCue->getType() == mixxx::CueType::Memory) {
            ++count;
        }
    }
    return count;
}

CuePointer memoryCueAt(
        const TrackPointer& pTrack,
        const mixxx::audio::FramePos& position) {
    const QList<CuePointer> cuePoints = pTrack->getCuePoints();
    for (const auto& pCue : cuePoints) {
        if (pCue && pCue->getType() == mixxx::CueType::Memory &&
                pCue->getStartAndEndPosition().startPosition == position) {
            return pCue;
        }
    }
    return {};
}

bool hasMemoryCueAt(const TrackPointer& pTrack, const mixxx::audio::FramePos& position) {
    const QList<CuePointer> cuePoints = pTrack->getCuePoints();
    for (const auto& pCue : cuePoints) {
        if (pCue && pCue->getType() == mixxx::CueType::Memory &&
                pCue->getStartAndEndPosition().startPosition == position) {
            return true;
        }
    }
    return false;
}

Seek30WorkerInputs inputsAt(double seconds) {
    return {
            frameAtSeconds(seconds),
            mixxx::audio::SampleRate::fromDouble(kSampleRate),
            false,
            {},
            false};
}
} // namespace

TEST(Seek30CommandMailboxTest, PreservesMultiplicityAndDefinesOverflow) {
    Seek30CommandMailbox mailbox;
    constexpr auto operation = Seek30Operation::ClearPrevious;
    constexpr std::uint64_t generation = 7;

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(mailbox.tryPush({operation, generation}));
    }

    Seek30Command command{};
    int popped = 0;
    while (mailbox.tryPop(&command)) {
        EXPECT_EQ(operation, command.operation);
        EXPECT_EQ(generation, command.generation);
        ++popped;
    }
    EXPECT_EQ(5, popped);

    for (std::size_t i = 0; i < Seek30CommandMailbox::kCapacity; ++i) {
        EXPECT_TRUE(mailbox.tryPush({Seek30Operation::CreateAtCurrent, generation}));
    }
    EXPECT_FALSE(mailbox.tryPush({Seek30Operation::CreateAtCurrent, generation}));
}

TEST(Seek30CommandMailboxTest, SupportsConcurrentProducers) {
    Seek30CommandMailbox mailbox;
    constexpr int kProducerCount = 4;
    constexpr int kCommandsPerProducer = 32;
    std::array<std::atomic<int>, 8> expected{};
    for (auto& count : expected) {
        count.store(0, std::memory_order_relaxed);
    }

    std::atomic<bool> start{false};
    std::atomic<bool> pushesSucceeded{true};
    std::array<std::thread, kProducerCount> producers;
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers[producer] = std::thread(
                [&mailbox, &expected, &start, &pushesSucceeded, producer]() {
                    while (!start.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    for (int index = 0; index < kCommandsPerProducer; ++index) {
                        const auto operation = static_cast<Seek30Operation>(
                                (producer + index) % expected.size());
                        if (!mailbox.tryPush({operation, 11})) {
                            pushesSucceeded.store(
                                    false, std::memory_order_relaxed);
                            return;
                        }
                        expected[static_cast<std::size_t>(operation)].fetch_add(
                                1, std::memory_order_relaxed);
                    }
                });
    }

    start.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        producer.join();
    }
    ASSERT_TRUE(pushesSucceeded.load(std::memory_order_relaxed));

    std::array<int, 8> actual{};
    Seek30Command command{};
    while (mailbox.tryPop(&command)) {
        ++actual[static_cast<std::size_t>(command.operation)];
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        EXPECT_EQ(expected[index].load(std::memory_order_relaxed), actual[index]);
    }
}

TEST(Seek30WorkerStateTest, RepeatedCommandsAndQuantizedDuplicateCreation) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 2.0, 3.0});
    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    auto inputs = inputsAt(0.0);
    auto firstTarget = state.process({Seek30Operation::SeekNext, generation}, inputs);
    ASSERT_TRUE(firstTarget.position.isValid());
    EXPECT_EQ(frameAtSeconds(1.0), firstTarget.position);
    inputs.currentPosition = firstTarget.position;
    const auto secondTarget = state.process(
            {Seek30Operation::SeekNext, generation}, inputs);
    EXPECT_EQ(frameAtSeconds(2.0), secondTarget.position);

    inputs = inputsAt(4.0);
    state.process({Seek30Operation::ClearPrevious, generation}, inputs);
    state.process({Seek30Operation::ClearPrevious, generation}, inputs);
    EXPECT_EQ(1, memoryCueCount(pTrack));

    inputs.currentPosition = frameAtSeconds(1.25);
    inputs.quantize = true;
    inputs.closestBeat = frameAtSeconds(8.0);
    state.process({Seek30Operation::CreateAtCurrent, generation}, inputs);
    state.process({Seek30Operation::CreateAtCurrent, generation}, inputs);
    EXPECT_EQ(2, memoryCueCount(pTrack));
}

TEST(Seek30WorkerStateTest, RefreshesExternalMemoryCueAdditionWithoutTrackReload) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 3.0});
    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    const auto initialTarget = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    ASSERT_TRUE(initialTarget.position.isValid());
    EXPECT_EQ(frameAtSeconds(1.0), initialTarget.position);

    const auto addedPosition = frameAtSeconds(2.0);
    pTrack->createAndAddCue(
            mixxx::CueType::Memory,
            Cue::kNoHotCue,
            addedPosition,
            addedPosition);

    const auto target = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(1.5));
    EXPECT_EQ(generation, state.generation());
    EXPECT_EQ(addedPosition, target.position);
}

TEST(Seek30WorkerStateTest, RefreshesExternalMemoryCueRemovalWithoutTrackReload) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 3.0});
    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    const auto pRemovedCue = memoryCueAt(pTrack, frameAtSeconds(1.0));
    ASSERT_TRUE(pRemovedCue);
    pTrack->removeCue(pRemovedCue);

    const auto target = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    EXPECT_EQ(generation, state.generation());
    EXPECT_EQ(frameAtSeconds(3.0), target.position);
}

TEST(Seek30WorkerStateTest, RefreshesQueuedCommandAfterExternalCueEdit) {
    Seek30CommandMailbox mailbox;
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 3.0});

    std::mutex barrierMutex;
    std::condition_variable barrier;
    bool statePrimed = false;
    bool allowDispatch = false;
    std::uint64_t generation = 0;
    std::uint64_t generationAfterDispatch = 0;
    Seek30SeekTarget primingTarget;
    Seek30SeekTarget publishedTarget;
    std::uint64_t publishedTargetCount = 0;
    std::thread::id readerWorkerThread;
    std::thread::id publishThread;

    // Warm the worker-owned cache, then hold the reader worker before mailbox
    // drain while an external edit changes the next cue.
    std::thread readerWorker([&] {
        state.trackLoaded(pTrack);
        const auto target = state.process(
                {Seek30Operation::SeekNext, state.generation()}, inputsAt(0.0));
        {
            std::lock_guard lock(barrierMutex);
            generation = state.generation();
            primingTarget = target;
            readerWorkerThread = std::this_thread::get_id();
            statePrimed = true;
        }
        barrier.notify_one();

        {
            std::unique_lock lock(barrierMutex);
            barrier.wait(lock, [&] { return allowDispatch; });
        }

        std::uint64_t targetSequence = 0;
        processSeek30CommandMailbox(
                mailbox,
                state,
                targetSequence,
                [] { return inputsAt(0.0); },
                [&](const Seek30SeekTarget& queuedTarget) {
                    std::lock_guard lock(barrierMutex);
                    publishedTarget = queuedTarget;
                    ++publishedTargetCount;
                    publishThread = std::this_thread::get_id();
                });

        generationAfterDispatch = state.generation();
    });

    bool workerPrimed = false;
    {
        std::unique_lock lock(barrierMutex);
        workerPrimed = barrier.wait_for(
                lock,
                std::chrono::seconds(2),
                [&] { return statePrimed; });
    }

    bool commandWaitedForEdit = false;
    bool commandQueued = false;
    bool cueRemoved = false;
    if (workerPrimed) {
        {
            std::lock_guard lock(barrierMutex);
            commandWaitedForEdit = !allowDispatch && publishedTargetCount == 0;
        }
        if (commandWaitedForEdit) {
            commandQueued = mailbox.tryPush(
                    {Seek30Operation::SeekNext, generation});
        }
        if (commandQueued) {
            const auto pRemovedCue = memoryCueAt(pTrack, frameAtSeconds(1.0));
            if (pRemovedCue) {
                pTrack->removeCue(pRemovedCue);
                cueRemoved = true;
            }
        }
    }

    {
        std::lock_guard lock(barrierMutex);
        allowDispatch = true;
    }
    barrier.notify_one();
    readerWorker.join();

    EXPECT_TRUE(workerPrimed);
    EXPECT_TRUE(commandWaitedForEdit);
    EXPECT_TRUE(commandQueued);
    EXPECT_TRUE(cueRemoved);
    EXPECT_EQ(frameAtSeconds(1.0), primingTarget.position);
    EXPECT_EQ(generation, primingTarget.generation);
    EXPECT_EQ(generation, generationAfterDispatch);
    EXPECT_EQ(1, memoryCueCount(pTrack));
    EXPECT_EQ(1u, publishedTargetCount);
    EXPECT_EQ(frameAtSeconds(3.0), publishedTarget.position);
    EXPECT_EQ(generation, publishedTarget.generation);
    EXPECT_EQ(1u, publishedTarget.sequence);
    EXPECT_EQ(readerWorkerThread, publishThread);

    Seek30Command leftoverCommand{};
    EXPECT_FALSE(mailbox.tryPop(&leftoverCommand));
}

TEST(Seek30WorkerStateTest, RefreshesExternalMemoryCueMoveAndOrdering) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 3.0, 5.0});
    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    const auto pMovedCue = memoryCueAt(pTrack, frameAtSeconds(1.0));
    ASSERT_TRUE(pMovedCue);
    pMovedCue->setStartAndEndPosition(
            frameAtSeconds(4.0), frameAtSeconds(4.0));

    auto inputs = inputsAt(0.0);
    const auto firstTarget = state.process(
            {Seek30Operation::SeekNext, generation}, inputs);
    EXPECT_EQ(frameAtSeconds(3.0), firstTarget.position);

    inputs.currentPosition = frameAtSeconds(3.5);
    const auto movedTarget = state.process(
            {Seek30Operation::SeekNext, generation}, inputs);
    EXPECT_EQ(generation, state.generation());
    EXPECT_EQ(frameAtSeconds(4.0), movedTarget.position);
}

TEST(Seek30WorkerStateTest, RefreshesMemoryCueTypeChangeToLoopWithoutTrackReload) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 3.0});
    const auto pCue = memoryCueAt(pTrack, frameAtSeconds(1.0));
    ASSERT_TRUE(pCue);
    pCue->setEndPosition(frameAtSeconds(2.0));

    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    const auto initialTarget = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    ASSERT_EQ(frameAtSeconds(1.0), initialTarget.position);

    pCue->setType(mixxx::CueType::Loop);
    const auto withoutMemoryCue = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    EXPECT_EQ(generation, state.generation());
    EXPECT_EQ(frameAtSeconds(3.0), withoutMemoryCue.position);
}

TEST(Seek30WorkerStateTest, RefreshesLoopCueTypeChangeToMemoryWithoutTrackReload) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 3.0});
    const auto pCue = memoryCueAt(pTrack, frameAtSeconds(1.0));
    ASSERT_TRUE(pCue);
    pCue->setEndPosition(frameAtSeconds(2.0));
    pCue->setType(mixxx::CueType::Loop);

    state.trackLoaded(pTrack);
    const auto generation = state.generation();
    const auto initialTarget = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    ASSERT_EQ(frameAtSeconds(3.0), initialTarget.position);

    pCue->setType(mixxx::CueType::Memory);
    const auto withMemoryCueAgain = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    EXPECT_EQ(generation, state.generation());
    EXPECT_EQ(frameAtSeconds(1.0), withMemoryCueAgain.position);
}

TEST(Seek30WorkerStateTest, CapturesStableCueViewDuringExternalEdits) {
    const auto pTrack = trackWithMemoryCues({1.0, 3.0});
    const auto pMovedCue = memoryCueAt(pTrack, frameAtSeconds(1.0));
    ASSERT_TRUE(pMovedCue);

    const auto memoryCues =
            snapshotSeek30MemoryCues(pTrack->getCuePoints());
    ASSERT_EQ(2, memoryCues.size());
    EXPECT_EQ(pMovedCue, memoryCues.at(0).pCue);
    EXPECT_EQ(mixxx::CueType::Memory, memoryCues.at(0).type);
    EXPECT_EQ(frameAtSeconds(1.0), memoryCues.at(0).startPosition);
    EXPECT_EQ(frameAtSeconds(3.0), memoryCues.at(1).startPosition);

    pMovedCue->setStartAndEndPosition(
            frameAtSeconds(5.0), frameAtSeconds(5.0));
    pMovedCue->setType(mixxx::CueType::Loop);

    EXPECT_EQ(mixxx::CueType::Memory, memoryCues.at(0).type);
    EXPECT_EQ(frameAtSeconds(1.0), memoryCues.at(0).startPosition);
    EXPECT_EQ(frameAtSeconds(3.0), memoryCues.at(1).startPosition);
    EXPECT_EQ(mixxx::CueType::Loop, pMovedCue->getType());
    EXPECT_EQ(frameAtSeconds(5.0),
            pMovedCue->getStartAndEndPosition().startPosition);
}

TEST(Seek30WorkerStateTest, RefreshesExternalMemoryCueInvalidationWithoutTrackReload) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 3.0, 5.0});
    const auto pCue = memoryCueAt(pTrack, frameAtSeconds(1.0));
    ASSERT_TRUE(pCue);

    pCue->setStartPosition(mixxx::audio::kInvalidFramePos);
    const auto invalidPositions = pCue->getStartAndEndPosition();
    ASSERT_FALSE(invalidPositions.startPosition.isValid());
    ASSERT_TRUE(invalidPositions.endPosition.isValid());
    const auto initialMemoryCues =
            snapshotSeek30MemoryCues(pTrack->getCuePoints());
    ASSERT_EQ(2, initialMemoryCues.size());
    EXPECT_EQ(frameAtSeconds(3.0), initialMemoryCues.at(0).startPosition);
    EXPECT_EQ(frameAtSeconds(5.0), initialMemoryCues.at(1).startPosition);

    state.trackLoaded(pTrack);
    const auto generation = state.generation();
    const auto withoutInvalidStart = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    ASSERT_EQ(frameAtSeconds(3.0), withoutInvalidStart.position);

    pCue->setStartPosition(frameAtSeconds(1.0));
    const auto withValidStart = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    ASSERT_EQ(frameAtSeconds(1.0), withValidStart.position);

    pCue->setStartPosition(mixxx::audio::kInvalidFramePos);
    const auto memoryCuesAfterInvalidation =
            snapshotSeek30MemoryCues(pTrack->getCuePoints());
    ASSERT_EQ(2, memoryCuesAfterInvalidation.size());
    EXPECT_EQ(frameAtSeconds(3.0),
            memoryCuesAfterInvalidation.at(0).startPosition);
    EXPECT_EQ(frameAtSeconds(5.0),
            memoryCuesAfterInvalidation.at(1).startPosition);
    const auto afterExternalInvalidation = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    ASSERT_EQ(frameAtSeconds(3.0), afterExternalInvalidation.position);

    state.process(
            {Seek30Operation::ClearCurrent, generation}, inputsAt(3.0));
    EXPECT_FALSE(hasMemoryCueAt(pTrack, frameAtSeconds(3.0)));

    pCue->setStartPosition(frameAtSeconds(1.0));
    const auto afterRestoration = state.process(
            {Seek30Operation::SeekNext, generation}, inputsAt(0.0));
    EXPECT_EQ(generation, state.generation());
    EXPECT_EQ(frameAtSeconds(1.0), afterRestoration.position);
}

TEST(Seek30WorkerStateTest, ClearNearestUsesOneSecondGateAndGeneration) {
    Seek30WorkerState state;
    const auto pOldTrack = trackWithMemoryCues({1.0});
    state.trackLoaded(pOldTrack);
    const auto oldGeneration = state.generation();

    const auto pNewTrack = trackWithMemoryCues({3.0});
    state.trackLoaded(pNewTrack);
    const auto newGeneration = state.generation();
    EXPECT_NE(oldGeneration, newGeneration);

    auto inputs = inputsAt(3.5);
    state.process({Seek30Operation::ClearAll, oldGeneration}, inputs);
    EXPECT_EQ(1, memoryCueCount(pNewTrack));

    inputs.currentPosition = frameAtSeconds(4.0);
    state.process({Seek30Operation::ClearNearest, newGeneration}, inputs);
    EXPECT_EQ(1, memoryCueCount(pNewTrack));

    inputs.currentPosition = frameAtFrames(3.0 * kSampleRate + kSampleRate - 1.0);
    state.process({Seek30Operation::ClearNearest, newGeneration}, inputs);
    EXPECT_EQ(0, memoryCueCount(pNewTrack));

    state.trackLoaded(TrackPointer());
    state.process({Seek30Operation::CreateAtCurrent, newGeneration}, inputs);
    EXPECT_EQ(0, memoryCueCount(pNewTrack));
}

TEST(Seek30WorkerStateTest, ClearNearestPrefersCueAtOrBeforeOnTie) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({9.5, 10.5});
    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    state.process(
            {Seek30Operation::ClearNearest, generation}, inputsAt(10.0));

    EXPECT_EQ(1, memoryCueCount(pTrack));
    EXPECT_FALSE(hasMemoryCueAt(pTrack, frameAtSeconds(9.5)));
    EXPECT_TRUE(hasMemoryCueAt(pTrack, frameAtSeconds(10.5)));
}

TEST(Seek30WorkerStateTest, SeekPreviousUsesOnePointFiveSecondGate) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({0.0});
    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    auto inputs = inputsAt(1.5);
    inputs.playing = true;
    const auto boundaryTarget = state.process(
            {Seek30Operation::SeekPrevious, generation}, inputs);
    ASSERT_TRUE(boundaryTarget.position.isValid());
    EXPECT_EQ(frameAtSeconds(0.0), boundaryTarget.position);

    inputs.currentPosition =
            frameAtFrames(1.5 * kSampleRate - 1.0);
    const auto insideWindowTarget = state.process(
            {Seek30Operation::SeekPrevious, generation}, inputs);
    EXPECT_FALSE(insideWindowTarget.position.isValid());
}

TEST(Seek30WorkerStateTest, ClearCurrentClearNextAndSeekPreviousBehavior) {
    Seek30WorkerState state;
    const auto pTrack = trackWithMemoryCues({1.0, 3.0, 5.0});
    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    auto inputs = inputsAt(3.0);
    state.process({Seek30Operation::ClearCurrent, generation}, inputs);
    EXPECT_EQ(2, memoryCueCount(pTrack));

    state.process({Seek30Operation::ClearCurrent, generation}, inputs);
    EXPECT_EQ(2, memoryCueCount(pTrack));

    inputs.currentPosition = frameAtSeconds(1.5);
    state.process({Seek30Operation::ClearNext, generation}, inputs);
    EXPECT_EQ(1, memoryCueCount(pTrack));

    inputs.currentPosition = frameAtSeconds(4.0);
    const auto target = state.process(
            {Seek30Operation::SeekPrevious, generation}, inputs);
    ASSERT_TRUE(target.position.isValid());
    EXPECT_EQ(frameAtSeconds(1.0), target.position);

    inputs.currentPosition = frameAtSeconds(0.5);
    const auto noTarget = state.process(
            {Seek30Operation::SeekPrevious, generation}, inputs);
    EXPECT_FALSE(noTarget.position.isValid());

    inputs.currentPosition = frameAtSeconds(6.0);
    state.process({Seek30Operation::ClearNext, generation}, inputs);
    EXPECT_EQ(1, memoryCueCount(pTrack));
}
