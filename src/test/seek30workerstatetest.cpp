#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <initializer_list>
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
    for (const auto& pCue : pTrack->getCuePoints()) {
        if (pCue && pCue->getType() == mixxx::CueType::Memory) {
            ++count;
        }
    }
    return count;
}

bool hasMemoryCueAt(const TrackPointer& pTrack, const mixxx::audio::FramePos& position) {
    for (const auto& pCue : pTrack->getCuePoints()) {
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
    const auto pTrack = trackWithMemoryCues({9.0, 11.0});
    state.trackLoaded(pTrack);
    const auto generation = state.generation();

    state.process(
            {Seek30Operation::ClearNearest, generation}, inputsAt(10.0));

    EXPECT_EQ(1, memoryCueCount(pTrack));
    EXPECT_FALSE(hasMemoryCueAt(pTrack, frameAtSeconds(9.0)));
    EXPECT_TRUE(hasMemoryCueAt(pTrack, frameAtSeconds(11.0)));
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
