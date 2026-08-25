#pragma once

#include <QObject>
#include <atomic>
#include <cstdint>
#include <memory>

#include "audio/frame.h"
#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "control/controlvalue.h"
#include "engine/controls/enginecontrol.h"
#include "engine/controls/seek30workerstate.h"

class CachingReaderWorker;

// Adapter for the Seek30 controls. DirectConnection callbacks only enqueue
// scalar commands for CachingReaderWorker. Track and cue ownership remains in
// that worker; process() only forwards a scalar target to EngineBuffer.
class Seek30Control final : public EngineControl {
    Q_OBJECT
  public:
    Seek30Control(const QString& group, UserSettingsPointer pConfig);
    ~Seek30Control() override = default;

    void process(const double rate,
            mixxx::audio::FramePos currentPosition,
            const std::size_t bufferSize) override;

    void setWorker(CachingReaderWorker* pWorker);

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
    void createControls();
    void enqueue(Seek30Operation operation);

    // Called by CachingReaderWorker only after it has removed a command from
    // the mailbox. It reads only scalar ControlObject values and EngineControl
    // frameInfo(); it never accesses worker-owned track or cue state.
    Seek30WorkerInputs seek30WorkerInputs() const;
    void publishSeekTarget(const Seek30SeekTarget& target);

    std::unique_ptr<ControlObject> m_pMemoryCue;
    std::unique_ptr<ControlPushButton> m_pSeek30;
    std::unique_ptr<ControlPushButton> m_pSeek30Prev;
    std::unique_ptr<ControlPushButton> m_pMemoryCreateAtCurrent;
    std::unique_ptr<ControlPushButton> m_pMemoryClearAll;
    std::unique_ptr<ControlPushButton> m_pMemoryClearCurrent;
    std::unique_ptr<ControlPushButton> m_pMemoryClearPrev;
    std::unique_ptr<ControlPushButton> m_pMemoryClearNext;
    std::unique_ptr<ControlPushButton> m_pMemoryClearNearest;

    std::atomic<CachingReaderWorker*> m_pWorker{nullptr};
    ControlValueAtomic<Seek30SeekTarget> m_seekTarget;
    std::uint64_t m_consumedSeekSequence{0};

    friend class CachingReaderWorker;
};
