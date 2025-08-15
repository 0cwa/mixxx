#pragma once

#include <memory>

#include <QObject>

#include "audio/frame.h"
#include "audio/types.h"
#include "engine/engine.h"
#include "engine/controls/enginecontrol.h"
#include "control/controlpushbutton.h"
#include "control/controlobject.h"
#include "engine/enginebuffer.h"

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

  private slots:
    void slotSeek30(double v) {
        if (v <= 0) {
            return;
        }
        // Convert 30.0 s -> engine sample position, then -> FramePos via factory.
        const double engineSamplePos =
                30.0 * m_sampleRate.toDouble() * mixxx::kEngineChannelOutputCount;

        const auto target = mixxx::audio::FramePos::fromEngineSamplePos(engineSamplePos);

        if (target.isValid()) {
            getEngineBuffer()->seekAbs(target);
        }
    }

  private:
    void createControls() {
        m_pSeek30 = std::make_unique<ControlPushButton>(ConfigKey(m_group, "seek_30s"));
        m_pSeek30->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(m_pSeek30.get(),
                &ControlObject::valueChanged,
                this,
                &Seek30Control::slotSeek30,
                Qt::DirectConnection);
    }

    std::unique_ptr<ControlPushButton> m_pSeek30;
    mixxx::audio::SampleRate m_sampleRate;
};
