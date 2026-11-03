#include "engine/readaheadmanager.h"

#include <algorithm>

#include "audio/frame.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/controls/cuecontrol.h"
#include "engine/controls/loopingcontrol.h"
#include "engine/controls/ratecontrol.h"
#include "engine/engine.h"
#include "util/defs.h"
#include "util/sample.h"

namespace {
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

SINT ReadAheadManager::getNextSamples(double dRate,
        CSAMPLE* pOutput,
        SINT requestedSamples,
        mixxx::audio::ChannelCount channelCount) {
    cancelPendingRetry();
    return getNextSamplesInternal(
            dRate, pOutput, requestedSamples, channelCount, false)
            .samplesRead;
}

ReadAheadManager::NextSamplesResult ReadAheadManager::getNextSamplesWithRetry(
        double dRate,
        CSAMPLE* pOutput,
        SINT requestedSamples,
        mixxx::audio::ChannelCount channelCount) {
    if (!m_pReader) {
        return {getNextSamples(dRate, pOutput, requestedSamples, channelCount), false};
    }
    return getNextSamplesInternal(
            dRate, pOutput, requestedSamples, channelCount, true);
}

void ReadAheadManager::cancelPendingRetry() {
    m_pendingRetry.active = false;
    m_hasPendingReadHint = false;
}

ReadAheadManager::ReadPlan ReadAheadManager::makeReadPlan(bool inReverse,
        SINT requestSamples,
        SINT requestedSamples,
        mixxx::audio::ChannelCount channelCount) {
    ReadPlan plan;
    plan.active = true;
    plan.inReverse = inReverse;
    plan.requestPosition = m_currentPosition;
    plan.requestSamples = requestSamples;
    plan.channelCount = channelCount;

    // Loops and saved jumps can shorten a read, so retain their decision until
    // this exact input range has been read successfully.
    plan.loopTriggerPosition = m_pLoopingControl->nextTrigger(inReverse,
            mixxx::audio::FramePos::fromSamplePosMaybeInvalid(
                    m_currentPosition, channelCount),
            &plan.loopTargetPosition);
    const double loopTrigger =
            plan.loopTriggerPosition.toSamplePosMaybeInvalid(channelCount);
    plan.target = plan.loopTargetPosition.toSamplePosMaybeInvalid(channelCount);
    plan.targetPosition = plan.loopTargetPosition;
    plan.samplesFromReader = requestedSamples;

    if (loopTrigger != kNoTrigger) {
        plan.samplesToSeekTrigger = inReverse ? m_currentPosition - loopTrigger
                                              : loopTrigger - m_currentPosition;
        if (plan.samplesToSeekTrigger >= 0) {
            plan.preseekSamples = SampleUtil::ceilPlayPosToFrameStart(
                    plan.samplesToSeekTrigger, channelCount);
            if (plan.preseekSamples <= requestedSamples) {
                plan.reachedTrigger = true;
                plan.samplesFromReader = plan.preseekSamples;
            }
        }
    }

    plan.jumpTriggerPosition = m_pCueControl->nextTrigger(inReverse,
            mixxx::audio::FramePos::fromSamplePosMaybeInvalid(
                    m_currentPosition, channelCount),
            &plan.jumpTargetPosition,
            static_cast<mixxx::audio::FrameDiff_t>(requestedSamples / channelCount));
    double jumpTrigger =
            plan.jumpTriggerPosition.toSamplePosMaybeInvalid(channelCount);

    // If a loop and a saved jump cancel one another, no-op the jump to avoid
    // an infinite silent loop.
    if (jumpTrigger != kNoTrigger && loopTrigger != kNoTrigger &&
            plan.jumpTriggerPosition == plan.loopTargetPosition &&
            plan.loopTriggerPosition == plan.jumpTargetPosition) {
        jumpTrigger = kNoTrigger;
    }

    if (jumpTrigger != kNoTrigger) {
        const double samplesToJumpTrigger = inReverse
                ? m_currentPosition - jumpTrigger
                : jumpTrigger - m_currentPosition;
        if (samplesToJumpTrigger >= 0) {
            const SINT prejumpSamples = SampleUtil::ceilPlayPosToFrameStart(
                    samplesToJumpTrigger, channelCount);
            if (prejumpSamples <= requestedSamples) {
                plan.reachedTrigger = true;
                if (loopTrigger == kNoTrigger || prejumpSamples < plan.preseekSamples) {
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
    if (!plan.reachedTrigger) {
        return plan;
    }

    plan.positionAfterTrigger = plan.target;
    if (plan.preseekSamples > 0) {
        const double overshoot = plan.preseekSamples - plan.samplesToSeekTrigger;
        plan.positionAfterTrigger += inReverse ? -overshoot : overshoot;
    }
    const SINT seekReadPosition = SampleUtil::roundPlayPosToFrameStart(
            plan.positionAfterTrigger +
                    (inReverse ? plan.preseekSamples : -plan.preseekSamples),
            channelCount);

    const auto trackEnd = m_pLoopingControl->getTrackFrame();
    if (!trackEnd.isValid()) {
        return plan;
    }

    const SINT trackSamples = std::max<SINT>(0,
            SampleUtil::roundPlayPosToFrameStart(
                    trackEnd.toSamplePos(channelCount), channelCount));
    const SINT rangeStart = seekReadPosition -
            (inReverse ? plan.samplesFromReader : 0);
    const SINT rangeEnd = seekReadPosition +
            (inReverse ? 0 : plan.samplesFromReader);
    const SINT clippedStart = std::clamp<SINT>(rangeStart, 0, trackSamples);
    const SINT clippedEnd = std::clamp<SINT>(rangeEnd, 0, trackSamples);
    plan.crossFadeSamples = clippedEnd - clippedStart;
    plan.crossFadeStart = std::clamp<SINT>(
            inReverse ? rangeEnd - clippedEnd : clippedStart - rangeStart,
            0,
            plan.samplesFromReader);
    plan.crossFadeReadPosition = seekReadPosition +
            (inReverse ? -plan.crossFadeStart : plan.crossFadeStart);
    return plan;
}

ReadAheadManager::NextSamplesResult ReadAheadManager::getNextSamplesInternal(
        double dRate,
        CSAMPLE* pOutput,
        SINT requestedSamples,
        mixxx::audio::ChannelCount channelCount,
        bool retryOnCacheMiss) {
    const auto rejectRequest = [this] {
        cancelPendingRetry();
        return NextSamplesResult{0, false};
    };

    // Validate the interleaved-buffer contract before using channelCount or
    // touching either the reader staging buffer or crossfade storage.
    if (pOutput == nullptr) {
        return rejectRequest();
    }
    if (!channelCount.isValid() ||
            channelCount.value() > mixxx::kMaxEngineChannelInputCount.value()) {
        return rejectRequest();
    }
    const SINT maxRequestSamples = static_cast<SINT>(MAX_BUFFER_LEN) *
            static_cast<SINT>(channelCount.value());
    if (requestedSamples < 0 || requestedSamples > maxRequestSamples ||
            requestedSamples > kMaxCrossFadeSamples) {
        return rejectRequest();
    }
    if (requestedSamples % channelCount.value() != 0) {
        return rejectRequest();
    }

    const SINT requestSamples = requestedSamples;
    const bool inReverse = dRate < 0;
    const bool reusePending = retryOnCacheMiss &&
            m_pendingRetry.matches(m_currentPosition,
                    inReverse,
                    requestSamples,
                    channelCount);
    if (m_pendingRetry.active && !reusePending) {
        cancelPendingRetry();
    }
    ReadPlan plan = reusePending
            ? m_pendingRetry
            : makeReadPlan(inReverse, requestSamples, requestSamples, channelCount);

    VERIFY_OR_DEBUG_ASSERT(plan.samplesFromReader >= 0 &&
            plan.samplesFromReader <= maxRequestSamples &&
            plan.samplesFromReader <= kMaxCrossFadeSamples) {
        qDebug() << "Need negative samples in ReadAheadManager::getNextSamples. Ignoring read";
        return rejectRequest();
    }
    VERIFY_OR_DEBUG_ASSERT(plan.crossFadeStart >= 0 &&
            plan.crossFadeStart <= plan.samplesFromReader &&
            plan.crossFadeSamples >= 0 &&
            plan.crossFadeSamples <= kMaxCrossFadeSamples &&
            plan.crossFadeSamples <=
                    plan.samplesFromReader - plan.crossFadeStart) {
        return rejectRequest();
    }
    SINT crossFadeOutputOffset = 0;
    if (plan.crossFadeSamples > 0) {
        crossFadeOutputOffset = SampleUtil::ceilPlayPosToFrameStart(
                plan.crossFadeStart, channelCount);
        VERIFY_OR_DEBUG_ASSERT(crossFadeOutputOffset >= 0 &&
                crossFadeOutputOffset <= plan.samplesFromReader &&
                plan.crossFadeSamples <=
                        plan.samplesFromReader - crossFadeOutputOffset) {
            return rejectRequest();
        }
    }

    if (retryOnCacheMiss && !reusePending) {
        m_pendingRetry = plan;
    }
    const auto readResult = retryOnCacheMiss
            ? m_pReader->readWithRetry(plan.startSample,
                      plan.samplesFromReader,
                      plan.inReverse,
                      pOutput,
                      channelCount)
            : m_pReader->read(plan.startSample,
                      plan.samplesFromReader,
                      plan.inReverse,
                      pOutput,
                      channelCount);
    if (retryOnCacheMiss && readResult == CachingReader::ReadResult::UNAVAILABLE) {
        const SINT requestStartSample = plan.startSample -
                (plan.inReverse ? plan.samplesFromReader : 0);
        const SINT requestFrameCount = std::min<SINT>(
                plan.samplesFromReader / channelCount.value(), MAX_BUFFER_LEN);
        // A primary read can exceed the default two-chunk look-ahead. Hint the
        // full bounded plan so retries can load every missing chunk.
        m_pendingReadHint = {requestStartSample / channelCount.value(),
                requestFrameCount,
                Hint::Type::CurrentPosition};
        m_hasPendingReadHint = true;
        SampleUtil::clear(pOutput, requestSamples);
        ++m_cacheMissCount;
        return {0, true};
    }
    if (readResult == CachingReader::ReadResult::UNAVAILABLE) {
        SampleUtil::clear(pOutput, plan.samplesFromReader);
        ++m_cacheMissCount;
    } else if (readResult == CachingReader::ReadResult::PARTIALLY_UNAVAILABLE) {
        ++m_cacheMissCount;
    } else if (m_cacheMissCount > 0) {
        SampleUtil::applyRampingGain(pOutput,
                CSAMPLE_GAIN_ZERO,
                CSAMPLE_GAIN_ONE,
                plan.samplesFromReader);
        if (!m_cacheMissExpected) {
            qDebug() << "ReadAheadManager: continue after number cache misses:" << m_cacheMissCount;
        }
        m_cacheMissCount = 0;
        m_cacheMissExpected = false;
    }

    if (plan.reachedTrigger && plan.crossFadeSamples > 0) {
        const auto crossFadeResult = retryOnCacheMiss
                ? m_pReader->readWithRetry(plan.crossFadeReadPosition,
                          plan.crossFadeSamples,
                          plan.inReverse,
                          m_pCrossFadeBuffer,
                          channelCount)
                : m_pReader->read(plan.crossFadeReadPosition,
                          plan.crossFadeSamples,
                          plan.inReverse,
                          m_pCrossFadeBuffer,
                          channelCount);
        if (retryOnCacheMiss &&
                crossFadeResult == CachingReader::ReadResult::UNAVAILABLE) {
            m_pendingReadHint = {
                    (plan.crossFadeReadPosition -
                            (plan.inReverse ? plan.crossFadeSamples : 0)) /
                            channelCount,
                    plan.crossFadeSamples / channelCount,
                    Hint::Type::CurrentPosition};
            m_hasPendingReadHint = true;
            SampleUtil::clear(pOutput, requestSamples);
            ++m_cacheMissCount;
            return {0, true};
        }
        if (crossFadeResult == CachingReader::ReadResult::UNAVAILABLE) {
            SampleUtil::clear(m_pCrossFadeBuffer, plan.crossFadeSamples);
            ++m_cacheMissCount;
        } else if (crossFadeResult == CachingReader::ReadResult::PARTIALLY_UNAVAILABLE) {
            ++m_cacheMissCount;
        }
        SampleUtil::linearCrossfadeBuffersOut(
                pOutput + crossFadeOutputOffset,
                m_pCrossFadeBuffer,
                plan.crossFadeSamples,
                channelCount);
    } else if (plan.reachedTrigger) {
        SampleUtil::applyRampingGain(pOutput,
                CSAMPLE_GAIN_ONE,
                CSAMPLE_GAIN_ZERO,
                plan.samplesFromReader);
    }

    if (plan.inReverse) {
        addReadLogEntry(plan.requestPosition,
                plan.requestPosition - plan.samplesFromReader);
        m_currentPosition = plan.requestPosition - plan.samplesFromReader;
    } else {
        addReadLogEntry(plan.requestPosition,
                plan.requestPosition + plan.samplesFromReader);
        m_currentPosition = plan.requestPosition + plan.samplesFromReader;
    }
    if (plan.reachedTrigger) {
        DEBUG_ASSERT(plan.target != kNoTrigger);
        if (m_pRateControl) {
            m_pRateControl->notifyWrapAround(plan.loopTriggerPosition.isValid()
                            ? plan.loopTriggerPosition
                            : plan.jumpTriggerPosition,
                    plan.targetPosition);
        }
        m_currentPosition = plan.positionAfterTrigger;
    }

    if (retryOnCacheMiss) {
        m_pendingRetry.active = false;
        m_hasPendingReadHint = false;
    }
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
    m_readAheadLog.clear();
}

void ReadAheadManager::hintReader(double dRate,
        gsl::not_null<HintVector*> pHintList,
        mixxx::audio::ChannelCount channelCount) {
    if (m_hasPendingReadHint) {
        pHintList->append(m_pendingReadHint);
    }
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
            current_position.frame + current_position.frameCount < 0) {
        return;
    }

    // top priority, we need to read this data immediately
    current_position.type = Hint::Type::CurrentPosition;
    pHintList->append(current_position);
}

// Not thread-save, call from engine thread only
void ReadAheadManager::addReadLogEntry(double virtualPlaypositionStart,
        double virtualPlaypositionEndNonInclusive) {
    ReadLogEntry newEntry(virtualPlaypositionStart,
            virtualPlaypositionEndNonInclusive);
    if (m_readAheadLog.size() > 0) {
        ReadLogEntry& last = m_readAheadLog.back();
        if (last.merge(newEntry)) {
            return;
        }
    }
    m_readAheadLog.push_back(newEntry);
}

// Not thread-save, call from engine thread only
double ReadAheadManager::getFilePlaypositionFromLog(
        double currentFilePlayposition,
        double numConsumedSamples) {
    if (numConsumedSamples == 0) {
        return currentFilePlayposition;
    }

    if (m_readAheadLog.size() == 0) {
        // No log entries to read from.
        qDebug() << this << "No read ahead log entries to read from. Case not currently handled.";
        // TODO(rryan) log through a stats pipe eventually
        return currentFilePlayposition;
    }

    double filePlayposition = 0;
    while (m_readAheadLog.size() > 0 && numConsumedSamples > 0) {
        ReadLogEntry& entry = m_readAheadLog.front();
        // Advance our idea of the current virtual playposition to this
        // ReadLogEntry's start position.
        filePlayposition = entry.advancePlayposition(&numConsumedSamples);

        if (entry.length() == 0) {
            // This entry is empty now.
            m_readAheadLog.pop_front();
        }
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
