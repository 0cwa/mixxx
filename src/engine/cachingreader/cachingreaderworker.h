#pragma once

#ifdef BUILD_TESTING
#include <gtest/gtest_prod.h>
#endif

#include <QMutex>
#include <QString>
#include <atomic>
#include <cstdint>

#include "audio/frame.h"
#include "audio/types.h"
#include "engine/cachingreader/cachingreaderchunk.h"
#include "engine/controls/seek30workerstate.h"
#include "engine/engineworker.h"
#include "sources/audiosource.h"
#include "track/track_decl.h"

template<class DataType>
class FIFO;

class Seek30Control;

// POD with trivial ctor/dtor/copy for passing through FIFO
typedef struct CachingReaderChunkReadRequest {
    CachingReaderChunk* chunk;

    void giveToWorker(CachingReaderChunkForOwner* chunkForOwner) {
        DEBUG_ASSERT(chunkForOwner);
        chunk = chunkForOwner;
        chunkForOwner->giveToWorker();
    }
} CachingReaderChunkReadRequest;

enum ReaderStatus {
    TRACK_LOADED,
    TRACK_UNLOADED,
    CHUNK_READ_SUCCESS,
    CHUNK_READ_EOF,
    CHUNK_READ_INVALID,
    CHUNK_READ_DISCARDED, // response without frame index range!
};

// POD with trivial ctor/dtor/copy for passing through FIFO
typedef struct ReaderStatusUpdate {
  private:
    CachingReaderChunk* chunk;
    SINT readableFrameIndexRangeStart;
    SINT readableFrameIndexRangeEnd;

  public:
    ReaderStatus status;

    void init(
            ReaderStatus statusArg,
            CachingReaderChunk* chunkArg,
            const mixxx::IndexRange& readableFrameIndexRangeArg) {
        status = statusArg;
        chunk = chunkArg;
        readableFrameIndexRangeStart = readableFrameIndexRangeArg.start();
        readableFrameIndexRangeEnd = readableFrameIndexRangeArg.end();
    }

    static ReaderStatusUpdate readDiscarded(
            CachingReaderChunk* chunk) {
        ReaderStatusUpdate update;
        update.init(CHUNK_READ_DISCARDED, chunk, mixxx::IndexRange());
        return update;
    }

    static ReaderStatusUpdate trackLoaded(
            const mixxx::IndexRange& readableFrameIndexRange) {
        DEBUG_ASSERT(!readableFrameIndexRange.empty());
        ReaderStatusUpdate update;
        update.init(TRACK_LOADED, nullptr, readableFrameIndexRange);
        return update;
    }

    static ReaderStatusUpdate trackUnloaded() {
        ReaderStatusUpdate update;
        update.init(TRACK_UNLOADED, nullptr, mixxx::IndexRange());
        return update;
    }

    CachingReaderChunkForOwner* takeFromWorker() {
        CachingReaderChunkForOwner* pChunk = nullptr;
        if (chunk) {
            DEBUG_ASSERT(dynamic_cast<CachingReaderChunkForOwner*>(chunk));
            pChunk = static_cast<CachingReaderChunkForOwner*>(chunk);
            chunk = nullptr;
            pChunk->takeFromWorker();
        }
        return pChunk;
    }

    mixxx::IndexRange readableFrameIndexRange() const {
        return mixxx::IndexRange::between(
                readableFrameIndexRangeStart,
                readableFrameIndexRangeEnd);
    }
} ReaderStatusUpdate;

class CachingReaderWorker : public EngineWorker {
    Q_OBJECT

  public:
    // Construct a CachingReader with the given group.
    CachingReaderWorker(const QString& group,
            FIFO<CachingReaderChunkReadRequest>* pChunkReadRequestFIFO,
            FIFO<ReaderStatusUpdate>* pReaderStatusFIFO,
            mixxx::audio::ChannelCount maxSupportedChannel);
    ~CachingReaderWorker() override = default;

    // Request to load a new track. wake() must be called afterwards.
#ifdef __STEM__
    void newTrack(TrackPointer pTrack, mixxx::StemChannelSelection stemMask);
#else
    void newTrack(TrackPointer pTrack);
#endif

    // Run upkeep operations like loading tracks and reading from file. Run by a
    // thread pool via the EngineWorkerScheduler.
    void run() override;

    void quitWait();

    // Configure the worker-side Seek30 owner. The pointer is published before
    // any track-loaded callback or command can use it.
    void setSeek30Control(Seek30Control* pControl);

    // Called by DirectConnection callbacks. The newest command is dropped when
    // the fixed mailbox is full; the overflow count is saturating.
    bool enqueueSeek30Command(Seek30Operation operation);

    std::uint64_t seek30Generation() const {
        return m_seek30Generation.load(std::memory_order_acquire);
    }

    std::uint64_t seek30CommandOverflowCount() const {
        return m_seek30CommandOverflowCount.load(std::memory_order_relaxed);
    }

    enum class DiagnosticState {
        Waiting,
        LoadingTrack,
        Decoding,
        PublishingStatus,
    };

    DiagnosticState diagnosticState() const {
        return static_cast<DiagnosticState>(m_diagnosticState.loadAcquire());
    }
    int diagnosticActiveChunk() const {
        return m_diagnosticActiveChunk.loadAcquire();
    }
    int diagnosticLastCompletedChunk() const {
        return m_diagnosticLastCompletedChunk.loadAcquire();
    }
    int diagnosticCompletedRequests() const {
        return m_diagnosticCompletedRequests.loadAcquire();
    }
    int diagnosticDequeuedRequests() const {
        return m_diagnosticDequeuedRequests.loadAcquire();
    }
    int diagnosticPublishedStatuses() const {
        return m_diagnosticPublishedStatuses.loadAcquire();
    }
    int diagnosticStatusCapacity() const;

  signals:
    // Emitted once a new track is loaded and ready to be read from.
    void trackLoading();
    void trackLoaded(TrackPointer pTrack,
            mixxx::audio::SampleRate sampleRate,
            mixxx::audio::ChannelCount channelCount,
            mixxx::audio::FramePos numFrame);
    void trackLoadFailed(TrackPointer pTrack, const QString& reason);

  private:
    friend class CachingReaderWorkerTest;
#ifdef BUILD_TESTING
    FRIEND_TEST(CachingReaderWorkerTest,
            ShutdownPublicationFailureSuppressesLoadFailureSignal);
#endif

#ifdef __STEM__
    struct NewTrackRequest {
        TrackPointer track;
        mixxx::StemChannelSelection stemMask;
    };
#endif
    const QString m_group;
    QString m_tag;

    // Thread-safe FIFOs for communication between the engine callback and
    // reader thread.
    FIFO<CachingReaderChunkReadRequest>* m_pChunkReadRequestFIFO;
    FIFO<ReaderStatusUpdate>* m_pReaderStatusFIFO;

    // Queue of Tracks to load, and the corresponding lock. Must acquire the
    // lock to touch.
    QMutex m_newTrackMutex;
    QAtomicInt m_newTrackAvailable;
#ifdef __STEM__
    NewTrackRequest m_pNewTrack;
#else
    TrackPointer m_pNewTrack;
#endif

    bool discardAllPendingRequests();
    // Publish without spinning when the callback-side FIFO is full. Returns
    // false only during shutdown; an unpublished chunk is returned to the
    // owner in that case.
    bool publishStatus(ReaderStatusUpdate update);

    /// call to be prepare for new tracks
    /// Make sure engine has been stopped before
    bool closeAudioSource();

    /// Internal method to unload a track.
    /// does not emit signals
    bool unloadTrack();

    /// Internal method to load a track. Emits trackLoaded when finished.
#ifdef __STEM__
    bool loadTrack(const TrackPointer& pTrack, mixxx::StemChannelSelection stemMask);
#else
    bool loadTrack(const TrackPointer& pTrack);
#endif

    ReaderStatusUpdate processReadRequest(
            const CachingReaderChunkReadRequest& request);

    void verifyFirstSound(const CachingReaderChunk* pChunk,
            mixxx::audio::ChannelCount channelCount);

    // Track and cue ownership is updated only from run()'s worker thread.
    void updateSeek30Track(TrackPointer pNewTrack);
    void processSeek30Commands();
    void recordSeek30CommandOverflow();

    // The current audio source of the track loaded
    mixxx::AudioSourcePointer m_pAudioSource;

    mixxx::audio::FramePos m_firstSoundFrameToVerify;

    // Temporary buffer for reading samples from all channels
    // before conversion to a stereo signal.
    mixxx::SampleBuffer m_tempReadBuffer;

    // The maximum number of channel that this reader can support
    mixxx::audio::ChannelCount m_maxSupportedChannel;

    QAtomicInt m_stop;

    std::atomic<Seek30Control*> m_pSeek30Control{nullptr};
    Seek30CommandMailbox m_seek30CommandMailbox;
    Seek30WorkerState m_seek30State;
    std::atomic<std::uint64_t> m_seek30Generation{0};
    std::atomic<std::uint64_t> m_seek30CommandOverflowCount{0};
    std::uint64_t m_seek30TargetSequence{0};

    // Lock-free snapshots written by the reader thread and sampled by the
    // CachingReader's diagnostics timer. They never affect worker behavior.
    QAtomicInt m_diagnosticState;
    QAtomicInt m_diagnosticActiveChunk;
    QAtomicInt m_diagnosticLastCompletedChunk;
    QAtomicInt m_diagnosticCompletedRequests;
    QAtomicInt m_diagnosticDequeuedRequests;
    QAtomicInt m_diagnosticPublishedStatuses;
};
