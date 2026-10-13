#pragma once

#ifdef BUILD_TESTING

#include <atomic>
#include <cstddef>
#include <cstdint>

enum class F001RtProbeEventKindForTest : std::uint8_t {
    Allocation,
    PthreadMutex,
    WaitConditionWakeAllEnter,
    WaitConditionWakeAllExit,
    ScopeEnter,
    ScopeExit,
};

enum class F001RtAllocationApiForTest : std::uint8_t {
    None,
    Malloc,
    Calloc,
    Realloc,
    AlignedAlloc,
    PosixMemalign,
    ReallocArray,
};

// A witness names an actual C++ call only when its returned pointer matches
// a successful captured C allocator call in the same thread and scope.
enum class F001RtCppVariantForTest : std::uint8_t {
    Throwing,
    Array,
    Nothrow,
    ArrayNothrow,
    Aligned,
    AlignedArray,
    AlignedNothrow,
    AlignedArrayNothrow,
};

struct F001RtCppWitnessForTest {
    F001RtCppVariantForTest variant{};
    std::size_t requestedBytes{0};
    std::size_t requestedAlignment{0};
    F001RtAllocationApiForTest callee{F001RtAllocationApiForTest::None};
    bool successfulCall{false};
    bool matchedCallee{false};
    bool disarmedZero{false};
};

enum class F001RtPthreadApiForTest : std::uint8_t {
    None,
    Lock,
    TryLock,
    Unlock,
};

enum class F001RtProbeScopeForTest : std::uint8_t {
    MixerProcess,
    SchedulerRunWorkers,
    SchedulerWakeAll,
    SchedulerDestructorWake,
    WorkerProcessLoopCueUpdate,
    Seek30UpdateLoopCue,
    CueControlTrackLoaded,
    CueControlTrackCuesUpdated,
    CueControlLoadCuesFromTrack,
    CueControlQuantizeRenderer,
    CueControlQuantizeExplicit,
    TrackRemoveTempLoopCue,
    TrackCreateAndAddCue,
    TrackSlotCueUpdated,
    CueSetStartAndEndPosition,
    PthreadCalibration,
    AllocatorCalibration,
    QtWakeCalibration,
};

struct F001RtProbeEventForTest {
    F001RtProbeEventKindForTest kind{F001RtProbeEventKindForTest::Allocation};
    F001RtAllocationApiForTest allocationApi{F001RtAllocationApiForTest::None};
    F001RtPthreadApiForTest pthreadApi{F001RtPthreadApiForTest::None};
    std::uint32_t caseToken{0};
    std::uint32_t resultCode{0};
    std::uint64_t threadId{0};
    std::uint64_t activeScopeMask{0};
    F001RtProbeScopeForTest scope{F001RtProbeScopeForTest::MixerProcess};
    std::size_t requestedBytes{0};
    std::size_t requestedAlignment{0};
    std::uintptr_t resultIdentity{0};
    std::uintptr_t argumentIdentity{0};
    std::uintptr_t callerProgramCounter{0};
};

struct F001RtProbeThreadBufferForTest {
    static constexpr std::size_t kThreadCapacity = 64;
    static constexpr std::size_t kCapacity = 512;
    std::uint64_t threadId{0};
    std::atomic<std::uint32_t> eventCount{0};
    std::atomic<bool> eventOverflow{false};
    std::atomic<bool> scopeOverflow{false};
    F001RtProbeEventForTest events[kCapacity]{};
};

#if defined(__linux__)
extern "C" {
bool f001RtProbeInitializeForTest() noexcept __attribute__((weak));
bool f001RtProbePrepareThreadForTest(std::uint32_t token) noexcept __attribute__((weak));
bool f001RtProbeSetThreadTokenForTest(std::uint32_t token) noexcept __attribute__((weak));
void f001RtProbeArmForTest(bool armed) noexcept __attribute__((weak));
bool f001RtProbeResetForTest() noexcept __attribute__((weak));
bool f001RtProbeQuiescentForTest() noexcept __attribute__((weak));
bool f001RtProbeSelectedScopesIdleForTest() noexcept __attribute__((weak));
bool f001RtProbeOverflowedForTest() noexcept __attribute__((weak));
bool f001RtProbeCalibrateAllocatorsForTest() noexcept __attribute__((weak));
bool f001RtProbeCalibratePthreadForTest() noexcept __attribute__((weak));
bool f001RtProbeCalibrateWaitConditionForTest() noexcept __attribute__((weak));
bool f001RtProbeCalibrateCapacityForTest() noexcept __attribute__((weak));
const F001RtCppWitnessForTest* f001RtProbeCalibrateCppForTest(
        std::size_t* pCount) noexcept __attribute__((weak));
const F001RtProbeThreadBufferForTest* f001RtProbeBuffersForTest(
        std::size_t* pCount) noexcept __attribute__((weak));
bool f001RtProbeEnterScopeForTest(
        F001RtProbeScopeForTest scope, std::uint32_t caseToken) noexcept
        __attribute__((weak));
void f001RtProbeLeaveScopeForTest() noexcept __attribute__((weak));
}
#else
inline bool f001RtProbeInitializeForTest() noexcept {
    return false;
}
inline bool f001RtProbePrepareThreadForTest(std::uint32_t) noexcept {
    return false;
}
inline bool f001RtProbeSetThreadTokenForTest(std::uint32_t) noexcept {
    return false;
}
inline void f001RtProbeArmForTest(bool) noexcept {
}
inline bool f001RtProbeResetForTest() noexcept {
    return false;
}
inline bool f001RtProbeQuiescentForTest() noexcept {
    return false;
}
inline bool f001RtProbeSelectedScopesIdleForTest() noexcept {
    return false;
}
inline bool f001RtProbeOverflowedForTest() noexcept {
    return false;
}
inline bool f001RtProbeCalibrateAllocatorsForTest() noexcept {
    return false;
}
inline bool f001RtProbeCalibratePthreadForTest() noexcept {
    return false;
}
inline bool f001RtProbeCalibrateWaitConditionForTest() noexcept {
    return false;
}
inline bool f001RtProbeCalibrateCapacityForTest() noexcept {
    return false;
}
inline const F001RtCppWitnessForTest* f001RtProbeCalibrateCppForTest(
        std::size_t* pCount) noexcept {
    if (pCount) {
        *pCount = 0;
    }
    return nullptr;
}
inline const F001RtProbeThreadBufferForTest* f001RtProbeBuffersForTest(
        std::size_t* pCount) noexcept {
    if (pCount) {
        *pCount = 0;
    }
    return nullptr;
}
inline bool f001RtProbeEnterScopeForTest(
        F001RtProbeScopeForTest, std::uint32_t) noexcept {
    return false;
}
inline void f001RtProbeLeaveScopeForTest() noexcept {
}
#endif

class F001RtProbeScopeGuardForTest final {
  public:
    explicit F001RtProbeScopeGuardForTest(
            F001RtProbeScopeForTest scope, std::uint32_t caseToken = 0) noexcept
            : m_active(f001RtProbeEnterScopeForTest &&
                    f001RtProbeEnterScopeForTest(scope, caseToken)) {
    }
    ~F001RtProbeScopeGuardForTest() {
        if (m_active && f001RtProbeLeaveScopeForTest) {
            f001RtProbeLeaveScopeForTest();
        }
    }

    F001RtProbeScopeGuardForTest(const F001RtProbeScopeGuardForTest&) = delete;
    F001RtProbeScopeGuardForTest& operator=(const F001RtProbeScopeGuardForTest&) = delete;

  private:
    bool m_active;
};

#endif // BUILD_TESTING
