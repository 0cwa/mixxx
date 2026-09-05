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
};
