#pragma once

#ifdef BUILD_TESTING

#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <QObject>
#include <QSemaphore>

enum class F001ProbeEventForTest : std::uint16_t {
    LoopScalarRead,
    WorkerLoopCueUpdate,
    Seek30LoopCueUpdate,
    EngineSlotTrackLoaded,
    EngineNotifyTrackLoaded,
    CueControlFrameInfo,
    CueControlTrackLoaded,
    CueControlTrackCuesUpdated,
    CueControlLoadCues,
    CueControlQuantizeExplicit,
    CueControlQuantizeRenderer,
    TrackCueUpdated,
    TrackCreateCue,
    TrackRemoveTemporaryCue,
    TrackMutex,
    CueSetStartAndEnd,
    CueMutex,
    CueSignalDirect,
    CueSignalQueued,
    TrackSignalDirect,
    TrackSignalQueued,
};

enum class F001ProbePhaseForTest : std::uint8_t {
    Point,
    Before,
    After,
    Attempt,
    Acquired,
    Released,
    Clear,
    Install,
    Connect,
};

enum class F001ProbeMethodForTest : std::uint8_t {
    None,
    TrackLoaded,
    LoadCuesFromTrack,
    GetCuePoints,
    GetMainCuePosition,
    MarkDirty,
    CreateAndAddCue,
    RemoveTempLoopCue,
    SlotCueUpdated,
    CueGetType,
    CueGetHotCue,
    CueGetPosition,
    CueGetStartAndEndPosition,
    CueSetStartAndEndPosition,
};

// These values describe only the selected source-visible methods. In
// particular, they say nothing about locks inside Qt or ControlObjects.

struct F001ProbeEntryForTest {
    std::uint64_t sequence{0};
    std::uint32_t caseToken{0};
    F001ProbeEventForTest event{F001ProbeEventForTest::LoopScalarRead};
    F001ProbePhaseForTest phase{F001ProbePhaseForTest::Point};
    F001ProbeMethodForTest method{F001ProbeMethodForTest::None};
    std::uintptr_t objectIdentity{0};
    std::uintptr_t relatedIdentity{0};
    std::uintptr_t currentThreadIdentity{0};
    std::uintptr_t affinityIdentity{0};
    double positionA{0};
    double positionB{0};
    std::uint64_t scalarBits{0};
    std::uint32_t flags{0};
};

// One fixture-owned recorder is shared explicitly with the exercised objects.
// append() uses a bounded atomic reservation and does not format or allocate.
class F001ProbeForTest final {
  public:
    // Getter mutex phases can occur repeatedly during a load and refresh.
    // Overflow still fails the case; the larger cap avoids truncating normal
    // two-track replacement evidence before it can be reviewed.
    static constexpr std::size_t kCapacity = 256;

    void setCaseToken(std::uint32_t token) noexcept {
        m_caseToken.store(token, std::memory_order_relaxed);
    }

    std::uint32_t caseToken() const noexcept {
        return m_caseToken.load(std::memory_order_relaxed);
    }

    void record(F001ProbeEventForTest event,
            F001ProbePhaseForTest phase,
            F001ProbeMethodForTest method,
            const void* object,
            const void* related = nullptr,
            double positionA = 0,
            double positionB = 0,
            std::uint64_t scalarBits = 0,
            std::uint32_t flags = 0,
            const QThread* affinity = nullptr) noexcept {
        const auto index = m_next.fetch_add(1, std::memory_order_relaxed);
        if (index >= m_entries.size()) {
            m_overflow.store(true, std::memory_order_relaxed);
            return;
        }

        auto* const currentThread = QThread::currentThread();
        m_entries[index] = {
                index + 1,
                caseToken(),
                event,
                phase,
                method,
                reinterpret_cast<std::uintptr_t>(object),
                reinterpret_cast<std::uintptr_t>(related),
                reinterpret_cast<std::uintptr_t>(currentThread),
                reinterpret_cast<std::uintptr_t>(affinity),
                positionA,
                positionB,
                scalarBits,
                flags};
    }

    void recordObject(F001ProbeEventForTest event,
            F001ProbePhaseForTest phase,
            F001ProbeMethodForTest method,
            const QObject* object,
            const void* related = nullptr,
            double positionA = 0,
            double positionB = 0,
            std::uint64_t scalarBits = 0,
            std::uint32_t flags = 0) noexcept {
        record(event,
                phase,
                method,
                object,
                related,
                positionA,
                positionB,
                scalarBits,
                flags,
                object ? object->thread() : nullptr);
    }

    std::size_t count() const noexcept {
        return std::min(m_next.load(std::memory_order_relaxed), m_entries.size());
    }

    bool overflowed() const noexcept {
        return m_overflow.load(std::memory_order_relaxed);
    }

    const F001ProbeEntryForTest& entry(std::size_t index) const noexcept {
        return m_entries[index];
    }

  private:
    std::array<F001ProbeEntryForTest, kCapacity> m_entries{};
    std::atomic<std::size_t> m_next{0};
    std::atomic<std::uint32_t> m_caseToken{0};
    std::atomic<bool> m_overflow{false};
};

// Install only while the owning fixture can outlive every blocked worker.
// The worker returns after two seconds if the fixture cannot release it.
struct F001LoadCuesHoldForTest {
    QSemaphore entered;
    QSemaphore trackLoadAttempt;
    QSemaphore release;
    std::atomic<bool> timedOut{false};
};

inline void f001RecordObjectForTest(
        const std::atomic<F001ProbeForTest*>& probeSlot,
        F001ProbeEventForTest event,
        F001ProbePhaseForTest phase,
        F001ProbeMethodForTest method,
        const QObject* object,
        const void* related = nullptr,
        double positionA = 0,
        double positionB = 0,
        std::uint64_t scalarBits = 0,
        std::uint32_t flags = 0) noexcept {
    if (auto* pProbe = probeSlot.load(std::memory_order_acquire)) {
        pProbe->recordObject(event,
                phase,
                method,
                object,
                related,
                positionA,
                positionB,
                scalarBits,
                flags);
    }
}

inline std::uint32_t f001CaseTokenForTest(
        const std::atomic<F001ProbeForTest*>& probeSlot) noexcept {
    auto* const pProbe = probeSlot.load(std::memory_order_acquire);
    return pProbe ? pProbe->caseToken() : 0;
}

inline std::uint64_t f001DoubleBitsForTest(double value) noexcept {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

#endif // BUILD_TESTING
