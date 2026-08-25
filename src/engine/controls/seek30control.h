#pragma once

#include <QList>
#include <QObject>
#include <atomic>
#include <memory>

#include "audio/frame.h"
#include "audio/types.h"
#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "control/controlvalue.h"
#include "engine/controls/enginecontrol.h"
#include "engine/engine.h"
#include "engine/enginebuffer.h"
#include "track/cue.h"

// A very small control that seeks the deck to 30.0 seconds absolute
// using EngineBuffer::seekAbs(...).
//
// Exposes a trigger button "[<group>],seek_30s".
// When pressed (value > 0), the deck seeks to 30s.
//
// The current sample rate is read from EngineControl's atomic FrameInfo.
class Seek30Control final : public EngineControl {
    Q_OBJECT
  public:
    Seek30Control(const QString& group, UserSettingsPointer pConfig)
            : EngineControl(group, pConfig) {
        createControls();
    }

    ~Seek30Control() override = default;

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
    struct TrackState {
        TrackPointer pLoadedTrack;
        QList<CuePointer> memoryCues;
    };

    // TrackState is non-trivial, so use the non-atomic ring implementation
    // directly. It provides lock-free snapshots while preserving ownership of
    // the track and cue pointers in each snapshot.
    class TrackStateValue final
            : public ControlValueAtomicBase<TrackState, 8, false> {
      public:
        TrackStateValue() = default;
    };

    class StateUpdateGuard final {
      public:
        StateUpdateGuard() = default;
        explicit StateUpdateGuard(Seek30Control* pControl)
                : m_pControl(pControl) {
        }
        ~StateUpdateGuard();

        StateUpdateGuard(const StateUpdateGuard&) = delete;
        StateUpdateGuard& operator=(const StateUpdateGuard&) = delete;

        StateUpdateGuard(StateUpdateGuard&& other) noexcept
                : m_pControl(other.m_pControl) {
            other.m_pControl = nullptr;
        }
        StateUpdateGuard& operator=(StateUpdateGuard&& other) noexcept {
            if (this != &other) {
                if (m_pControl) {
                    m_pControl->finishStateUpdate();
                }
                m_pControl = other.m_pControl;
                other.m_pControl = nullptr;
            }
            return *this;
        }

        explicit operator bool() const {
            return m_pControl != nullptr;
        }

      private:
        Seek30Control* m_pControl{nullptr};
    };

    StateUpdateGuard tryBeginStateUpdate();
    StateUpdateGuard beginStateUpdate();
    void finishStateUpdate();
    bool tryGetTrackState(TrackState* pState) const;

    // Rebuilds the cache from the track's cue list.
    QList<CuePointer> rebuildMemoryCueCache(const TrackPointer& pTrack) const;

    static void sortCueCache(QList<CuePointer>* pMemoryCues);

    // Returns the smallest non-negative index not yet used by memory cues.
    static int nextFreeMemoryCueIndex(const QList<CuePointer>& memoryCues);

    double sampleRate() const {
        return frameInfo().sampleRate.toDouble();
    }

    // Helper to create & register a new memory cue at a given position.
    // Returns the index that was assigned.
    int createMemoryCueAt(TrackState* pState, const mixxx::audio::FramePos& pos);

    void createControls() {
        m_pMemoryCue = std::make_unique<ControlObject>(ConfigKey(m_group, "memory_cue"));
        m_pMemoryCue->set(0.f);

        m_pSeek30 = std::make_unique<ControlPushButton>(ConfigKey(m_group, "seek_30s"));
        m_pSeek30->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pSeek30.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::slotSeek30,
                Qt::DirectConnection);

        m_pSeek30Prev = std::make_unique<ControlPushButton>(ConfigKey(m_group, "seek_30Prev"));
        m_pSeek30Prev->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pSeek30Prev.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::slotSeek30Prev,
                Qt::DirectConnection);

        m_pMemoryCreateAtCurrent = std::make_unique<ControlPushButton>(
                ConfigKey(m_group, "memory_create_at_current"));
        m_pMemoryCreateAtCurrent->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pMemoryCreateAtCurrent.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::createAtCurrent,
                Qt::DirectConnection);

        m_pMemoryClearAll = std::make_unique<ControlPushButton>(
                ConfigKey(m_group, "memory_clear_all"));
        m_pMemoryClearAll->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pMemoryClearAll.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::clearAll,
                Qt::DirectConnection);

        m_pMemoryClearCurrent = std::make_unique<ControlPushButton>(
                ConfigKey(m_group, "memory_clear_current"));
        m_pMemoryClearCurrent->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pMemoryClearCurrent.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::clearCurrent,
                Qt::DirectConnection);

        m_pMemoryClearPrev = std::make_unique<ControlPushButton>(
                ConfigKey(m_group, "memory_clear_prev"));
        m_pMemoryClearPrev->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pMemoryClearPrev.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::clearPrev,
                Qt::DirectConnection);

        m_pMemoryClearNext = std::make_unique<ControlPushButton>(
                ConfigKey(m_group, "memory_clear_next"));
        m_pMemoryClearNext->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pMemoryClearNext.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::clearNext,
                Qt::DirectConnection);

        m_pMemoryClearNearest = std::make_unique<ControlPushButton>(
                ConfigKey(m_group, "memory_clear_nearest"));
        m_pMemoryClearNearest->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pMemoryClearNearest.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::clearNearest,
                Qt::DirectConnection);
    }

    std::unique_ptr<ControlObject> m_pMemoryCue;
    std::unique_ptr<ControlPushButton> m_pSeek30;
    std::unique_ptr<ControlPushButton> m_pSeek30Prev;
    std::unique_ptr<ControlPushButton> m_pMemoryCreateAtCurrent;
    std::unique_ptr<ControlPushButton> m_pMemoryClearAll;
    std::unique_ptr<ControlPushButton> m_pMemoryClearCurrent;
    std::unique_ptr<ControlPushButton> m_pMemoryClearPrev;
    std::unique_ptr<ControlPushButton> m_pMemoryClearNext;
    std::unique_ptr<ControlPushButton> m_pMemoryClearNearest;
    TrackStateValue m_trackState;
    // Direct control callbacks never wait for a track-state writer. A flag is
    // used instead of a mutex so these callbacks remain nonblocking.
    std::atomic_flag m_trackStateUpdateInProgress = ATOMIC_FLAG_INIT;
};
