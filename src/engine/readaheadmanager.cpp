#include "engine/readaheadmanager.h"

#include <algorithm>

#include "audio/frame.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/controls/cuecontrol.h"
#include "engine/controls/loopingcontrol.h"
#include "engine/controls/ratecontrol.h"
#include "engine/engine.h"
#include "util/assert.h"
#include "util/defs.h"
#include "util/sample.h"

namespace {
// Crossfade lengths are interleaved sample counts. A caller can request up to
// MAX_BUFFER_LEN frames for the engine's maximum supported channel layout.
constexpr SINT kMaxCrossFadeSamples =
        static_cast<SINT>(MAX_BUFFER_LEN) * mixxx::kMaxEngineChannelInputCount;
} // namespace

ReadAheadManager::ReadAheadManager()
        : m_pLoopingControl(nullptr),
          m_pCueControl(nullptr),
          m_pRateControl(nullptr),
          m_currentPosition(0),
          m_pReader(nullptr),
          m_pCrossFadeBuffer(SampleUtil::alloc(kMaxCrossFadeSamples)),
          m_cacheMissCount(0),
          m_cacheMissExpected(false) {
    // For testing only: ReadAheadManagerMock
}

ReadAheadManager::ReadAheadManager(CachingReader* pReader,
        LoopingControl* pLoopingControl,
        CueControl* pCueControl)
        : m_pLoopingControl(pLoopingControl),
          m_pCueControl(pCueControl),
          m_pRateControl(nullptr),
          m_currentPosition(0),
          m_pReader(pReader),
          m_pCrossFadeBuffer(SampleUtil::alloc(kMaxCrossFadeSamples)),
          m_cacheMissCount(0),
          m_cacheMissExpected(false) {
    DEBUG_ASSERT(m_pLoopingControl != nullptr);
    DEBUG_ASSERT(m_pCueControl != nullptr);
    DEBUG_ASSERT(m_pReader != nullptr);
}

ReadAheadManager::~ReadAheadManager() {
    SampleUtil::free(m_pCrossFadeBuffer);
}

ReadAheadManager::NextSamplesResult ReadAheadManager::getNextSamplesWithRetry(
        double dRate,
        CSAMPLE* pOutput,
        SINT requested_samples,
        mixxx::audio::ChannelCount channelCount) {
    return getNextSamplesInternal(
            dRate, pOutput, requested_samples, channelCount, true, &m_pendingRetry);
}

ReadAheadManager::NextSamplesResult ReadAheadManager::getNextSamplesWithRetry(
        double dRate,
        CSAMPLE* pOutput,
        SINT requested_samples,
        mixxx::audio::ChannelCount channelCount,
        RetryState& retryState) {
    return getNextSamplesInternal(
            dRate, pOutput, requested_samples, channelCount, true, &retryState);
}

void ReadAheadManager::cancelPendingRetry() {
    m_pendingRetry.active = false;
}

void ReadAheadManager::cancelPendingRetry(RetryState& retryState) {
    retryState.active = false;
}

SINT ReadAheadManager::getNextSamples(double dRate,
        CSAMPLE* pOutput,
        SINT requested_samples,
        mixxx::audio::ChannelCount channelCount) {
    return getNextSamplesInternal(
            dRate, pOutput, requested_samples, channelCount, false, nullptr)
            .samplesRead;
}

ReadAheadManager::RetryState ReadAheadManager::makeReadPlan(bool inReverse,
        SINT requestSamples,
        SINT requestedSamples,
        mixxx::audio::ChannelCount channelCount) {
    RetryState plan;
    plan.active = true;
    plan.inReverse = inReverse;
    plan.requestPosition = m_currentPosition;
    plan.requestSamples = requestSamples;
    plan.requestedSamples = requestedSamples;
    plan.channelCount = channelCount;

    // A loop (beat loop or track on repeat) will only limit the amount we
    // can read in one shot.
    plan.loopTriggerPosition =
            m_pLoopingControl->nextTrigger(inReverse,
                    mixxx::audio::FramePos::fromSamplePosMaybeInvalid(
                            m_currentPosition, channelCount),
                    &plan.loopTargetPosition);
    const double loopTrigger =
            plan.loopTriggerPosition.toSamplePosMaybeInvalid(channelCount);
    plan.target = plan.loopTargetPosition.toSamplePosMaybeInvalid(channelCount);
    plan.targetPosition = plan.loopTargetPosition;

    // By default, we are reading as many samples as requested.
    plan.samplesFromReader = requestedSamples;
    if (loopTrigger != kNoTrigger) {
        plan.samplesToSeekTrigger = inReverse
                ? m_currentPosition - loopTrigger
                : loopTrigger - m_currentPosition;
        if (plan.samplesToSeekTrigger >= 0.0) {
            // We can only read whole frames from the reader. Use ceil here, to
            // be sure to reach the loop trigger.
            plan.preseekSamples = SampleUtil::ceilPlayPosToFrameStart(
                    plan.samplesToSeekTrigger, channelCount);
            if (plan.preseekSamples <= requestedSamples) {
                plan.reachedTrigger = true;
                plan.samplesFromReader = plan.preseekSamples;
            }
        }
    }

    // A saved jump cue will only limit the amount we can read in one shot.
    plan.jumpTriggerPosition =
            m_pCueControl->nextTrigger(inReverse,
                    mixxx::audio::FramePos::fromSamplePosMaybeInvalid(
                            m_currentPosition, channelCount),
                    &plan.jumpTargetPosition,
                    static_cast<mixxx::audio::FrameDiff_t>(
                            requestedSamples / channelCount));
    double jumpTrigger =
            plan.jumpTriggerPosition.toSamplePosMaybeInvalid(channelCount);

    // If there is both a loop and saved jump that are armed, and they both
    // cancel each other (Loop from A -> B, jump from A -> B), we no-op the jump
    // to prevent an infinite silent play loop.
    if (jumpTrigger != kNoTrigger && loopTrigger != kNoTrigger &&
            plan.jumpTriggerPosition == plan.loopTargetPosition &&
            plan.loopTriggerPosition == plan.jumpTargetPosition) {
        jumpTrigger = kNoTrigger;
    }

    if (jumpTrigger != kNoTrigger) {
        const double samplesToJumpTrigger = inReverse
                ? m_currentPosition - jumpTrigger
                : jumpTrigger - m_currentPosition;
        if (samplesToJumpTrigger >= 0.0) {
            const SINT prejumpSamples = SampleUtil::ceilPlayPosToFrameStart(
                    samplesToJumpTrigger, channelCount);
            if (prejumpSamples <= requestedSamples) {
                plan.reachedTrigger = true;
                // A loop end may be before the jump. If the jump is first, this
                // should be our new target.
                if (loopTrigger == kNoTrigger ||
                        prejumpSamples < plan.preseekSamples) {
                    plan.samplesFromReader = prejumpSamples;
                    plan.preseekSamples = prejumpSamples;
                    plan.samplesToSeekTrigger = samplesToJumpTrigger;
                    plan.target =
                            plan.jumpTargetPosition.toSamplePosMaybeInvalid(channelCount);
                    plan.targetPosition = plan.jumpTargetPosition;
                }
            }
        }
    }

    plan.startSample = SampleUtil::roundPlayPosToFrameStart(
            m_currentPosition, channelCount);
    if (plan.reachedTrigger) {
        plan.positionAfterTrigger = plan.target;
        if (plan.preseekSamples > 0) {
            // Compensate for reading up to one frame past the trigger so the
            // loop or saved jump retains its intended length.
            plan.positionAfterTrigger +=
                    plan.preseekSamples - plan.samplesToSeekTrigger;
        }
        plan.seekReadPosition = SampleUtil::roundPlayPosToFrameStart(
                plan.positionAfterTrigger +
                        (plan.inReverse
                                        ? plan.preseekSamples
                                        : -plan.preseekSamples),
                channelCount);
        plan.crossFadeSamples = plan.samplesFromReader;
        if (plan.seekReadPosition < 0) {
            plan.crossFadeStart = -plan.seekReadPosition;
            plan.crossFadeSamples -= plan.crossFadeStart;
        } else {
            const int trackSamples = static_cast<int>(
                    m_pLoopingControl->getTrackFrame().toSamplePos(channelCount));
            if (plan.seekReadPosition > trackSamples) {
                plan.crossFadeStart = plan.seekReadPosition - trackSamples;
                plan.crossFadeSamples -= plan.crossFadeStart;
            }
        }
        plan.crossFadeReadPosition = plan.seekReadPosition +
                (plan.inReverse ? plan.crossFadeStart : -plan.crossFadeStart);
    }
    return plan;
}

ReadAheadManager::NextSamplesResult ReadAheadManager::getNextSamplesInternal(
        double dRate,
        CSAMPLE* pOutput,
        SINT requested_samples,
        mixxx::audio::ChannelCount channelCount,
        bool retryOnCacheMiss,
        RetryState* pRetryState) {
    // qDebug() << "getNextSamples:" << m_currentPosition << requested_samples;

    const SINT requestSamples = requested_samples;
    int modSamples = requested_samples % channelCount;
    if (modSamples != 0) {
        qDebug() << "ERROR: Non-aligned requested_samples to ReadAheadManager::getNextSamples";
        requested_samples -= modSamples;
    }
    const bool inReverse = dRate < 0;
    const bool reusePendingRetry = retryOnCacheMiss && pRetryState &&
            pRetryState->matches(m_currentPosition,
                    inReverse,
                    requestSamples,
                    channelCount);
    if (pRetryState && pRetryState->active && !reusePendingRetry) {
        pRetryState->active = false;
    }

    RetryState plan = reusePendingRetry
            ? *pRetryState
            : makeReadPlan(
                      inReverse, requestSamples, requested_samples, channelCount);

    // Sanity checks.
    VERIFY_OR_DEBUG_ASSERT(plan.samplesFromReader >= 0) {
        qDebug() << "Need negative samples in ReadAheadManager::getNextSamples. Ignoring read";
        return {0, false};
    }

    if (retryOnCacheMiss && pRetryState && !reusePendingRetry) {
        *pRetryState = plan;
    }

    const double readLogEnd = plan.inReverse
            ? m_currentPosition - plan.samplesFromReader
            : m_currentPosition + plan.samplesFromReader;
    if (!canAddReadLogEntry(m_currentPosition, readLogEnd)) {
        // Read-ahead capacity is not a cache miss. Report a bounded empty read
        // so scalers can pad their output and EngineBuffer can consume older
        // mappings. Keeping retryPending set here would leave grain scalers
        // retrying this same request forever because no reader call was made.
        SampleUtil::clear(
                pOutput,
                retryOnCacheMiss ? plan.requestSamples : plan.samplesFromReader);
        if (pRetryState) {
            pRetryState->active = false;
        }
        return {0, false};
    }

    const auto readResult = retryOnCacheMiss
            ? m_pReader->readWithRetry(
                      plan.startSample,
                      plan.samplesFromReader,
                      plan.inReverse,
                      pOutput,
                      channelCount)
            : m_pReader->read(
                      plan.startSample,
                      plan.samplesFromReader,
                      plan.inReverse,
                      pOutput,
                      channelCount);
    if (readResult == CachingReader::ReadResult::UNAVAILABLE) {
        // Cache miss - no samples written
        SampleUtil::clear(pOutput,
                retryOnCacheMiss ? plan.requestSamples : plan.samplesFromReader);
        // Set the cache miss flag to decide when to apply ramping
        // after the following read attempts.
        m_cacheMissCount++;
        if (retryOnCacheMiss) {
            // Do not advance the read-ahead cursor when a grain-based scaler
            // asks for a retryable read. Advancing here would make its next
            // attempt label the newly available chunk with the wrong absolute
            // frame position.
            return {0, true};
        }
    } else if (m_cacheMissCount > 0) {
        // Previous read was a cache miss, but now we got something back.
        // Apply ramping gain, because the last buffer has unwanted silence
        // and new samples without fading are causing a pop.
        SampleUtil::applyRampingGain(pOutput,
                CSAMPLE_GAIN_ZERO,
                CSAMPLE_GAIN_ONE,
                plan.samplesFromReader);
        // Reset the cache miss flag, because we are now back on track.
        if (!m_cacheMissExpected) {
            qDebug() << "ReadAheadManager: continue after number cache misses:" << m_cacheMissCount;
        }
        m_cacheMissCount = 0;
        m_cacheMissExpected = false;
    }

    // Increment or decrement current read-ahead position
    // Mixing int and double here is desired, because the fractional frame should
    // be resist
    if (!addReadLogEntry(m_currentPosition, readLogEnd)) {
        // This is unreachable while the admission check and append remain
        // adjacent on the engine thread. Keep the fallback safe if that
        // invariant changes: discard the newly read samples and do not move
        // the cursor without a mapping.
        SampleUtil::clear(
                pOutput,
                retryOnCacheMiss ? plan.requestSamples : plan.samplesFromReader);
        if (pRetryState) {
            pRetryState->active = false;
        }
        return {0, false};
    }
    if (plan.inReverse) {
        m_currentPosition -= plan.samplesFromReader;
    } else {
        m_currentPosition += plan.samplesFromReader;
    }

    // Activate on this trigger if necessary
    if (plan.reachedTrigger) {
        DEBUG_ASSERT(plan.target != kNoTrigger);
        if (m_pRateControl) {
            m_pRateControl->notifyWrapAround(plan.loopTriggerPosition.isValid()
                            ? plan.loopTriggerPosition
                            : plan.jumpTriggerPosition,
                    plan.targetPosition);
        }
        // TODO probably also useful for hotcue_X_indicator in CueControl::updateIndicators()

        // Jump to other end of loop or track.
        m_currentPosition = plan.positionAfterTrigger;

        if (plan.crossFadeSamples > 0) {
            const auto readResult = m_pReader->read(plan.crossFadeReadPosition,
                    plan.crossFadeSamples,
                    plan.inReverse,
                    m_pCrossFadeBuffer,
                    channelCount);
            if (readResult == CachingReader::ReadResult::UNAVAILABLE) {
                qDebug() << "ERROR: Couldn't get all needed samples for crossfade.";
                // Cache miss - no samples written
                SampleUtil::clear(
                        m_pCrossFadeBuffer, plan.samplesFromReader);
                // Set the cache miss flag to decide when to apply ramping
                // after the following read attempts.
                m_cacheMissCount++;
            }

            // do crossfade from the current buffer into the new loop beginning
            if (plan.samplesFromReader != 0) { // avoid division by zero
                SampleUtil::linearCrossfadeBuffersOut(pOutput +
                                SampleUtil::ceilPlayPosToFrameStart(
                                        plan.crossFadeStart, channelCount),
                        m_pCrossFadeBuffer,
                        plan.crossFadeSamples,
                        channelCount);
            }
        } else {
            // No samples for crossfading, ramp to zero
            SampleUtil::applyRampingGain(pOutput,
                    CSAMPLE_GAIN_ONE,
                    CSAMPLE_GAIN_ZERO,
                    plan.samplesFromReader);
        }
    }

    if (retryOnCacheMiss) {
        DEBUG_ASSERT(pRetryState);
        pRetryState->active = false;
    }

    // qDebug() << "read" << m_currentPosition << plan.samplesFromReader;
    return {plan.samplesFromReader, false};
}

void ReadAheadManager::addRateControl(RateControl* pRateControl) {
    m_pRateControl = pRateControl;
}

// Not thread-save, call from engine thread only
void ReadAheadManager::notifySeek(double seekPosition) {
    cancelPendingRetry();
    m_currentPosition = seekPosition;
    m_cacheMissCount = 0;
    m_cacheMissExpected = true;
    m_readAheadLogStart = 0;
    m_readAheadLogSize = 0;
    m_readAheadLogOverflowSize = 0;
}

void ReadAheadManager::hintReader(double dRate,
        gsl::not_null<HintVector*> pHintList,
        mixxx::audio::ChannelCount channelCount) {
    bool in_reverse = dRate < 0;
    Hint current_position;

    // SoundTouch can read up to 2 chunks ahead. Always keep 2 chunks ahead in
    // cache.
    SINT frameCountToCache = 2 * CachingReaderChunk::kFrames;
    current_position.frameCount = frameCountToCache;

    // this called after the precious chunk was consumed
    if (in_reverse) {
        current_position.frame =
                static_cast<SINT>(ceil(m_currentPosition / channelCount)) -
                frameCountToCache;
    } else {
        current_position.frame =
                static_cast<SINT>(floor(m_currentPosition / channelCount));
    }

    // If we are trying to cache before the start of the track,
    // Then we don't need to cache because it's all zeros!
    if (current_position.frame < 0 &&
            current_position.frame + current_position.frameCount < 0)
    {
    	return;
    }

    // top priority, we need to read this data immediately
    current_position.type = Hint::Type::CurrentPosition;
    pHintList->append(current_position);
}

// Not thread-save, call from engine thread only
bool ReadAheadManager::canAddReadLogEntry(
        double virtualPlaypositionStart,
        double virtualPlaypositionEndNonInclusive) const {
    ReadLogEntry newEntry(virtualPlaypositionStart,
            virtualPlaypositionEndNonInclusive);
    if (m_readAheadLogOverflowSize > 0) {
        const ReadLogEntry& last = m_readAheadLogOverflow
                [m_readAheadLogOverflowSize - 1];
        if (last.canMerge(newEntry)) {
            return true;
        }
    }
    if (m_readAheadLogOverflowSize == 0 && m_readAheadLogSize > 0) {
        const ReadLogEntry& last =
                m_readAheadLog[m_readAheadLogStart + m_readAheadLogSize - 1];
        if (last.canMerge(newEntry)) {
            return true;
        }
    }
    return m_readAheadLogSize < m_readAheadLog.size() ||
            m_readAheadLogOverflowSize < m_readAheadLogOverflow.size();
}

bool ReadAheadManager::addReadLogEntry(double virtualPlaypositionStart,
        double virtualPlaypositionEndNonInclusive) {
    ReadLogEntry newEntry(virtualPlaypositionStart,
            virtualPlaypositionEndNonInclusive);
    if (m_readAheadLogOverflowSize > 0) {
        ReadLogEntry& last = m_readAheadLogOverflow
                [m_readAheadLogOverflowSize - 1];
        if (last.merge(newEntry)) {
            return true;
        }

        if (m_readAheadLogStart > 0) {
            for (std::size_t i = 0; i < m_readAheadLogSize; ++i) {
                m_readAheadLog[i] = m_readAheadLog[m_readAheadLogStart + i];
            }
            m_readAheadLogStart = 0;
        }

        // Promote as many queued spill mappings as fit before appending the
        // new mapping. This preserves their FIFO order when the main log has
        // room but the spill queue still contains more than one entry.
        while (m_readAheadLogSize < m_readAheadLog.size() &&
                m_readAheadLogOverflowSize > 0) {
            m_readAheadLog[m_readAheadLogSize] = m_readAheadLogOverflow[0];
            ++m_readAheadLogSize;
            for (std::size_t i = 1; i < m_readAheadLogOverflowSize; ++i) {
                m_readAheadLogOverflow[i - 1] = m_readAheadLogOverflow[i];
            }
            --m_readAheadLogOverflowSize;
        }
    }

    if (m_readAheadLogOverflowSize == 0 && m_readAheadLogSize > 0) {
        ReadLogEntry& last =
                m_readAheadLog[m_readAheadLogStart + m_readAheadLogSize - 1];
        if (last.merge(newEntry)) {
            return true;
        }
    }

    const auto nextIndex = m_readAheadLogStart + m_readAheadLogSize;
    if (nextIndex == m_readAheadLog.size()) {
        if (m_readAheadLogStart > 0) {
            std::move(m_readAheadLog.begin() + m_readAheadLogStart,
                    m_readAheadLog.begin() + nextIndex,
                    m_readAheadLog.begin());
            m_readAheadLogStart = 0;
        }
    }

    const auto entryIndex = m_readAheadLogStart + m_readAheadLogSize;
    if (entryIndex < m_readAheadLog.size()) {
        m_readAheadLog[entryIndex] = newEntry;
        ++m_readAheadLogSize;
        return true;
    }

    if (m_readAheadLogOverflowSize == m_readAheadLogOverflow.size()) {
        return false;
    }
    m_readAheadLogOverflow[m_readAheadLogOverflowSize] = newEntry;
    ++m_readAheadLogOverflowSize;
    return true;
}

// Not thread-save, call from engine thread only
double ReadAheadManager::getFilePlaypositionFromLog(
        double currentFilePlayposition,
        double numConsumedSamples) {
    if (numConsumedSamples == 0) {
        return currentFilePlayposition;
    }

    if (m_readAheadLogSize == 0 && m_readAheadLogOverflowSize == 0) {
        // No log entries to read from.
        qDebug() << this << "No read ahead log entries to read from. Case not currently handled.";
        // TODO(rryan) log through a stats pipe eventually
        return currentFilePlayposition;
    }

    double filePlayposition = 0;
    while ((m_readAheadLogSize > 0 || m_readAheadLogOverflowSize > 0) &&
            numConsumedSamples > 0) {
        ReadLogEntry& entry = m_readAheadLogSize > 0
                ? m_readAheadLog[m_readAheadLogStart]
                : m_readAheadLogOverflow[0];
        // Advance our idea of the current virtual playposition to this
        // ReadLogEntry's start position.
        filePlayposition = entry.advancePlayposition(&numConsumedSamples);

        if (entry.length() == 0) {
            // This entry is empty now.
            if (m_readAheadLogSize > 0) {
                ++m_readAheadLogStart;
                --m_readAheadLogSize;
            } else {
                for (std::size_t i = 1; i < m_readAheadLogOverflowSize; ++i) {
                    m_readAheadLogOverflow[i - 1] = m_readAheadLogOverflow[i];
                }
                --m_readAheadLogOverflowSize;
            }
        }
    }

    if (m_readAheadLogSize == 0) {
        m_readAheadLogStart = 0;
    }

    return filePlayposition;
}

mixxx::audio::FramePos ReadAheadManager::getFilePlaypositionFromLog(
        mixxx::audio::FramePos currentPosition,
        mixxx::audio::FrameDiff_t numConsumedFrames,
        mixxx::audio::ChannelCount channelCount) {
    const double positionSamples =
            getFilePlaypositionFromLog(currentPosition.toSamplePos(channelCount),
                    numConsumedFrames * channelCount);
    return mixxx::audio::FramePos::fromSamplePos(positionSamples, channelCount);
}
