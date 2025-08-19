#pragma once

#include <memory>
#include <algorithm>

#include <QObject>
#include <QList>
#include <QSet>

#include "audio/frame.h"
#include "audio/types.h"
#include "engine/engine.h"
#include "engine/controls/enginecontrol.h"
#include "control/controlpushbutton.h"
#include "control/controlobject.h"
#include "engine/enginebuffer.h"
#include "track/cue.h"

// A very small control that seeks the deck to 30.0 seconds absolute
// using EngineBuffer::seekAbs(...).
//
// Exposes a trigger button "[<group>],seek_30s".
// When pressed (value > 0), the deck seeks to 30s.
//
// NOTE: We capture the current sample rate via setFrameInfo(...)
// that EngineBuffer calls on all EngineControl instances.
class Seek30Control final : public EngineControl {
    Q_OBJECT
  public:
    Seek30Control(const QString& group, UserSettingsPointer pConfig)
            : EngineControl(group, pConfig) {
        createControls();
    }

    ~Seek30Control() override = default;

    // EngineControl API
    void setFrameInfo(mixxx::audio::FramePos /*position*/,
                      mixxx::audio::FramePos /*endPosition*/,
                      mixxx::audio::SampleRate sampleRate) override {
        m_sampleRate = sampleRate;
    }

    void trackLoaded(TrackPointer pNewTrack) override;

  private slots:
    void createAtCurrent(double v);
    void clearAll(double v);
    void slotSeek30(double v);

  private:

    // Cache only the memory cues that belong to the currently loaded track.
    QList<CuePointer> m_memoryCues;

    // Rebuilds the cache from the track's cue list.
    void rebuildMemoryCueCache();

    void sortCueCache();

    // Returns the smallest non-negative index not yet used by memory cues.
    int nextFreeMemoryCueIndex() const;

    // Helper to create & register a new memory cue at a given position.
    // Returns the index that was assigned.
    int createMemoryCueAt(const mixxx::audio::FramePos& pos);

    void createControls() {
        m_pSeek30 = std::make_unique<ControlPushButton>(ConfigKey(m_group, "seek_30s"));
        m_pSeek30->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pSeek30.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::slotSeek30,
                Qt::DirectConnection);

        m_pMemoryCreateAtCurrent = std::make_unique<ControlPushButton>(ConfigKey(m_group, "memory_create_at_current"));
        m_pMemoryCreateAtCurrent->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pMemoryCreateAtCurrent.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::createAtCurrent,
                Qt::DirectConnection);

        m_pMemoryClearAll = std::make_unique<ControlPushButton>(ConfigKey(m_group, "memory_clear_all"));
        m_pMemoryClearAll->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pMemoryClearAll.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::clearAll,
                Qt::DirectConnection);

    }

    std::unique_ptr<ControlPushButton> m_pSeek30;
    std::unique_ptr<ControlPushButton> m_pMemoryCreateAtCurrent;
    std::unique_ptr<ControlPushButton> m_pMemoryClearAll;
    mixxx::audio::SampleRate m_sampleRate;

    TrackPointer m_pLoadedTrack; // is written from an engine worker thread
};
