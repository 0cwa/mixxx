#pragma once

#include <qtmetamacros.h>

#include "engine/bufferscalers/enginebufferscale.h"
#include "signalsmith-stretch.h"
#include "util/samplebuffer.h"

class ReadAheadManager;

class EngineBufferScaleSignalSmith final : public EngineBufferScale {
    Q_OBJECT
  public:
    enum class Preset {
        Default,
        Cheaper
    };
    Q_ENUM(Preset);
    explicit EngineBufferScaleSignalSmith(ReadAheadManager* pReadAheadManager);
    ~EngineBufferScaleSignalSmith() override = default;

    void setScaleParameters(double base_rate, double* pTempoRatio, double* pPitchRatio) override;
    void setPreset(Preset preset) {
        m_currentPreset = preset;
        onSignalChanged();
    }
    void clear() override;
    double scaleBuffer(CSAMPLE* pOutputBuffer, SINT iOutputBufferSize) override;

  private:
    struct FrameReadResult {
        SINT framesRead;
        bool retryPending;
    };

    struct PendingScale {
        bool active{false};
        SINT outputFrames{0};
        SINT prerollFrames{0};
        SINT prerollRead{0};
        bool prerollApplied{false};
        SINT inputFrames{0};
        SINT inputRead{0};
        double frameOffset{0};
        double fractionalLeftover{0};
        double effectiveRate{0};
    };

    struct ScaleParameters {
        double baseRate{1};
        double tempoRatio{1};
        double pitchRatio{1};
        bool valid{true};
    };

    void onSignalChanged() override;
    void updateLatency();
    void applyScaleParameters(const ScaleParameters& parameters);
    bool readFrames(SINT frames, SINT& framesRead);
    FrameReadResult fetchAndDeinterleave(SINT samples, SINT offset = 0);

    ReadAheadManager* m_pReadAheadManager;
    signalsmith::stretch::SignalsmithStretch<float> m_stretch;

    std::vector<mixxx::SampleBuffer> m_buffers;
    std::vector<float*> m_bufferPtrs;
    mixxx::SampleBuffer m_interleavedBuffer;
    double m_frameFractionalLeftover{0};
    SINT m_expectedFrameLatency{0};
    double m_currentFrameOffset{0};
    PendingScale m_pendingScale;
    ScaleParameters m_deferredScaleParameters;
    bool m_hasDeferredScaleParameters{false};
    bool m_bBackwards{false};
    bool m_parametersValid{true};
    Preset m_currentPreset{Preset::Default};
};
