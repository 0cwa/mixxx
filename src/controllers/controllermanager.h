#pragma once

#include <QMutex>
#include <QSharedPointer>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "controllers/controllerenumerator.h"
#include "preferences/usersettings.h"
#include "util/duration.h"

// Forward declaration(s)
class Controller;
class ControllerLearningEventFilter;
class MappingInfoEnumerator;
class LegacyControllerMapping;
class ControllerEnumerator;

/// Function to sort controllers by name
bool controllerCompare(Controller *a, Controller *b);

/// Manages enumeration/operation/deletion of hardware controllers.
class ControllerManager : public QObject {
    Q_OBJECT
  public:
    /// Cumulative, best-effort timing counters for the controller poll loop.
    /// Values are sampled independently and are not a transactionally
    /// consistent snapshot.
    struct PollTimingStatistics {
        bool enabled{false};
        std::uint64_t pollSweepCount{0};
        std::uint64_t overrunCount{0};
        std::uint64_t skippedPollCount{0};
        std::int64_t maxPollDurationNanos{0};
        std::uint64_t overNominalEntryIntervalCount{0};
        std::int64_t maxEntryIntervalNanos{0};
    };

    explicit ControllerManager(UserSettingsPointer pConfig);
    ~ControllerManager() override;

    static const mixxx::Duration kPollInterval;

    QList<Controller*> getControllers() const;
    QList<Controller*> getControllerList(bool outputDevices=true, bool inputDevices=true);
    ControllerLearningEventFilter* getControllerLearningEventFilter() const;
    QSharedPointer<MappingInfoEnumerator> getMainThreadUserMappingEnumerator() const {
        return m_pMainThreadUserMappingEnumerator;
    }
    QSharedPointer<MappingInfoEnumerator> getMainThreadSystemMappingEnumerator() const {
        return m_pMainThreadSystemMappingEnumerator;
    }
    QString getConfiguredMappingFileForDevice(const QString& name) const;

    /// Read cumulative poll timing counters from a low-rate, non-controller
    /// thread. Recording is enabled only by the temporary opt-in environment
    /// variable documented in controllermanager.cpp.
    PollTimingStatistics getPollTimingStatistics() const;

    /// Prevent other parts of Mixxx from having to manually connect to our slots
    void setUpDevices() { emit requestSetUpDevices(); };

    static QList<QString> getMappingPaths(UserSettingsPointer pConfig);

  signals:
    void initialized();
    void devicesChanged();
    void requestSetUpDevices();
    void requestShutdown();
    void requestInitialize();
    void mappingApplied(bool applied);

  public slots:
    void slotApplyMapping(Controller* pController,
            std::shared_ptr<LegacyControllerMapping> pMapping,
            bool bEnabled);

  private slots:
    /// Perform initialization that should be delayed until the ControllerManager
    /// thread is started.
    void slotInitialize();
    /// Open whatever controllers are selected in the preferences. This currently
    /// only runs on start-up but maybe should instead be signaled by the
    /// preferences dialog on apply, and only open/close changed devices
    void slotSetUpDevices();
    void slotShutdown();
    /// Calls poll() on all devices that have isPolling() true.
    void slotPollDevices();

  private:
    void updateControllerList();
    void startPolling();
    void stopPolling();
    void pollIfAnyControllersOpen();
    void openController(Controller* pController);
    void closeController(Controller* pController);

    UserSettingsPointer m_pConfig;
    // WARNING: Do not parent m_pControllerLearningEventFilter to ControllerManager
    // because the CM is moved to its own thread and runs its own event loop.
    std::unique_ptr<ControllerLearningEventFilter> m_pControllerLearningEventFilter;
    QTimer m_pollTimer;
    mutable QMutex m_mutex;
    /// Guarded by m_mutex for main-thread reads.
    /// Written/iterated only on the ControllerManager thread
    std::vector<std::unique_ptr<ControllerEnumerator>> m_enumerators;
    /// Non-owning; Controllers are owned by their respective ControllerEnumerator.
    /// Guarded by m_mutex for main-thread reads.
    /// Written only on the ControllerManager thread
    QList<Controller*> m_controllers;
    /// The single shared background thread that drives the entire ControllerManager,
    /// all ControllerEnumerators, and all Controller instances.
    std::unique_ptr<QThread> m_pThread;
    /// Written once on the ControllerManager thread during slotInitialize(),
    /// before initialized() is emitted. Afterwards only read from the main thread
    QSharedPointer<MappingInfoEnumerator> m_pMainThreadUserMappingEnumerator;
    QSharedPointer<MappingInfoEnumerator> m_pMainThreadSystemMappingEnumerator;
    /// Accessed only from the ControllerManager thread via slotPollDevices().
    bool m_skipPoll;
    /// Set before the ControllerManager thread starts, then read-only.
    const bool m_pollTimingDiagnosticsEnabled;
    /// Last timer callback entry, accessed only on the ControllerManager thread.
    mixxx::Duration m_lastPollEntryTime;
    bool m_hasLastPollEntryTime{false};
    /// Independently sampled by getPollTimingStatistics(); no poll-path locks.
    std::atomic<std::uint64_t> m_pollSweepCount{0};
    std::atomic<std::uint64_t> m_overrunCount{0};
    std::atomic<std::uint64_t> m_skippedPollCount{0};
    std::atomic<std::int64_t> m_maxPollDurationNanos{0};
    std::atomic<std::uint64_t> m_overNominalEntryIntervalCount{0};
    std::atomic<std::int64_t> m_maxEntryIntervalNanos{0};
};
