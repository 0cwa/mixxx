#include <gtest/gtest.h>

#include <QSemaphore>
#include <atomic>

#include "engine/engineworker.h"
#include "engine/engineworkerscheduler.h"

namespace {

class SemaphoreProbeWorker final : public EngineWorker {
  public:
    bool waitForWake() {
        return m_semaRun.tryAcquire(1, 5000);
    }
};

class ScanBarrier final {
  public:
    bool waitForInitialScan() {
        return m_scanComplete.tryAcquire(1, 5000);
    }

    void releaseInitialScan() {
        m_resumeScan.release();
    }

    void afterScan() {
        if (m_firstScan.exchange(false)) {
            m_scanComplete.release();
            m_resumeScan.acquire();
        }
    }

  private:
    std::atomic<bool> m_firstScan{true};
    QSemaphore m_scanComplete;
    QSemaphore m_resumeScan;
};

// This standalone target runs one scheduler at a time. Publish the test-only
// tracing barrier before start, and clear it only after joining the scheduler.
std::atomic<ScanBarrier*> scanBarrier{nullptr};

class ScanBarrierShutdown final {
  public:
    ScanBarrierShutdown(EngineWorkerScheduler& scheduler, ScanBarrier& barrier)
            : m_scheduler(scheduler),
              m_barrier(barrier) {
        scanBarrier.store(&barrier);
    }

    ~ScanBarrierShutdown() {
        m_barrier.releaseInitialScan();
        m_scheduler.stopAndWait();
        scanBarrier.store(nullptr);
    }

    ScanBarrierShutdown(const ScanBarrierShutdown&) = delete;
    ScanBarrierShutdown& operator=(const ScanBarrierShutdown&) = delete;

  private:
    EngineWorkerScheduler& m_scheduler;
    ScanBarrier& m_barrier;
};

TEST(EngineWorkerSchedulerTest, RetainsNotificationAfterWorkerScan) {
    SemaphoreProbeWorker firstWorker;
    SemaphoreProbeWorker secondWorker;
    EngineWorkerScheduler scheduler;
    ScanBarrier barrier;
    // This guard joins before the barrier, scheduler, or non-owning workers
    // are destroyed, including a fatal assertion's early return.
    const ScanBarrierShutdown shutdown(scheduler, barrier);
    firstWorker.setScheduler(&scheduler);
    secondWorker.setScheduler(&scheduler);
    scheduler.start();
    ASSERT_TRUE(barrier.waitForInitialScan());

    // Event::end has observed the completed scan, but the scheduler has not
    // yet parked. One callback notification must wake both newly ready workers.
    firstWorker.workReady();
    secondWorker.workReady();
    scheduler.runWorkers();
    barrier.releaseInitialScan();

    EXPECT_TRUE(firstWorker.waitForWake());
    EXPECT_TRUE(secondWorker.waitForWake());
}

} // namespace

void schedulerScanFinishedForTest() {
    if (auto* barrier = scanBarrier.load()) {
        barrier->afterScan();
    }
}
