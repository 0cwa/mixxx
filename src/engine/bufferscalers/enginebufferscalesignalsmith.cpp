#include "engine/bufferscalers/enginebufferscalesignalsmith.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "engine/engine.h"
#include "engine/readaheadmanager.h"
#include "moc_enginebufferscalesignalsmith.cpp"
#include "util/assert.h"
#include "util/defs.h"
#include "util/fpclassify.h"
#include "util/sample.h"
#include "util/timer.h"

EngineBufferScaleSignalSmith::EngineBufferScaleSignalSmith(
        ReadAheadManager* pReadAheadManager)
        : m_pReadAheadManager(pReadAheadManager),
          m_interleavedBuffer(mixxx::kMaxSupportedStems * MAX_BUFFER_LEN *
                  mixxx::audio::ChannelCount::stereo()) {
    onSignalChanged();
}

void EngineBufferScaleSignalSmith::setScaleParameters(
        double base_rate, double* pTempoRatio, double* pPitchRatio) {
    ScaleParameters parameters{base_rate, *pTempoRatio, *pPitchRatio, true};
    parameters.valid = util_isfinite(base_rate) && base_rate > 0 &&
            util_isfinite(parameters.tempoRatio) &&
            util_isfinite(parameters.pitchRatio);
    if (parameters.valid) {
        const double rate = std::min(
                base_rate * std::fabs(parameters.tempoRatio), MAX_SEEK_SPEED);
        const double transpose = std::clamp(
                base_rate * parameters.pitchRatio,
                1.0 / MAX_SEEK_SPEED,
                MAX_SEEK_SPEED);
        parameters.tempoRatio =
                std::copysign(rate / base_rate, parameters.tempoRatio);
        parameters.pitchRatio = transpose / base_rate;
        parameters.valid = util_isfinite(parameters.tempoRatio) &&
                util_isfinite(parameters.pitchRatio);
    }
    if (parameters.valid) {
        *pTempoRatio = parameters.tempoRatio;
        *pPitchRatio = parameters.pitchRatio;
    } else {
        *pTempoRatio = 0;
        *pPitchRatio = 1;
    }
    if (m_pendingScale.active) {
        m_deferredScaleParameters = parameters;
        m_hasDeferredScaleParameters = true;
    } else {
        applyScaleParameters(parameters);
    }
}

void EngineBufferScaleSignalSmith::applyScaleParameters(
        const ScaleParameters& parameters) {
    m_parametersValid = parameters.valid;
    if (!m_parametersValid) {
        return;
    }
    m_dBaseRate = parameters.baseRate;
    m_bBackwards = parameters.tempoRatio < 0;
    m_dTempoRatio = std::fabs(parameters.tempoRatio);
    m_dPitchRatio = parameters.pitchRatio;
    m_effectiveRate = m_dBaseRate * m_dTempoRatio;
    m_stretch.setTransposeFactor(static_cast<float>(m_dBaseRate * m_dPitchRatio));
    m_stretch.setFormantFactor(1.0);
    updateLatency();
}

void EngineBufferScaleSignalSmith::updateLatency() {
    const double latency = static_cast<double>(m_stretch.inputLatency()) +
            std::round(m_effectiveRate *
                    static_cast<double>(m_stretch.outputLatency()));
    if (!util_isfinite(latency) || latency < 0 ||
            latency >= static_cast<double>(std::numeric_limits<SINT>::max())) {
        m_parametersValid = false;
        return;
    }
    m_expectedFrameLatency = static_cast<SINT>(latency);
}

void EngineBufferScaleSignalSmith::onSignalChanged() {
    if (!getOutputSignal().isValid()) {
        return;
    }
    const auto channelCount = getOutputSignal().getChannelCount();
    if (channelCount.value() > mixxx::audio::ChannelCount::stem().value()) {
        return;
    }
    m_buffers.resize(channelCount);
    m_bufferPtrs.resize(channelCount);
    for (int channel = 0; channel < channelCount; ++channel) {
        if (m_buffers[channel].size() != MAX_BUFFER_LEN) {
            m_buffers[channel] = mixxx::SampleBuffer(MAX_BUFFER_LEN);
        }
        m_bufferPtrs[channel] = m_buffers[channel].data();
    }
    switch (m_currentPreset) {
    case Preset::Cheaper:
        m_stretch.presetCheaper(channelCount, getOutputSignal().getSampleRate());
        break;
    default:
        qWarning() << "Unsupported preset" << m_currentPreset << " so defaulting to default.";
        [[fallthrough]];
    case Preset::Default:
        m_stretch.presetDefault(channelCount, getOutputSignal().getSampleRate());
        break;
    }
    clear();
}

void EngineBufferScaleSignalSmith::clear() {
    m_pReadAheadManager->cancelPendingRetry();
    m_pendingScale = {};
    m_stretch.reset();
    m_currentFrameOffset = 0;
    m_frameFractionalLeftover = 0;
    if (m_hasDeferredScaleParameters) {
        applyScaleParameters(m_deferredScaleParameters);
        m_hasDeferredScaleParameters = false;
    }
}

EngineBufferScaleSignalSmith::FrameReadResult
EngineBufferScaleSignalSmith::fetchAndDeinterleave(SINT sampleToRead, SINT frameOffset) {
    const auto readResult = m_pReadAheadManager->getNextSamplesWithRetry(
            // The value doesn't matter here. All that matters is direction.
            (m_bBackwards ? -1 : 1) * m_dBaseRate * m_dTempoRatio,
            m_interleavedBuffer.data(),
            sampleToRead,
            getOutputSignal().getChannelCount());
    if (readResult.retryPending) {
        return {0, true};
    }
    const auto frameRead = getOutputSignal().samples2frames(readResult.samplesRead);

    switch (getOutputSignal().getChannelCount()) {
    case mixxx::audio::ChannelCount::stereo():
        SampleUtil::deinterleaveBuffer(
                m_buffers[0].data(frameOffset),
                m_buffers[1].data(frameOffset),
                m_interleavedBuffer.data(),
                frameRead);
        break;
    case mixxx::audio::ChannelCount::stem():
        SampleUtil::deinterleaveBuffer(
                m_buffers[0].data(frameOffset),
                m_buffers[1].data(frameOffset),
                m_buffers[2].data(frameOffset),
                m_buffers[3].data(frameOffset),
                m_buffers[4].data(frameOffset),
                m_buffers[5].data(frameOffset),
                m_buffers[6].data(frameOffset),
                m_buffers[7].data(frameOffset),
                m_interleavedBuffer.data(),
                frameRead);
        break;
    default: {
        int chCount = getOutputSignal().getChannelCount();
        // The sampler are ordered as following in pBuffer
        //    1234..X1234...X...
        // And need to be reordered as following
        // m_buffers#1 = 11..
        // m_buffers#2 = 22..
        // m_buffers#3 = 33..
        // m_buffers#4 = 44..fff
        // m_buffers#X = XX..
        //
        // Because of the unanticipated number of buffer and channel, we cannot
        // use any SampleUtil in this case
        for (SINT frameIdx = 0; frameIdx < frameRead; ++frameIdx) {
            for (int channel = 0; channel < chCount; channel++) {
                m_buffers[channel].data(frameOffset)[frameIdx] =
                        m_interleavedBuffer.data()[frameIdx * chCount + channel];
            }
        }
    } break;
    }
    return {frameRead, false};
}

bool EngineBufferScaleSignalSmith::readFrames(SINT frames, SINT& framesRead) {
    bool lastReadFailed = false;
    while (framesRead < frames) {
        const auto result = fetchAndDeinterleave(
                getOutputSignal().frames2samples(frames - framesRead), framesRead);
        if (result.retryPending) {
            return false;
        }
        framesRead += result.framesRead;
        if (lastReadFailed && result.framesRead == 0) {
            for (int channel = 0; channel < getOutputSignal().getChannelCount(); ++channel) {
                SampleUtil::clear(
                        m_bufferPtrs[channel] + framesRead, frames - framesRead);
            }
            break;
        }
        lastReadFailed = result.framesRead == 0;
    }
    return true;
}

double EngineBufferScaleSignalSmith::scaleBuffer(CSAMPLE* pOutputBuffer, SINT iOutputBufferSize) {
    ScopedTimer t(QStringLiteral("EngineBufferScaleSignalsmith::scaleBuffer"));
    if (pOutputBuffer == nullptr || iOutputBufferSize <= 0) {
        return 0;
    }
    const auto channels = getOutputSignal().getChannelCount();
    if (!getOutputSignal().isValid() || !channels.isValid() ||
            channels.value() > mixxx::audio::ChannelCount::stem().value() ||
            iOutputBufferSize % channels != 0 ||
            iOutputBufferSize / channels > MAX_BUFFER_LEN ||
            (!m_pendingScale.active && !m_parametersValid)) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        return 0;
    }

    const SINT outputFrames = getOutputSignal().samples2frames(iOutputBufferSize);
    if (!m_pendingScale.active) {
        const SINT preroll = m_currentFrameOffset == 0 && m_dTempoRatio > 0
                ? std::min(m_expectedFrameLatency, SINT(MAX_BUFFER_LEN))
                : 0;
        double offset = m_currentFrameOffset + preroll;
        double demand = m_effectiveRate * outputFrames + m_frameFractionalLeftover;
        if (offset != m_expectedFrameLatency && demand > 0) {
            // Spread a latency change across callbacks to avoid a sudden input spike.
            const double maxCorrection = static_cast<double>(outputFrames);
            const double correction = std::clamp(m_expectedFrameLatency - offset,
                    -std::min(demand, maxCorrection),
                    maxCorrection);
            demand += correction;
            offset += correction;
        }
        if (!(demand >= 0 && demand < MAX_BUFFER_LEN + 1.0)) {
            SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
            return 0;
        }

        const SINT inputFrames = static_cast<SINT>(demand);
        m_pendingScale = {
                true,
                outputFrames,
                preroll,
                0,
                false,
                inputFrames,
                0,
                offset,
                demand - inputFrames,
                m_effectiveRate};
        DEBUG_ASSERT(0 <= m_pendingScale.fractionalLeftover &&
                m_pendingScale.fractionalLeftover < 1);
    } else if (outputFrames != m_pendingScale.outputFrames) {
        // Engine callbacks use a fixed buffer size. Keep the pending input
        // request intact if a caller temporarily supplies a different size.
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        return 0;
    }

    auto& pending = m_pendingScale;
    if (pending.prerollFrames > 0 && !pending.prerollApplied) {
        if (!readFrames(pending.prerollFrames, pending.prerollRead)) {
            SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
            return 0;
        }
        m_stretch.outputSeek(m_bufferPtrs.data(), pending.prerollFrames);
        pending.prerollApplied = true;
        // outputSeek changes the stretcher immediately. Retain the corrected
        // latency offset if the following input read needs to be retried.
        m_currentFrameOffset = pending.frameOffset;
    }

    if (!readFrames(pending.inputFrames, pending.inputRead)) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        return 0;
    }

    const PendingScale completed = pending;
    std::array<float*, mixxx::audio::ChannelCount::stem()> outputBufferPtrs;
    for (std::size_t channel = 0; channel < outputBufferPtrs.size(); ++channel) {
        outputBufferPtrs[channel] = m_interleavedBuffer.data(channel * MAX_BUFFER_LEN);
    }
    {
        ScopedTimer t(QStringLiteral("Signalsmith::process"));
        m_stretch.process(m_bufferPtrs.data(),
                completed.inputFrames,
                outputBufferPtrs.data(),
                completed.outputFrames);
    }
    switch (channels) {
    case mixxx::audio::ChannelCount::stereo():
        SampleUtil::interleaveBuffer(pOutputBuffer,
                outputBufferPtrs[0],
                outputBufferPtrs[1],
                completed.outputFrames);
        break;
    case mixxx::audio::ChannelCount::stem():
        SampleUtil::interleaveBuffer(pOutputBuffer,
                outputBufferPtrs[0],
                outputBufferPtrs[1],
                outputBufferPtrs[2],
                outputBufferPtrs[3],
                outputBufferPtrs[4],
                outputBufferPtrs[5],
                outputBufferPtrs[6],
                outputBufferPtrs[7],
                completed.outputFrames);
        break;
    default:
        for (SINT frame = 0; frame < completed.outputFrames; ++frame) {
            for (int channel = 0; channel < channels; ++channel) {
                pOutputBuffer[frame * channels + channel] = outputBufferPtrs[channel][frame];
            }
        }
    }

    m_currentFrameOffset = completed.frameOffset;
    m_frameFractionalLeftover = completed.fractionalLeftover;
    m_pendingScale = {};
    const double readFramesProcessed = completed.effectiveRate * completed.outputFrames;
    if (m_hasDeferredScaleParameters) {
        applyScaleParameters(m_deferredScaleParameters);
        m_hasDeferredScaleParameters = false;
    }
    return readFramesProcessed;
}
