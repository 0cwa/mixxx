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


// AI-generated bounded scheduler regression additions begin.
class SchedulerJoin final {
  public:
    explicit SchedulerJoin(EngineWorkerScheduler& scheduler)
            : m_scheduler(scheduler) {
    }

    ~SchedulerJoin() {
        m_scheduler.stopAndWait();
    }

  private:
    EngineWorkerScheduler& m_scheduler;
};

TEST(EngineWorkerSchedulerTest, RepeatedReadyCyclesResumeBothWorkers) {
    SemaphoreProbeWorker firstWorker;
    SemaphoreProbeWorker secondWorker;
    EngineWorkerScheduler scheduler;
    const SchedulerJoin join(scheduler);
    firstWorker.setScheduler(&scheduler);
    secondWorker.setScheduler(&scheduler);
    scheduler.start();
    for (int cycle = 0; cycle < 10000; ++cycle) {
        firstWorker.workReady();
        secondWorker.workReady();
        for (int duplicate = 0; duplicate < 16; ++duplicate) {
            scheduler.runWorkers();
        }
        ASSERT_TRUE(firstWorker.waitForWake()) << cycle;
        ASSERT_TRUE(secondWorker.waitForWake()) << cycle;
    }
}

class BlockedProbeWorker final : public EngineWorker {
  public:
    ~BlockedProbeWorker() override {
        finishAndWait();
    }

    void run() override {
        m_semaRun.acquire();
        m_entered.release();
        m_finish.acquire();
    }

    bool waitForWork() {
        return m_entered.tryAcquire(1, 5000);
    }

    void finishAndWait() {
        m_semaRun.release();
        m_finish.release();
        wait();
    }

  private:
    QSemaphore m_entered;
    QSemaphore m_finish;
};

TEST(EngineWorkerSchedulerTest, StopsWhileWorkerIsBlockedAndAllowsLateNotifications) {
    BlockedProbeWorker worker;
    EngineWorkerScheduler scheduler;
    worker.setScheduler(&scheduler);
    worker.start();
    worker.workReady();
    scheduler.start();
    const bool entered = worker.waitForWork();
    EXPECT_TRUE(entered);
    scheduler.stopAndWait();
    EXPECT_FALSE(scheduler.isRunning());
    if (entered) {
        EXPECT_TRUE(worker.isRunning());
    }
    for (int duplicate = 0; duplicate < 100; ++duplicate) {
        worker.workReady();
        scheduler.runWorkers();
    }
    scheduler.stopAndWait();
    EXPECT_FALSE(scheduler.isRunning());
    worker.finishAndWait();
}
// End AI-generated bounded scheduler regression additions.

} // namespace

void schedulerScanFinishedForTest() {
    if (auto* barrier = scanBarrier.load()) {
        barrier->afterScan();
    }
}
