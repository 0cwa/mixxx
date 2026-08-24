#include "engine/bufferscalers/enginebufferscalesignalsmith.h"

#include <algorithm>
#include <cmath>

#include "engine/engine.h"
#include "engine/readaheadmanager.h"
#include "moc_enginebufferscalesignalsmith.cpp"
#include "util/assert.h"
#include "util/defs.h"
#include "util/sample.h"
#include "util/timer.h"

EngineBufferScaleSignalSmith::EngineBufferScaleSignalSmith(ReadAheadManager* pReadAheadManager)
        : m_pReadAheadManager(pReadAheadManager),
          m_buffers(),
          m_bufferPtrs(),
          m_outputBuffers(),
          m_outputBufferPtrs(),
          m_interleavedBuffer(mixxx::kMaxSupportedStems * MAX_BUFFER_LEN),
          m_frameFractionalLeftover(0),
          m_expectedFrameLatency(0),
          m_currentFrameOffset(0),
          m_bBackwards(false),
          m_currentPreset(Preset::Default) {
    onSignalChanged();
}

void EngineBufferScaleSignalSmith::setScaleParameters(
        double base_rate, double* pTempoRatio, double* pPitchRatio) {
    m_dBaseRate = base_rate;
    m_bBackwards = *pTempoRatio < 0;
    double speedAbs = std::fabs(*pTempoRatio);
    if (!std::isfinite(m_dBaseRate) || !std::isfinite(speedAbs)) {
        speedAbs = 0.0;
    } else if (speedAbs > MAX_SEEK_SPEED) {
        speedAbs = MAX_SEEK_SPEED;
    } else if (speedAbs < MIN_SEEK_SPEED) {
        speedAbs = 0.0;
    }

    *pTempoRatio = m_bBackwards ? -speedAbs : speedAbs;

    m_dTempoRatio = speedAbs;
    m_dPitchRatio = *pPitchRatio;
    m_effectiveRate = m_dBaseRate * m_dTempoRatio;

    if (std::isfinite(m_dPitchRatio) && m_dPitchRatio > 0.0) {
        m_stretch.setTransposeFactor(static_cast<float>(m_dPitchRatio));
    } else {
        m_dPitchRatio = 1.0;
        m_stretch.setTransposeFactor(1.0f);
    }
    m_stretch.setFormantFactor(1.0);

    // The following value is calculated from the block and interval samples
    // size, which are set in the above preset. It remains constant during the
    // stretcher process.
    // As documented in
    // https://signalsmith-audio.co.uk/code/stretch/#how-to-use-latency-starting-and-ending,
    // stretch factor should be used when computing total latency
    m_expectedFrameLatency =
            static_cast<SINT>(m_stretch.inputLatency()) +
            static_cast<SINT>(std::round(m_effectiveRate *
                    static_cast<double>(m_stretch.outputLatency())));
}

void EngineBufferScaleSignalSmith::onSignalChanged() {
    if (!getOutputSignal().isValid()) {
        return;
    }

    uint8_t channelCount = getOutputSignal().getChannelCount();
    if (m_buffers.size() != channelCount) {
        m_buffers.resize(channelCount);
    }

    if (m_bufferPtrs.size() != channelCount) {
        m_bufferPtrs.resize(channelCount);
    }
    if (m_outputBuffers.size() != channelCount) {
        m_outputBuffers.resize(channelCount);
    }
    if (m_outputBufferPtrs.size() != channelCount) {
        m_outputBufferPtrs.resize(channelCount);
    }

    for (int chIdx = 0; chIdx < channelCount; chIdx++) {
        if (m_buffers[chIdx].size() != MAX_BUFFER_LEN) {
            m_buffers[chIdx] = mixxx::SampleBuffer(MAX_BUFFER_LEN);
        }
        if (m_outputBuffers[chIdx].size() != MAX_BUFFER_LEN) {
            m_outputBuffers[chIdx] = mixxx::SampleBuffer(MAX_BUFFER_LEN);
        }
        // Keep the planar buffer pointer arrays in sync after vector resizes
        // and SampleBuffer reallocations. Signalsmith only sees these raw
        // pointers, so stale entries can make it read/write unrelated memory.
        m_bufferPtrs[chIdx] = m_buffers[chIdx].data();
        m_outputBufferPtrs[chIdx] = m_outputBuffers[chIdx].data();
    }

    // Configure stretcher with preset settings
    switch (m_currentPreset) {
    case Preset::Cheaper:
        m_stretch.presetCheaper(channelCount, getOutputSignal().getSampleRate());
        break;
    default:
        qWarning() << "Unsupported presset" << m_currentPreset << " so defaulting to default.";
        [[fallthrough]];
    case Preset::Default:
        m_stretch.presetDefault(channelCount, getOutputSignal().getSampleRate());
        break;
    }
    clear();
}

void EngineBufferScaleSignalSmith::clear() {
    if (m_pReadAheadManager) {
        m_pReadAheadManager->cancelPendingRetry();
    }
    m_stretch.reset();
    m_currentFrameOffset = 0;
    m_frameFractionalLeftover = 0;
}

EngineBufferScaleSignalSmith::InputReadResult
EngineBufferScaleSignalSmith::fetchAndDeinterleave(SINT sampleToRead, SINT frameOffset) {
    const auto readResult = m_pReadAheadManager->getNextSamplesWithRetry(
            // The value doesn't matter here. All that matters is we
            // are going forward or backward.
            (m_bBackwards ? -1 : 1) * m_dBaseRate * m_dTempoRatio,
            m_interleavedBuffer.data(),
            sampleToRead,
            getOutputSignal().getChannelCount());
    const auto frameRead = getOutputSignal().samples2frames(readResult.samplesRead);

    if (readResult.retryPending) {
        return {0, true};
    }

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

double EngineBufferScaleSignalSmith::scaleBuffer(CSAMPLE* pOutputBuffer, SINT iOutputBufferSize) {
    ScopedTimer t(QStringLiteral("EngineBufferScaleSignalsmith::scaleBuffer"));

    SINT currentFrameOffset = m_currentFrameOffset;
    double frameFractionalLeftover = m_frameFractionalLeftover;

    // Unlike RubberBand, SignalSmith Stretch always output as much audio as it
    // was given. However, it does introduce latency (documented at
    // https://signalsmith-audio.co.uk/code/stretch/#how-to-use-latency) which
    // initially lead to a silence. To compensate that, we need to use the
    // `.outputSeek` method, which allows to pre-roll samples a realign the actual
    // output to real time.
    // However, this method will reset the buffer so it can only be used right after a reset
    if (currentFrameOffset == 0 &&
            currentFrameOffset < m_expectedFrameLatency
            // If the track has a zero rate, we skip correction as this is
            // usually a sign that the track is not playing. This will likely
            // create undesired silence (as opposite to a "zero BPM" play affect
            // if a track start playing with a zero BPM, but this is an
            // acceptable trade off for now)
            && m_dTempoRatio > 0) {
        const auto readResult = fetchAndDeinterleave(getOutputSignal().frames2samples(
                std::min(m_expectedFrameLatency - m_currentFrameOffset,
                        SINT(MAX_BUFFER_LEN))));
        if (readResult.retryPending) {
            SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
            return 0.0;
        }
        m_stretch.outputSeek(m_bufferPtrs.data(), readResult.framesRead);
        currentFrameOffset += readResult.framesRead;
        // outputSeek changes the stretcher immediately. Keep this successful
        // preroll committed if a later input read needs to be retried.
        m_currentFrameOffset = currentFrameOffset;
    }

    const SINT outputFrames = getOutputSignal().samples2frames(iOutputBufferSize);
    auto dFrameRequired =
            (m_dBaseRate * m_dTempoRatio * static_cast<double>(outputFrames)) +
            frameFractionalLeftover;

    if (!std::isfinite(dFrameRequired) || dFrameRequired <= 0.0) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        return 0.0;
    }

    if (currentFrameOffset != m_expectedFrameLatency && dFrameRequired > 0) {
        // This happens when the rate changes because the rate scales the input
        // latency. We need more or less latency frames to keep the output steady.
        // Avoid applying the whole latency delta in one callback. That creates
        // a one-buffer input spike for Signalsmith and can sound like a
        // high-pitched chirp.
        const double latencyDelta =
                static_cast<double>(m_expectedFrameLatency - currentFrameOffset);
        const double maxCorrection =
                static_cast<double>(std::min<SINT>(outputFrames, MAX_BUFFER_LEN));
        double frameOffset = 0.0;
        if (latencyDelta > 0.0) {
            const double inputRoom = static_cast<double>(MAX_BUFFER_LEN) - dFrameRequired;
            frameOffset = std::min({latencyDelta, maxCorrection, inputRoom});
            frameOffset = std::max(0.0, frameOffset);
        } else {
            frameOffset = -std::min({-latencyDelta, maxCorrection, dFrameRequired});
        }
        dFrameRequired += frameOffset;
        currentFrameOffset += static_cast<SINT>(frameOffset);
    }

    const SINT frameRequired = static_cast<SINT>(dFrameRequired);
    VERIFY_OR_DEBUG_ASSERT(frameRequired <= MAX_BUFFER_LEN && outputFrames <= MAX_BUFFER_LEN) {
        SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
        return 0.0;
    }

    frameFractionalLeftover = dFrameRequired - static_cast<double>(frameRequired);
    DEBUG_ASSERT(0 <= frameFractionalLeftover && frameFractionalLeftover < 1);

    SINT frameRead = 0;
    while (frameRead < frameRequired) {
        const auto readResult = fetchAndDeinterleave(
                getOutputSignal().frames2samples(frameRequired - frameRead), frameRead);

        if (readResult.retryPending) {
            SampleUtil::clear(pOutputBuffer, iOutputBufferSize);
            return 0.0;
        }

        const SINT currentFrameRead = readResult.framesRead;

        if (currentFrameRead <= 0) {
            // Do not leave stale planar samples in the unread tail. EOF or
            // starvation can occur after a partial read, and retrying forever
            // would spin if the read-ahead manager keeps returning nothing.
            for (int ch = 0; ch < getOutputSignal().getChannelCount(); ch++) {
                SampleUtil::clear(m_buffers[ch].data(frameRead), frameRequired - frameRead);
            }
            frameRead = frameRequired;
            break;
        }

        frameRead += currentFrameRead;
    }

    DEBUG_ASSERT(frameRead == frameRequired);

    {
        ScopedTimer t(QStringLiteral("Signalsmith::process"));
        m_stretch.process(
                m_bufferPtrs.data(), frameRead, m_outputBufferPtrs.data(), outputFrames);
    }

    auto outputFrameSize = getOutputSignal().samples2frames(iOutputBufferSize);
    switch (getOutputSignal().getChannelCount()) {
    case mixxx::audio::ChannelCount::stereo():
        SampleUtil::interleaveBuffer(pOutputBuffer,
                m_outputBuffers[0].data(),
                m_outputBuffers[1].data(),
                outputFrameSize);
        break;
    case mixxx::audio::ChannelCount::stem():
        SampleUtil::interleaveBuffer(pOutputBuffer,
                m_outputBuffers[0].data(),
                m_outputBuffers[1].data(),
                m_outputBuffers[2].data(),
                m_outputBuffers[3].data(),
                m_outputBuffers[4].data(),
                m_outputBuffers[5].data(),
                m_outputBuffers[6].data(),
                m_outputBuffers[7].data(),
                outputFrameSize);
        break;
    default: {
        int chCount = getOutputSignal().getChannelCount();
        // The buffers samples are ordered as following
        //  m_buffers#1 = 11..
        //  m_buffers#2 = 22..
        //  m_buffers#3 = 33..
        //  m_buffers#4 = 44..
        //  m_buffers#X = XX..
        // And need to be reordered as following in pBuffer
        //  1234..X1234...X...
        //
        // Because of the unanticipated number of buffer and channel, we cannot
        // use any SampleUtil in this case
        for (SINT frameIdx = 0;
                frameIdx < getOutputSignal().samples2frames(iOutputBufferSize);
                ++frameIdx) {
            for (int channel = 0; channel < chCount; channel++) {
                pOutputBuffer[frameIdx * chCount + channel] =
                        m_outputBuffers[channel].data()[frameIdx];
            }
        }
    } break;
    }

    m_currentFrameOffset = currentFrameOffset;
    m_frameFractionalLeftover = frameFractionalLeftover;

    // readFramesProcessed is interpreted as the total number of frames
    // consumed to produce the scaled buffer. Due to this, we do not take into
    // account directionality or starting point.
    return m_effectiveRate * outputFrames;
}
