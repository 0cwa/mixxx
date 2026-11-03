#pragma once

#include <gsl/pointers>
#include <list>

#include "audio/frame.h"
#include "engine/cachingreader/cachingreader.h"
#include "util/math.h"
#include "util/types.h"

class LoopingControl;
class CueControl;
class RateControl;

/// ReadAheadManager is a tool for keeping track of the engine's current position
/// in a file. In the case that the engine needs to read ahead of the current
/// play position (for example, to feed more samples into a library like
/// SoundTouch) then this will keep track of how many samples the engine has
/// consumed. The getNextSamples() method encapsulates the logic of determining
/// whether to take a loop or jump into a single method. Whenever the Engine
/// seeks or the current play position is invalidated somehow, the Engine must
/// call notifySeek to inform the ReadAheadManager to reset itself to the seek
/// point.
class ReadAheadManager {
  public:
    struct NextSamplesResult {
        SINT samplesRead;
        bool retryPending;
    };

    ReadAheadManager(); // Only for testing: ReadAheadManagerMock
    ReadAheadManager(CachingReader* reader,
            LoopingControl* pLoopingControl,
            CueControl* pCueControl);
    virtual ~ReadAheadManager();

    /// Call this method to fill buffer with requested_samples out of the
    /// lookahead buffer. Provide rate as dRate so that the manager knows the
    /// direction the audio is progressing in. Returns the total number of
    /// samples read into buffer. Note that it is very common that the total
    /// samples read is less than the requested number of samples. Requests
    /// must use a supported channel count, be frame-aligned, and fit the fixed
    /// reader and crossfade buffers. Invalid requests return zero without
    /// modifying the destination buffer.
    virtual SINT getNextSamples(double dRate,
            CSAMPLE* buffer,
            SINT requested_samples,
            mixxx::audio::ChannelCount channelCount);

    // Read input for a stretcher without advancing past a cache miss. A
    // derived test manager that only overrides getNextSamples() keeps its
    // existing behavior through this default implementation.
    virtual NextSamplesResult getNextSamplesWithRetry(double dRate,
            CSAMPLE* buffer,
            SINT requested_samples,
            mixxx::audio::ChannelCount channelCount);

    // Discard a retry plan when its scaler is reset or seeks elsewhere.
    virtual void cancelPendingRetry();

    /// Used to add a new EngineControls that ReadAheadManager will use to decide
    /// which samples to return.
    void addLoopingControl();
    void addRateControl(RateControl* pRateControl);

    /// Get the current read-ahead position in samples.
    /// unused in Mixxx, but needed for testing
    virtual inline double getPlaypos() const {
        return m_currentPosition;
    }

    virtual void notifySeek(double seekPosition);

    /// hintReader allows the ReadAheadManager to provide hints to the reader to
    /// indicate that the given portion of a song is about to be read.
    virtual void hintReader(double dRate,
            gsl::not_null<HintVector*> pHintList,
            mixxx::audio::ChannelCount channelCount);

    /// Return the position in sample
    virtual double getFilePlaypositionFromLog(
            double currentFilePlayposition,
            double numConsumedSamples);
    /// Return the position in frame
    mixxx::audio::FramePos getFilePlaypositionFromLog(
            mixxx::audio::FramePos currentPosition,
            mixxx::audio::FrameDiff_t numConsumedFrames,
            mixxx::audio::ChannelCount channelCount);

  private:
    struct ReadPlan {
        bool active{false};
        bool inReverse{false};
        bool reachedTrigger{false};
        double requestPosition{0.0};
        double target{0.0};
        double positionAfterTrigger{0.0};
        double samplesToSeekTrigger{0.0};
        SINT requestSamples{0};
        SINT samplesFromReader{0};
        SINT preseekSamples{0};
        SINT startSample{0};
        SINT crossFadeStart{0};
        SINT crossFadeSamples{0};
        SINT crossFadeReadPosition{0};
        mixxx::audio::ChannelCount channelCount;
        mixxx::audio::FramePos loopTriggerPosition;
        mixxx::audio::FramePos loopTargetPosition;
        mixxx::audio::FramePos jumpTriggerPosition;
        mixxx::audio::FramePos jumpTargetPosition;
        mixxx::audio::FramePos targetPosition;

        bool matches(double position,
                bool reverse,
                SINT samples,
                mixxx::audio::ChannelCount channels) const {
            return active && requestPosition == position &&
                    inReverse == reverse && requestSamples == samples &&
                    channelCount.value() == channels.value();
        }
    };

    ReadPlan makeReadPlan(bool inReverse,
            SINT requestSamples,
            SINT requestedSamples,
            mixxx::audio::ChannelCount channelCount);
    NextSamplesResult getNextSamplesInternal(double dRate,
            CSAMPLE* buffer,
            SINT requestedSamples,
            mixxx::audio::ChannelCount channelCount,
            bool retryOnCacheMiss);

    /// An entry in the read log indicates the virtual playposition the read
    /// began at and the virtual playposition it ended at.
    struct ReadLogEntry {
        double virtualPlaypositionStart;
        double virtualPlaypositionEndNonInclusive;

        ReadLogEntry(double virtualPlaypositionStart,
                double virtualPlaypositionEndNonInclusive) {
            this->virtualPlaypositionStart = virtualPlaypositionStart;
            this->virtualPlaypositionEndNonInclusive =
                    virtualPlaypositionEndNonInclusive;
        }

        bool direction() const {
            // NOTE(rryan): We try to avoid 0-length ReadLogEntry's when
            // possible but they have happened in the past. We treat 0-length
            // ReadLogEntry's as forward reads because this prevents them from
            // being interpreted as a seek in the common case.
            return virtualPlaypositionStart <= virtualPlaypositionEndNonInclusive;
        }

        double length() const {
            return fabs(virtualPlaypositionEndNonInclusive -
                    virtualPlaypositionStart);
        }

        /// Moves the start position forward or backward (depending on
        /// direction()) by numSamples.
        /// Caller should check if length() is 0 after consumption in
        /// order to expire the ReadLogEntry.
        double advancePlayposition(double* pNumConsumedSamples) {
            double available = math_min(*pNumConsumedSamples, length());
            virtualPlaypositionStart += (direction() ? 1 : -1) * available;
            *pNumConsumedSamples -= available;
            return virtualPlaypositionStart;
        }

        bool merge(const ReadLogEntry& other) {
            // Allow 0-length ReadLogEntry's to merge regardless of their
            // direction if they have the right start point.
            if ((other.length() == 0 || direction() == other.direction()) &&
                    virtualPlaypositionEndNonInclusive == other.virtualPlaypositionStart) {
                virtualPlaypositionEndNonInclusive =
                        other.virtualPlaypositionEndNonInclusive;
                return true;
            }
            return false;
        }
    };

    /// virtualPlaypositionEnd is the first sample in the direction that was
    /// read that was NOT read as part of this log entry.
    void addReadLogEntry(double virtualPlaypositionStart,
            double virtualPlaypositionEndNonInclusive);

    LoopingControl* m_pLoopingControl;
    CueControl* m_pCueControl;
    RateControl* m_pRateControl;
    std::list<ReadLogEntry> m_readAheadLog;
    double m_currentPosition; // In absolute samples
    CachingReader* m_pReader;
    CSAMPLE* m_pCrossFadeBuffer;
    int m_cacheMissCount;
    bool m_cacheMissExpected;
    ReadPlan m_pendingRetry;
    Hint m_pendingReadHint{};
    bool m_hasPendingReadHint{false};
};
