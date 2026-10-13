#include "engine/engineworkerscheduler.h"

#include "engine/engineworker.h"
#ifdef BUILD_TESTING
#include "test/f001rtprobe.h"
#endif
#include "moc_engineworkerscheduler.cpp"
#include "util/compatibility/qmutex.h"
#include "util/event.h"

EngineWorkerScheduler::EngineWorkerScheduler(QObject* pParent)
        : QThread(pParent),
          m_bWakeScheduler(false),
          m_bQuit(false) {
}

EngineWorkerScheduler::~EngineWorkerScheduler() {
    {
        // tell run method to terminate
        const auto lock = lockMutex(&m_mutex);
        m_bQuit = true;
#ifdef BUILD_TESTING
        F001RtProbeScopeGuardForTest f001DestructorWakeScope(
                F001RtProbeScopeForTest::SchedulerDestructorWake);
#endif
        m_waitCondition.wakeAll();
    }
    // wait for thread to terminate
    wait();
}

void EngineWorkerScheduler::workerReady() {
    m_bWakeScheduler.store(true);
}

void EngineWorkerScheduler::addWorker(EngineWorker* pWorker) {
    DEBUG_ASSERT(pWorker);
    const auto lock = lockMutex(&m_mutex);
    m_workers.push_back(pWorker);
}

void EngineWorkerScheduler::runWorkers() {
#ifdef BUILD_TESTING
    F001RtProbeScopeGuardForTest f001RtScope(
            F001RtProbeScopeForTest::SchedulerRunWorkers);
#endif
    // Wake the scheduler if we have written a worker-ready message to the
    // scheduler. This is called from the callback thread, so we use an
    // atomic and not a mutex.
    if (m_bWakeScheduler.exchange(false)) {
#ifdef BUILD_TESTING
        F001RtProbeScopeGuardForTest f001WakeScope(
                F001RtProbeScopeForTest::SchedulerWakeAll);
#endif
        m_waitCondition.wakeAll();
    }
}

void EngineWorkerScheduler::run() {
    static const QString tag("EngineWorkerScheduler");
    bool quit = false;
    while (!quit) {
        Event::start(tag);
        {
            const auto lock = lockMutex(&m_mutex);
            for(const auto& pWorker: m_workers) {
                pWorker->wakeIfReady();
            }
        }
        Event::end(tag);
        {
            const auto lock = lockMutex(&m_mutex);
            if (!m_bQuit) {
                // Wait for next runWorkers() call
                m_waitCondition.wait(&m_mutex); // unlock mutex and wait
            }
            // copy mutex protected var to local
            quit = m_bQuit;
        }
    }
}
