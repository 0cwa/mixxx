#pragma once

#include <QList>
#include <QObject>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "audio/frame.h"
#include "audio/types.h"
#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "control/controlvalue.h"
#include "engine/controls/enginecontrol.h"
#include "track/cue.h"
#include "track/track_decl.h"

// A very small control that seeks the deck to 30.0 seconds absolute
// using EngineBuffer::seekAbs(...).
//
// Exposes a trigger button "[<group>],seek_30s". When pressed (value > 0),
// the deck seeks to the next memory cue.
class Seek30Control final : public EngineControl {
    Q_OBJECT
  public:
    Seek30Control(const QString& group, UserSettingsPointer pConfig);
    ~Seek30Control() override = default;

    void process(const double rate,
            mixxx::audio::FramePos currentPosition,
            const std::size_t bufferSize) override;
    void trackLoaded(TrackPointer pNewTrack) override;

  private slots:
    void createAtCurrent(double v);
    void clearAll(double v);
    void clearPrev(double v);
    void clearNext(double v);
    void clearCurrent(double v);
    void clearNearest(double v);
    void slotSeek30(double v);
    void slotSeek30Prev(double v);

  private:
    friend class Seek30ControlTest;

    enum class SeekDirection : std::uint8_t {
        Next,
        Previous,
    };

    enum class ClearOperation : std::uint32_t {
        All = 1u << 0,
        Previous = 1u << 1,
        Next = 1u << 2,
        Current = 1u << 3,
        Nearest = 1u << 4,
    };

    struct SeekRequest {
        std::uint64_t generation{0};
        std::uint64_t sequence{0};
        SeekDirection direction{SeekDirection::Next};
    };

    struct CreateRequest {
        std::uint64_t generation{0};
        std::uint64_t sequence{0};
    };

    struct ClearRequest {
        std::uint64_t generation{0};
    };

    static_assert(std::is_trivially_copyable_v<SeekRequest>);
    static_assert(std::is_trivially_copyable_v<CreateRequest>);
    static_assert(std::is_trivially_copyable_v<ClearRequest>);

    static constexpr std::size_t kClearOperationCount = 5;

    void publishSeek(SeekDirection direction);
    void publishCreate();
    void publishClear(ClearOperation operation);

    static std::size_t clearOperationIndex(ClearOperation operation);

    // The following methods are called only from process(), on the audio
    // thread. They may access the borrowed track and the cue cache.
    void rebuildMemoryCueCache();
    void sortCueCache();
    int nextFreeMemoryCueIndex() const;
    int createMemoryCueAt(const mixxx::audio::FramePos& pos);
    bool currentFramePosition(mixxx::audio::FramePos* pPosition) const;

    void processCreateAtCurrent();
    void processClearAll();
    void processClearPrev();
    void processClearNext();
    void processClearCurrent();
    void processClearNearest();
    void processSeek30();
    void processSeek30Prev();

    void createControls();

    std::unique_ptr<ControlObject> m_pMemoryCue;
    std::unique_ptr<ControlPushButton> m_pSeek30;
    std::unique_ptr<ControlPushButton> m_pSeek30Prev;
    std::unique_ptr<ControlPushButton> m_pMemoryCreateAtCurrent;
    std::unique_ptr<ControlPushButton> m_pMemoryClearAll;
    std::unique_ptr<ControlPushButton> m_pMemoryClearCurrent;
    std::unique_ptr<ControlPushButton> m_pMemoryClearPrev;
    std::unique_ptr<ControlPushButton> m_pMemoryClearNext;
    std::unique_ptr<ControlPushButton> m_pMemoryClearNearest;

    // EngineBuffer owns the TrackPointer. It publishes the borrowed pointer
    // before publishing the generation, and keeps that ownership while the
    // loading gate permits process() to run. The audio thread is the only
    // thread that dereferences m_pLoadedTrack.
    std::atomic<Track*> m_pPublishedTrack{nullptr};
    std::atomic<std::uint64_t> m_trackGeneration{0};
    Track* m_pLoadedTrack{nullptr};
    std::uint64_t m_processedTrackGeneration{0};

    // All cue-cache state is audio-thread-only.
    QList<CuePointer> m_memoryCues;

    // Each clear operation is represented by an idempotent bit. Its
    // generation is kept in a scalar mailbox so a command from an old track
    // cannot mutate the new track.
    std::atomic<std::uint32_t> m_pendingClearOperations{0};
    std::array<ControlValueAtomic<ClearRequest>, kClearOperationCount> m_clearRequests;

    // Seek and create requests are latest-value mailboxes. The sequence
    // number makes each publication one-shot without clearing a mailbox from
    // the consumer and racing a producer.
    ControlValueAtomic<SeekRequest> m_seekRequest;
    ControlValueAtomic<CreateRequest> m_createRequest;
    std::atomic<std::uint64_t> m_nextRequestSequence{0};
    std::uint64_t m_lastSeekRequestSequence{0};
    std::uint64_t m_lastCreateRequestSequence{0};
};
