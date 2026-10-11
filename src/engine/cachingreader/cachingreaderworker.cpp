#include "engine/cachingreader/cachingreaderworker.h"

#include <algorithm>
#include <chrono>
#if defined(Q_OS_LINUX)
#include <time.h>
#endif

#include <QAtomicInt>
#include <QtDebug>
#ifdef BUILD_TESTING
#include <condition_variable>
#include <mutex>
#endif
#include <limits>
#include <utility>

#include "analyzer/analyzersilence.h"
#include "engine/controls/seek30control.h"
#include "moc_cachingreaderworker.cpp"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "util/compatibility/qmutex.h"
#include "util/event.h"
#include "util/fifo.h"
#include "util/logger.h"
#include "util/span.h"

namespace {

mixxx::Logger kLogger("CachingReaderWorker");

using DiagnosticClock = std::chrono::steady_clock;
constexpr std::uint32_t kUnavailableTimingUsec =
        std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kMaximumTimingUsec = kUnavailableTimingUsec - 1;
#if defined(Q_OS_LINUX) && defined(__x86_64__)
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
        "Cache timing pairs must be lock-free on x86_64 Linux");
static_assert(std::atomic<std::int64_t>::is_always_lock_free,
        "Cache timing ages must be lock-free on x86_64 Linux");
#endif

std::uint32_t encodeDiagnosticUsec(std::int64_t usec) {
    if (usec < 0) {
        return kUnavailableTimingUsec;
    }
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
            usec, static_cast<std::int64_t>(kMaximumTimingUsec)));
}

std::int64_t decodeDiagnosticUsec(std::uint32_t usec) {
    if (usec == kUnavailableTimingUsec) {
        return -1;
    }
    return static_cast<std::int64_t>(usec);
}

std::uint64_t encodeDiagnosticTimingPair(
        std::int64_t wallUsec, std::int64_t threadCpuUsec) {
    return (static_cast<std::uint64_t>(encodeDiagnosticUsec(wallUsec)) << 32) |
            encodeDiagnosticUsec(threadCpuUsec);
}

std::uint32_t diagnosticTimingPairWall(std::uint64_t timingPair) {
    return static_cast<std::uint32_t>(timingPair >> 32);
}

bool diagnosticTimingPairHasLargerWall(
        std::uint64_t candidatePair, std::uint64_t retainedPair) {
    const std::uint32_t retainedWall =
            diagnosticTimingPairWall(retainedPair);
    return retainedWall == kUnavailableTimingUsec ||
            diagnosticTimingPairWall(candidatePair) > retainedWall;
}

void decodeDiagnosticTimingPair(
        std::uint64_t timingPair,
        std::int64_t* pWallUsec,
        std::int64_t* pThreadCpuUsec) {
    *pWallUsec = decodeDiagnosticUsec(
            diagnosticTimingPairWall(timingPair));
    *pThreadCpuUsec = decodeDiagnosticUsec(
            static_cast<std::uint32_t>(timingPair));
}

std::int64_t diagnosticMonotonicUsec() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
            DiagnosticClock::now().time_since_epoch())
            .count();
}

bool diagnosticWorkerThreadCpuUsec(std::int64_t* pTimeUsec) {
#if defined(Q_OS_LINUX)
    timespec threadCpuTime{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &threadCpuTime) != 0) {
        return false;
    }
    *pTimeUsec = static_cast<std::int64_t>(threadCpuTime.tv_sec) * 1'000'000 +
            threadCpuTime.tv_nsec / 1'000;
    return true;
#else
    (void)pTimeUsec;
    return false;
#endif
}

// we need the last silence frame and the first sound frame
constexpr SINT kNumSoundFrameToVerify = 2;

#ifdef BUILD_TESTING
struct TestTrackLifecycleRegistration {
    std::mutex mutex;
    std::condition_variable callbacksFinished;
    QString group;
    CachingReaderWorker::TestTrackLifecycleHook hook{nullptr};
    void* context{nullptr};
    int callbacksInFlight{0};
};

TestTrackLifecycleRegistration s_testTrackLifecycleRegistration;

void notifyTestTrackLifecycle(
        const QString& group,
        CachingReaderWorker::TestTrackLifecycleEvent event,
        quint64 generation) {
    CachingReaderWorker::TestTrackLifecycleHook hook = nullptr;
    void* context = nullptr;
    {
        const std::lock_guard<std::mutex> lock(
                s_testTrackLifecycleRegistration.mutex);
        if (s_testTrackLifecycleRegistration.group.isEmpty() ||
                s_testTrackLifecycleRegistration.group == group) {
            hook = s_testTrackLifecycleRegistration.hook;
            context = s_testTrackLifecycleRegistration.context;
            if (hook) {
                ++s_testTrackLifecycleRegistration.callbacksInFlight;
            }
        }
    }
    if (!hook) {
        return;
    }

    hook(group, event, generation, context);

    {
        const std::lock_guard<std::mutex> lock(
                s_testTrackLifecycleRegistration.mutex);
        --s_testTrackLifecycleRegistration.callbacksInFlight;
    }
    s_testTrackLifecycleRegistration.callbacksFinished.notify_all();
}
#endif

} // anonymous namespace

CachingReaderWorker::CachingReaderWorker(
        const QString& group,
        FIFO<CachingReaderChunkReadRequest>* pChunkReadRequestFIFO,
        FIFO<ReaderStatusUpdate>* pReaderStatusFIFO,
        mixxx::audio::ChannelCount maxSupportedChannel,
        bool enableTimingDiagnostics)
        : m_group(group),
          m_tag(QString("CachingReaderWorker %1").arg(m_group)),
          m_pChunkReadRequestFIFO(pChunkReadRequestFIFO),
          m_pReaderStatusFIFO(pReaderStatusFIFO),
          m_maxSupportedChannel(maxSupportedChannel),
          m_timingDiagnosticsEnabled(enableTimingDiagnostics),
          m_diagnosticState(static_cast<int>(DiagnosticState::Waiting)),
          m_diagnosticActiveChunk(-1),
          m_diagnosticLastCompletedChunk(-1),
          m_diagnosticCompletedRequests(0),
          m_diagnosticDequeuedRequests(0),
          m_diagnosticPublishedStatuses(0) {
}

#ifdef BUILD_TESTING
void CachingReaderWorker::setTestTrackLifecycleHook(
        const QString& group,
        TestTrackLifecycleHook hook,
        void* context) {
    VERIFY_OR_DEBUG_ASSERT(hook && context) {
        return;
    }
    const std::lock_guard<std::mutex> lock(
            s_testTrackLifecycleRegistration.mutex);
    VERIFY_OR_DEBUG_ASSERT(!s_testTrackLifecycleRegistration.hook) {
        return;
    }
    s_testTrackLifecycleRegistration.group = group;
    s_testTrackLifecycleRegistration.hook = hook;
    s_testTrackLifecycleRegistration.context = context;
}

void CachingReaderWorker::clearTestTrackLifecycleHook(void* context) {
    std::unique_lock<std::mutex> lock(
            s_testTrackLifecycleRegistration.mutex);
    VERIFY_OR_DEBUG_ASSERT(
            s_testTrackLifecycleRegistration.context == context) {
        return;
    }
    s_testTrackLifecycleRegistration.hook = nullptr;
    s_testTrackLifecycleRegistration.context = nullptr;
    s_testTrackLifecycleRegistration.group.clear();
    s_testTrackLifecycleRegistration.callbacksFinished.wait(
            lock,
            [] { return s_testTrackLifecycleRegistration.callbacksInFlight == 0; });
}
#endif

int CachingReaderWorker::diagnosticStatusCapacity() const {
    return m_pReaderStatusFIFO->capacity();
}

bool CachingReaderWorker::diagnosticTimingSnapshot(
        DiagnosticTimingSnapshot* pSnapshot) const {
    if (!pSnapshot) {
        return false;
    }
    decodeDiagnosticTimingPair(
            m_diagnosticLatestRequestTimingPair.load(std::memory_order_relaxed),
            &pSnapshot->lastRequestWallUsec,
            &pSnapshot->lastRequestThreadCpuUsec);
    decodeDiagnosticTimingPair(
            m_diagnosticSlowestRequestTimingPair.load(std::memory_order_relaxed),
            &pSnapshot->maxRequestWallUsec,
            &pSnapshot->maxRequestThreadCpuUsec);
    decodeDiagnosticTimingPair(
            m_diagnosticLatestSampleBufferTimingPair.load(
                    std::memory_order_relaxed),
            &pSnapshot->lastSourceReadWallUsec,
            &pSnapshot->lastSourceReadThreadCpuUsec);
    decodeDiagnosticTimingPair(
            m_diagnosticSlowestSampleBufferTimingPair.load(
                    std::memory_order_relaxed),
            &pSnapshot->maxSourceReadWallUsec,
            &pSnapshot->maxSourceReadThreadCpuUsec);
    return true;
}

std::int64_t CachingReaderWorker::diagnosticActiveRequestAgeUsec() const {
    const std::int64_t startTokenBefore =
            m_diagnosticActiveRequestStartUsec.load(std::memory_order_acquire);
    const int activeChunkBefore = diagnosticActiveChunk();
    if (startTokenBefore == 0 || activeChunkBefore < 0) {
        return -1;
    }
    const std::int64_t ageUsec =
            diagnosticMonotonicUsec() - startTokenBefore + 1;
    const std::int64_t startTokenAfter =
            m_diagnosticActiveRequestStartUsec.load(std::memory_order_acquire);
    const int activeChunkAfter = diagnosticActiveChunk();
    if (startTokenBefore != startTokenAfter ||
            activeChunkBefore != activeChunkAfter || activeChunkAfter < 0) {
        return -1;
    }
    return std::max<std::int64_t>(0, ageUsec);
}

std::int64_t CachingReaderWorker::diagnosticActiveSourceReadAgeUsec() const {
    const std::int64_t startTokenBefore =
            m_diagnosticActiveSourceReadStartUsec.load(std::memory_order_acquire);
    const int activeChunkBefore = diagnosticActiveChunk();
    if (startTokenBefore == 0 || activeChunkBefore < 0) {
        return -1;
    }
    const std::int64_t ageUsec =
            diagnosticMonotonicUsec() - startTokenBefore + 1;
    const std::int64_t startTokenAfter =
            m_diagnosticActiveSourceReadStartUsec.load(std::memory_order_acquire);
    const int activeChunkAfter = diagnosticActiveChunk();
    if (startTokenBefore != startTokenAfter ||
            activeChunkBefore != activeChunkAfter || activeChunkAfter < 0) {
        return -1;
    }
    return std::max<std::int64_t>(0, ageUsec);
}

void CachingReaderWorker::recordDiagnosticTiming(
        const DiagnosticTimingSample& sample) {
    const std::uint64_t requestTimingPair = encodeDiagnosticTimingPair(
            sample.requestWallUsec, sample.requestThreadCpuUsec);
    m_diagnosticLatestRequestTimingPair.store(
            requestTimingPair, std::memory_order_relaxed);
    const std::uint64_t slowestRequestTimingPair =
            m_diagnosticSlowestRequestTimingPair.load(
                    std::memory_order_relaxed);
    if (diagnosticTimingPairHasLargerWall(
                requestTimingPair, slowestRequestTimingPair)) {
        m_diagnosticSlowestRequestTimingPair.store(
                requestTimingPair, std::memory_order_relaxed);
    }

    const std::uint64_t sampleBufferTimingPair = encodeDiagnosticTimingPair(
            sample.sourceReadWallUsec, sample.sourceReadThreadCpuUsec);
    m_diagnosticLatestSampleBufferTimingPair.store(
            sampleBufferTimingPair, std::memory_order_relaxed);
    if (sample.sourceReadWallUsec >= 0) {
        const std::uint64_t slowestSampleBufferTimingPair =
                m_diagnosticSlowestSampleBufferTimingPair.load(
                        std::memory_order_relaxed);
        if (diagnosticTimingPairHasLargerWall(
                    sampleBufferTimingPair,
                    slowestSampleBufferTimingPair)) {
            m_diagnosticSlowestSampleBufferTimingPair.store(
                    sampleBufferTimingPair, std::memory_order_relaxed);
        }
    }
}

bool CachingReaderWorker::publishStatus(ReaderStatusUpdate update) {
    const int previousState = m_diagnosticState.loadAcquire();
    m_diagnosticState.storeRelease(
            static_cast<int>(DiagnosticState::PublishingStatus));
    while (!m_stop.loadAcquire()) {
        if (m_pReaderStatusFIFO->write(&update, 1) == 1) {
            m_diagnosticPublishedStatuses.fetchAndAddRelaxed(1);
            m_diagnosticState.storeRelease(previousState);
            return true;
        }

        // The callback wakes the worker after consuming status updates. This
        // avoids burning a worker core while backpressure is active.
        m_semaRun.acquire();
    }

    // The owner cannot process more updates after it has begun destruction.
    // Return chunk ownership explicitly before abandoning this status update.
    update.takeFromWorker();
    m_diagnosticState.storeRelease(previousState);
    return false;
}

ReaderStatusUpdate CachingReaderWorker::processReadRequest(
        const CachingReaderChunkReadRequest& request,
        DiagnosticTimingSample* pTimingSample) {
    CachingReaderChunk* pChunk = request.chunk;
    DEBUG_ASSERT(pChunk);

    // Before trying to read any data we need to check if the audio source
    // is available and if any audio data that is needed by the chunk is
    // actually available.
    auto chunkFrameIndexRange = pChunk->frameIndexRange(m_pAudioSource);
    DEBUG_ASSERT(!m_pAudioSource ||
            chunkFrameIndexRange.isSubrangeOf(m_pAudioSource->frameIndexRange()));
    if (chunkFrameIndexRange.empty()) {
        ReaderStatusUpdate result;
        result.init(CHUNK_READ_INVALID, pChunk, m_pAudioSource ? m_pAudioSource->frameIndexRange() : mixxx::IndexRange());
        return result;
    }

    // Try to read the data required for the chunk from the audio source
    std::int64_t sourceReadStartUsec = 0;
    std::int64_t sourceReadThreadCpuStartUsec = 0;
    bool sourceReadThreadCpuStartAvailable = false;
    if (pTimingSample) {
        sourceReadStartUsec = diagnosticMonotonicUsec();
        m_diagnosticActiveSourceReadStartUsec.store(
                sourceReadStartUsec + 1, std::memory_order_release);
        sourceReadThreadCpuStartAvailable =
                diagnosticWorkerThreadCpuUsec(&sourceReadThreadCpuStartUsec);
    }
    const mixxx::IndexRange bufferedFrameIndexRange = pChunk->bufferSampleFrames(
            m_pAudioSource,
            mixxx::SampleBuffer::WritableSlice(m_tempReadBuffer));
    if (pTimingSample) {
        pTimingSample->sourceReadWallUsec =
                diagnosticMonotonicUsec() - sourceReadStartUsec;
        m_diagnosticActiveSourceReadStartUsec.store(
                0, std::memory_order_release);
        std::int64_t sourceReadThreadCpuEndUsec = 0;
        if (sourceReadThreadCpuStartAvailable &&
                diagnosticWorkerThreadCpuUsec(&sourceReadThreadCpuEndUsec)) {
            pTimingSample->sourceReadThreadCpuUsec =
                    sourceReadThreadCpuEndUsec - sourceReadThreadCpuStartUsec;
        }
    }
    DEBUG_ASSERT(!m_pAudioSource ||
            bufferedFrameIndexRange.isSubrangeOf(m_pAudioSource->frameIndexRange()));
    // The readable frame range might have changed
    chunkFrameIndexRange = intersect(chunkFrameIndexRange, m_pAudioSource->frameIndexRange());
    DEBUG_ASSERT(bufferedFrameIndexRange.empty() ||
            bufferedFrameIndexRange.isSubrangeOf(chunkFrameIndexRange));

    ReaderStatus status = bufferedFrameIndexRange.empty() ? CHUNK_READ_EOF : CHUNK_READ_SUCCESS;
    if (bufferedFrameIndexRange != chunkFrameIndexRange) {
        kLogger.warning()
                << m_group
                << "Failed to read chunk samples for frame index range:"
                << "expected =" << chunkFrameIndexRange
                << ", actual =" << bufferedFrameIndexRange;
        if (bufferedFrameIndexRange.empty()) {
            status = CHUNK_READ_INVALID; // overwrite EOF (see above)
        }
    }

    if (status == CHUNK_READ_SUCCESS) {
        // This call here assumes that the caching reader will read the first sound cue at
        // one of the first chunks. The check serves as a sanity check to ensure that the
        // sample data has not changed since it has ben analyzed. This could happen because
        // of a change in actual audio data or because the file was decoded using a different
        // decoder
        // This is part of a first prove of concept and needs to be replaces with a different
        // solution which is still under discussion. This might be also extended
        // to further checks whether a automatic offset adjustment is possible or a the
        // sample position metadata shall be treated as outdated.
        // Failures of the sanity check only result in an entry into the log at the moment.
        verifyFirstSound(pChunk, m_pAudioSource->getSignalInfo().getChannelCount());
    }

    ReaderStatusUpdate result;
    result.init(status, pChunk, m_pAudioSource ? m_pAudioSource->frameIndexRange() : mixxx::IndexRange());
    return result;
}

// WARNING: Always called from a different thread (GUI)
#ifdef __STEM__
void CachingReaderWorker::newTrack(
        TrackPointer pTrack,
        mixxx::StemChannelSelection stemMask,
        quint64 generation) {
#else
void CachingReaderWorker::newTrack(TrackPointer pTrack, quint64 generation) {
#endif
    {
        const auto locker = lockMutex(&m_newTrackMutex);
#ifdef __STEM__
        m_pNewTrack = NewTrackRequest{pTrack, generation, stemMask};
#else
        m_pNewTrack = NewTrackRequest{pTrack, generation};
#endif
        m_newTrackAvailable.storeRelease(1);
    }
    workReady();
}

void CachingReaderWorker::setSeek30Control(Seek30Control* pControl) {
    m_pSeek30Control.store(pControl, std::memory_order_release);
}

void CachingReaderWorker::updateSeek30Track(TrackPointer pNewTrack) {
    m_seek30State.trackLoaded(std::move(pNewTrack));
    m_seek30Generation.store(
            m_seek30State.generation(), std::memory_order_release);
}

bool CachingReaderWorker::enqueueSeek30Command(Seek30Operation operation) {
    const Seek30Command command{
            operation,
            m_seek30Generation.load(std::memory_order_acquire)};
    if (!m_seek30CommandMailbox.tryPush(command)) {
        recordSeek30CommandOverflow();
        return false;
    }
    workReady();
    return true;
}

void CachingReaderWorker::recordSeek30CommandOverflow() {
    auto count = m_seek30CommandOverflowCount.load(std::memory_order_relaxed);
    while (count != std::numeric_limits<std::uint64_t>::max() &&
            !m_seek30CommandOverflowCount.compare_exchange_weak(
                    count,
                    count + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
    }
}

void CachingReaderWorker::processSeek30Commands() {
    Seek30Control* const pControl =
            m_pSeek30Control.load(std::memory_order_acquire);
    if (!pControl) {
        return;
    }

    Seek30Command command;
    if (!m_seek30CommandMailbox.tryPop(&command)) {
        return;
    }

    auto inputs = pControl->seek30WorkerInputs();
    do {
        if (command.generation != m_seek30State.generation()) {
            continue;
        }

        const auto target = m_seek30State.process(command, inputs);
        if (target.position.isValid()) {
            inputs.currentPosition = target.position;
            auto publishedTarget = target;
            publishedTarget.sequence = ++m_seek30TargetSequence;
            pControl->publishSeekTarget(publishedTarget);
        }
    } while (m_seek30CommandMailbox.tryPop(&command));
}

void CachingReaderWorker::run() {
    // the id of this thread, for debugging purposes
    static auto lastId = QAtomicInt(0);
    const auto id = lastId.fetchAndAddRelaxed(1) + 1;
    QThread::currentThread()->setObjectName(
            QStringLiteral("CachingReaderWorker ") + QString::number(id));

    Event::start(m_tag);
    while (!m_stop.loadAcquire()) {
        // Request is initialized by reading from FIFO
        CachingReaderChunkReadRequest request;
        if (m_newTrackAvailable.loadAcquire()) {
            m_diagnosticState.storeRelease(
                    static_cast<int>(DiagnosticState::LoadingTrack));
            m_diagnosticActiveChunk.storeRelease(-1);
            NewTrackRequest pLoadTrack;
            { // locking scope
                const auto locker = lockMutex(&m_newTrackMutex);
                pLoadTrack = m_pNewTrack;
                m_newTrackAvailable.storeRelease(0);
            } // implicitly unlocks the mutex
#ifdef BUILD_TESTING
            notifyTestTrackLifecycle(
                    m_group,
                    TestTrackLifecycleEvent::RequestDequeued,
                    pLoadTrack.generation);
#endif
            if (pLoadTrack.track) {
                // in this case the engine is still running with the old track
#ifdef __STEM__
                if (!loadTrack(
                            pLoadTrack.track,
                            pLoadTrack.stemMask,
                            pLoadTrack.generation)) {
#else
                if (!loadTrack(pLoadTrack.track, pLoadTrack.generation)) {
#endif
                    break;
                }
            } else {
                // here, the engine is already stopped
                if (!unloadTrack()) {
                    break;
                }
#ifdef BUILD_TESTING
                notifyTestTrackLifecycle(
                        m_group,
                        TestTrackLifecycleEvent::NullTrackUnloaded,
                        pLoadTrack.generation);
#endif
            }
            processSeek30Commands();
        } else {
            processSeek30Commands();
            if (m_pChunkReadRequestFIFO->read(&request, 1) == 1) {
                m_diagnosticDequeuedRequests.fetchAndAddRelaxed(1);
                // Read the requested chunk and send the result. Publish only
                // primitive progress snapshots for off-thread diagnostics.
                const int chunkIndex = request.chunk->getIndex();
                m_diagnosticActiveChunk.storeRelease(chunkIndex);
                m_diagnosticState.storeRelease(
                        static_cast<int>(DiagnosticState::Decoding));
                DiagnosticTimingSample timingSample;
                std::int64_t requestStartThreadCpuUsec = 0;
                const std::int64_t requestStartUsec =
                        m_timingDiagnosticsEnabled ? diagnosticMonotonicUsec() : 0;
                if (m_timingDiagnosticsEnabled) {
                    m_diagnosticActiveRequestStartUsec.store(
                            requestStartUsec + 1, std::memory_order_release);
                }
                const bool requestStartThreadCpuAvailable =
                        m_timingDiagnosticsEnabled &&
                        diagnosticWorkerThreadCpuUsec(
                                &requestStartThreadCpuUsec);
                const ReaderStatusUpdate update = processReadRequest(
                        request,
                        m_timingDiagnosticsEnabled ? &timingSample : nullptr);
                if (m_timingDiagnosticsEnabled) {
                    timingSample.requestWallUsec =
                            diagnosticMonotonicUsec() - requestStartUsec;
                    m_diagnosticActiveRequestStartUsec.store(
                            0, std::memory_order_release);
                    std::int64_t requestEndThreadCpuUsec = 0;
                    if (requestStartThreadCpuAvailable &&
                            diagnosticWorkerThreadCpuUsec(
                                    &requestEndThreadCpuUsec)) {
                        timingSample.requestThreadCpuUsec =
                                requestEndThreadCpuUsec -
                                requestStartThreadCpuUsec;
                    }
                    recordDiagnosticTiming(timingSample);
                }
                m_diagnosticLastCompletedChunk.storeRelease(chunkIndex);
                m_diagnosticCompletedRequests.fetchAndAddRelaxed(1);
                m_diagnosticActiveChunk.storeRelease(-1);
                if (!publishStatus(update)) {
                    break;
                }
            } else {
                m_diagnosticActiveChunk.storeRelease(-1);
                m_diagnosticState.storeRelease(
                        static_cast<int>(DiagnosticState::Waiting));
                Event::end(m_tag);
                m_semaRun.acquire();
                Event::start(m_tag);
            }
        }
    }
}

bool CachingReaderWorker::discardAllPendingRequests() {
    bool allPublished = true;
    CachingReaderChunkReadRequest request;
    while (m_pChunkReadRequestFIFO->read(&request, 1) == 1) {
        m_diagnosticDequeuedRequests.fetchAndAddRelaxed(1);
        const auto update = ReaderStatusUpdate::readDiscarded(request.chunk);
        allPublished = publishStatus(update) && allPublished;
    }
    return allPublished;
}

bool CachingReaderWorker::closeAudioSource() {
    const bool allPublished = discardAllPendingRequests();

    if (m_pAudioSource) {
        // Closes open file handles of the old track.
        m_pAudioSource->close();
        m_pAudioSource.reset();
    }

    // This function has to be called with the engine stopped only
    // to avoid collecting new requests for the old track
    DEBUG_ASSERT(!m_pChunkReadRequestFIFO->readAvailable());
    return allPublished;
}

bool CachingReaderWorker::unloadTrack() {
    if (!closeAudioSource()) {
        return false;
    }
    updateSeek30Track(TrackPointer());

    const auto update = ReaderStatusUpdate::trackUnloaded();
    return publishStatus(update);
}

#ifdef __STEM__
bool CachingReaderWorker::loadTrack(
        const TrackPointer& pTrack,
        mixxx::StemChannelSelection stemMask,
        quint64 generation) {
#else
bool CachingReaderWorker::loadTrack(const TrackPointer& pTrack, quint64 generation) {
#endif
    if (m_stop.loadAcquire()) {
        return false;
    }
    // This emit is directly connected and returns synchronized
    // after the engine has been stopped.
    emit trackLoading(generation);
#ifdef BUILD_TESTING
    notifyTestTrackLifecycle(
            m_group,
            TestTrackLifecycleEvent::TrackLoadingAccepted,
            generation);
#endif
    if (m_stop.loadAcquire()) {
        return false;
    }

    // Keep TrackPointer and cue ownership on this worker even when the
    // EngineBuffer's unload notification originates from another thread.
    updateSeek30Track(TrackPointer());

    if (!closeAudioSource()) {
        return false;
    }

    if (!pTrack->getFileInfo().checkFileExists()) {
        kLogger.warning()
                << m_group
                << "File not found"
                << pTrack->getFileInfo();
        const auto update = ReaderStatusUpdate::trackUnloaded();
        if (!publishStatus(update) || m_stop.loadAcquire()) {
            return false;
        }
        emit trackLoadFailed(pTrack,
                tr("The file '%1' could not be found.")
                        .arg(QDir::toNativeSeparators(pTrack->getLocation())),
                generation);
        return true;
    }

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(m_maxSupportedChannel);
#ifdef __STEM__
    config.setStemMask(stemMask);
#endif
    m_pAudioSource = SoundSourceProxy(pTrack).openAudioSource(config);
    if (!m_pAudioSource) {
        kLogger.warning()
                << m_group
                << "Failed to open file"
                << pTrack->getFileInfo();
        const auto update = ReaderStatusUpdate::trackUnloaded();
        if (!publishStatus(update) || m_stop.loadAcquire()) {
            return false;
        }
        emit trackLoadFailed(pTrack,
                tr("The file '%1' could not be loaded.")
                        .arg(QDir::toNativeSeparators(pTrack->getLocation())),
                generation);
        return true;
    }

    // It is critical that the audio source doesn't contain more channels than
    // requested as this could lead to overflow when reading chunks
    VERIFY_OR_DEBUG_ASSERT(m_pAudioSource->getSignalInfo().getChannelCount() >=
                    mixxx::audio::ChannelCount::mono() &&
            m_pAudioSource->getSignalInfo().getChannelCount() <=
                    m_maxSupportedChannel) {
        // Cache the channel count before we reset the AudioSourcePointer
        const int chCount = static_cast<int>(m_pAudioSource->getSignalInfo().getChannelCount());
        m_pAudioSource.reset(); // Close open file handles
        const auto update = ReaderStatusUpdate::trackUnloaded();
        if (!publishStatus(update) || m_stop.loadAcquire()) {
            return false;
        }
        emit trackLoadFailed(pTrack,
                tr("The file '%1' could not be loaded because it contains %2 "
                   "channels, and only 1 to %3 are supported.")
                        .arg(QDir::toNativeSeparators(pTrack->getLocation()),
                                QString::number(chCount),
                                QString::number(m_maxSupportedChannel)),
                generation);
        return true;
    }

    // Initially assume that the complete content offered by audio source
    // is available for reading. Later if read errors occur this value will
    // be decreased to avoid repeated reading of corrupt audio data.
    if (m_pAudioSource->frameIndexRange().empty()) {
        m_pAudioSource.reset(); // Close open file handles
        kLogger.warning()
                << m_group
                << "Failed to open empty file"
                << pTrack->getFileInfo();
        const auto update = ReaderStatusUpdate::trackUnloaded();
        if (!publishStatus(update) || m_stop.loadAcquire()) {
            return false;
        }
        emit trackLoadFailed(pTrack,
                tr("The file '%1' is empty and could not be loaded.")
                        .arg(QDir::toNativeSeparators(pTrack->getLocation())),
                generation);
        return true;
    }

    updateSeek30Track(pTrack);

    // Adjust the internal buffer
    const SINT tempReadBufferSize =
            m_pAudioSource->getSignalInfo().frames2samples(
                    CachingReaderChunk::kFrames);
    if (m_tempReadBuffer.size() != tempReadBufferSize) {
        mixxx::SampleBuffer(tempReadBufferSize).swap(m_tempReadBuffer);
    }

    const auto update =
            ReaderStatusUpdate::trackLoaded(
                    m_pAudioSource->frameIndexRange());
    if (!publishStatus(update) || m_stop.loadAcquire()) {
        return false;
    }

    // Emit that the track is loaded.

    // This code is a workaround until we have found a better solution to
    // verify and correct offsets.
    CuePointer pN60dBSound =
            pTrack->findCueByType(mixxx::CueType::N60dBSound);
    if (pN60dBSound) {
        m_firstSoundFrameToVerify = pN60dBSound->getPosition();
    }

    // The engine must not request any chunks before receiving the
    // trackLoaded() signal
    DEBUG_ASSERT(!m_pChunkReadRequestFIFO->readAvailable());

    emit trackLoaded(
            pTrack,
            m_pAudioSource->getSignalInfo().getSampleRate(),
            m_pAudioSource->getSignalInfo().getChannelCount(),
            mixxx::audio::FramePos(m_pAudioSource->frameLength()),
            generation);
    return true;
}

void CachingReaderWorker::quitWait() {
    m_stop = 1;
    m_semaRun.release();
    wait();
}

void CachingReaderWorker::verifyFirstSound(const CachingReaderChunk* pChunk,
        mixxx::audio::ChannelCount channelCount) {
    if (!m_firstSoundFrameToVerify.isValid()) {
        return;
    }

    const int firstSoundIndex =
            CachingReaderChunk::indexForFrame(static_cast<SINT>(
                    m_firstSoundFrameToVerify.toLowerFrameBoundary()
                            .value()));
    if (pChunk->getIndex() == firstSoundIndex) {
        auto sampleBuffer = mixxx::SampleBuffer(
                kNumSoundFrameToVerify * channelCount);
        // We read two frames, the last silence frame and the first non-silence frame from
        // m_firstSoundFrameToVerify. end points to one position after them.
        sampleBuffer.clear(); // we need to clear the buffer, for the case the
                              // very first sample is the first sound.
        SINT end = static_cast<SINT>(m_firstSoundFrameToVerify.toLowerFrameBoundary().value()) + 1;
        mixxx::IndexRange probeFrameIndexRange =
                mixxx::IndexRange::between(end - kNumSoundFrameToVerify, end);
        mixxx::IndexRange bufferedFrameIndexRange =
                pChunk->readBufferedSampleFrames(
                        sampleBuffer.data(), channelCount, probeFrameIndexRange);
        VERIFY_OR_DEBUG_ASSERT(bufferedFrameIndexRange.end() == probeFrameIndexRange.end()) {
            qWarning() << "skipping verifyFirstSound()";
            return;
        }
        if (AnalyzerSilence::verifyFirstSound(sampleBuffer.span(),
                    mixxx::audio::FramePos(1),
                    channelCount)) {
            qDebug() << "First sound found at the previously stored position";
        } else {
            // This can happen in case of track edits or replacements, changed
            // encoders or encoding issues.
            qWarning() << "First sound has been moved! The beatgrid and "
                          "other annotations are no longer valid"
                       << m_pAudioSource->getUrlString();
        }
        m_firstSoundFrameToVerify = mixxx::audio::FramePos();
    }
}
