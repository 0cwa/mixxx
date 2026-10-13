#pragma once

#include <QList>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "audio/frame.h"
#include "audio/types.h"
#include "track/cue.h"
#include "track/track_decl.h"

#ifdef BUILD_TESTING
class F001ProbeForTest;
#endif

enum class Seek30Operation : std::uint8_t {
    SeekNext,
    SeekPrevious,
    CreateAtCurrent,
    ClearAll,
    ClearPrevious,
    ClearNext,
    ClearCurrent,
    ClearNearest,
};

struct Seek30Command {
    Seek30Operation operation;
    std::uint64_t generation;
};

#ifdef BUILD_TESTING
// Fixed-capacity worker-side evidence for the focused final-drain test.
// Written only by CachingReaderWorker's worker thread and read after join.
struct LoopCueApplySnapshotForTest {
    enum class Branch : std::uint8_t {
        StableObservationUnavailable,
        NoLoadedTrack,
        RemoveTemporaryCue,
        UpdateExistingCue,
        ExistingCueUnchanged,
        CreateTemporaryCue,
    };

    Branch branch{Branch::NoLoadedTrack};
    std::uintptr_t trackIdentity{0};
    bool trackIdValid{false};
    std::uintptr_t cueIdentity{0};
    bool observedStartValid{false};
    bool observedEndValid{false};
    double observedStart{0};
    double observedEnd{0};
    bool cueBeforePresent{false};
    bool cueBeforeStartValid{false};
    bool cueBeforeEndValid{false};
    double cueBeforeStart{0};
    double cueBeforeEnd{0};
    bool cueAfterPresent{false};
    bool cueAfterStartValid{false};
    bool cueAfterEndValid{false};
    double cueAfterStart{0};
    double cueAfterEnd{0};
};

struct LoopCueApplyTraceForTest {
    static constexpr std::size_t kCapacity = 16;
    std::array<LoopCueApplySnapshotForTest, kCapacity> entries{};
    std::size_t count{0};
    bool overflow{false};

    void append(const LoopCueApplySnapshotForTest& entry) {
        if (count == entries.size()) {
            overflow = true;
            return;
        }
        entries[count++] = entry;
    }
};
#endif

struct Seek30WorkerInputs {
    mixxx::audio::FramePos currentPosition;
    mixxx::audio::SampleRate sampleRate;
    bool quantize;
    mixxx::audio::FramePos closestBeat;
    bool playing;
};

struct Seek30SeekTarget {
    mixxx::audio::FramePos position{mixxx::audio::kInvalidFramePos};
    std::uint64_t generation{0};
    std::uint64_t sequence{0};
};

static_assert(std::is_trivially_copyable_v<Seek30Command>);
static_assert(std::is_trivially_copyable_v<Seek30WorkerInputs>);
static_assert(std::is_trivially_copyable_v<Seek30SeekTarget>);

// Bounded multi-producer, single-consumer mailbox for scalar Seek30 commands.
// A full mailbox drops the newest command. The caller records that overflow;
// no command is silently replaced by a latest-value flag.
class Seek30CommandMailbox final {
  public:
    static constexpr std::size_t kCapacity = 256;

    Seek30CommandMailbox();

    bool tryPush(Seek30Command command);
    bool tryPop(Seek30Command* pCommand);

  private:
    struct Slot {
        std::atomic<std::size_t> sequence{0};
        Seek30Command command{};
    };

    std::array<Slot, kCapacity> m_slots;
    std::atomic<std::size_t> m_enqueuePosition{0};
    std::atomic<std::size_t> m_dequeuePosition{0};
};

class Seek30WorkerState final {
  public:
    void trackLoaded(TrackPointer pNewTrack);

    // Synchronize the volatile, no-hotcue loop cue from the worker thread.
    // This shares the Track/Cue owner used by Seek30 so updates retain cue
    // identity and Track insertion order without touching Track from the
    // realtime callback.
    void updateLoopCue(
            const mixxx::audio::FramePos& startPosition,
            const mixxx::audio::FramePos& endPosition);

#ifdef BUILD_TESTING
    void setF001ProbeForTest(F001ProbeForTest* pProbe) {
        m_pF001ProbeForTest.store(pProbe, std::memory_order_release);
    }
    void setLoopCueApplyTraceForTest(LoopCueApplyTraceForTest* pTrace) {
        m_pLoopCueApplyTraceForTest.store(pTrace, std::memory_order_release);
    }
    void recordUnavailableLoopCueObservationForTest();
#endif

    std::uint64_t generation() const {
        return m_generation;
    }

    Seek30SeekTarget process(
            const Seek30Command& command,
            const Seek30WorkerInputs& inputs);

  private:
    void rebuildMemoryCueCache();
    void sortCueCache();
    void createMemoryCueAt(const mixxx::audio::FramePos& position);
    void clearCurrent(const mixxx::audio::FramePos& currentPosition);
    void clearPrevious(const mixxx::audio::FramePos& currentPosition);
    void clearNext(const mixxx::audio::FramePos& currentPosition);
    void clearNearest(const Seek30WorkerInputs& inputs);
    Seek30SeekTarget seekNext(const Seek30WorkerInputs& inputs);
    Seek30SeekTarget seekPrevious(const Seek30WorkerInputs& inputs);

    TrackPointer m_pLoadedTrack;
    QList<CuePointer> m_memoryCues;
    std::uint64_t m_generation{0};
#ifdef BUILD_TESTING
    std::atomic<F001ProbeForTest*> m_pF001ProbeForTest{nullptr};
    std::atomic<LoopCueApplyTraceForTest*> m_pLoopCueApplyTraceForTest{nullptr};
#endif
};
