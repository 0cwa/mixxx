#pragma once

#include <QList>
#include <QTimer>
#include <array>
#include <atomic>
#include <memory>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "engine/controls/enginecontrol.h"
#include "track/cue.h"

// Prepared by ChatGPT. The historical seek_30s/seek_30Prev control names
// navigate genuine memory cues. Track and cue operations run on this QObject's
// thread. Other threads publish commands to a bounded queue without allocations
// or locks. Accepted commands retain publication order. End ChatGPT text.
class Seek30Control final : public EngineControl {
    Q_OBJECT
  public:
    Seek30Control(const QString& group, UserSettingsPointer pConfig);
    ~Seek30Control() override = default;

    void trackLoaded(TrackPointer pNewTrack) override;

  private slots:
    void processPendingCommands();

  private:
    enum class Command {
        Next,
        Previous,
        Create,
        ClearAll,
        ClearCurrent,
        ClearPrevious,
        ClearNext,
        ClearNearest,
        Count,
    };
    static constexpr std::size_t kCommandCount = static_cast<std::size_t>(Command::Count);
    static constexpr uint32_t kQueueCapacity = 64;
    static constexpr uint32_t kNoPendingCommand = kQueueCapacity;
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);

    struct PendingCommand {
        Command command{Command::Count};
        uint32_t generation{0};
        uint32_t next{kNoPendingCommand};
        FrameInfo frame;
        mixxx::audio::FramePos queuedSeekPosition;
    };

    void requestCommand(Command command, double value);
    void enqueueCommand(Command command, uint32_t generation);
    bool preparePendingCommandBatch(uint32_t generation);
    void applyCommand(const PendingCommand& pending);
    void rebuildMemoryCueCache();
    void createAtCurrent(mixxx::audio::FramePos position);
    void clearAtCurrent(Command command, mixxx::audio::FramePos position);
    void navigate(bool previous, const PendingCommand& pending);

    std::unique_ptr<ControlObject> m_pMemoryCue;
    std::array<std::unique_ptr<ControlPushButton>, kCommandCount> m_controls;
    std::array<PendingCommand, kQueueCapacity> m_commandQueue;
    std::atomic<uint64_t> m_availableCommandSlots{~uint64_t{0}};
    std::atomic<uint32_t> m_pendingCommandHead{kNoPendingCommand};
    std::array<uint32_t, kQueueCapacity> m_pendingBatch;
    uint32_t m_pendingBatchSize{0};
    bool m_processingCommands{false};
    std::atomic<uint32_t> m_trackGeneration{0};
    std::atomic<uint32_t> m_droppedCommands{0};
    uint32_t m_loadedGeneration{0};
    const Qt::HANDLE m_ownerThreadId;
    QTimer m_commandTimer;
    TrackPointer m_pLoadedTrack;
    QList<CuePointer> m_memoryCues;
    mixxx::audio::FramePos m_navigationPosition;

    friend class MemoryCueControlTest;
};
