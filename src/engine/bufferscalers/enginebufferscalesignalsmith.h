#pragma once

#include <QObject>

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
    }
    void clear() override;
    double scaleBuffer(CSAMPLE* pOutputBuffer, SINT iOutputBufferSize) override;

  private:
    void onSignalChanged() override;
    struct InputReadResult {
        SINT framesRead;
        bool retryPending;
    };

    InputReadResult fetchAndDeinterleave(SINT sampleToRead, SINT frameOffset = 0);

    ReadAheadManager* m_pReadAheadManager;
    signalsmith::stretch::SignalsmithStretch<float> m_stretch;

    /// The audio buffers samples used to send audio to Signalsmith and to
    /// receive processed audio from Signalsmith. This is needed because Mixxx
    /// uses interleaved buffers in most other places.
    std::vector<mixxx::SampleBuffer> m_buffers;
    /// These point to the buffers in `m_buffers`. They can be defined here
    /// since this object cannot be moved or copied.
    std::vector<float*> m_bufferPtrs;

    std::vector<mixxx::SampleBuffer> m_outputBuffers;
    std::vector<float*> m_outputBufferPtrs;

    /// Contains interleaved samples read from `m_pReadAheadManager`. These need
    /// to be deinterleaved before they can be passed to Signalsmith.
    mixxx::SampleBuffer m_interleavedBuffer;

    // This stores the fractional part of the sample count that should have been
    // inputted to perfectly match the requested the rates. However, since there
    // is no such a thing as fractional sample, we keep memory of it till it
    // constitute and entire frame and add it up to stay as much in sync as
    // possible.
    double m_frameFractionalLeftover;
    SINT m_expectedFrameLatency;
    SINT m_currentFrameOffset;
    // Holds the playback direction
    bool m_bBackwards;
    Preset m_currentPreset;
};
