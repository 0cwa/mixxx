#include <gtest/gtest.h>

#include <chrono>

#include "util/performancetimer.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/visualplayposition.h"

namespace {

constexpr int kVSyncOffsetMicros = 5000;
constexpr int kSyncIntervalMicros = 16667;
constexpr double kAudioBufferMicros = 10000.0;

class FixedVSyncProvider final : public VSyncTimeProvider {
  public:
    std::chrono::microseconds fromTimerToNextSync(
            const PerformanceTimer&) override {
        return std::chrono::microseconds(kVSyncOffsetMicros);
    }

    std::chrono::microseconds getSyncInterval() const override {
        return std::chrono::microseconds(kSyncIntervalMicros);
    }
};

void setPosition(VisualPlayPosition* pPosition,
        double playPosition,
        double playRate,
        double positionStep,
        double slipPosition = 0.0,
        double slipRate = 0.0,
        SlipModeState slipModeState = SlipModeState::Disabled,
        bool loopEnabled = false,
        bool loopInAdjustActive = false,
        bool loopOutAdjustActive = false,
        double loopStartPosition = 0.0,
        double loopEndPosition = 0.0) {
    pPosition->set(playPosition,
            playRate,
            positionStep,
            slipPosition,
            slipRate,
            slipModeState,
            loopEnabled,
            loopInAdjustActive,
            loopOutAdjustActive,
            loopStartPosition,
            loopEndPosition,
            120.0,
            kAudioBufferMicros);
}

} // namespace

TEST(VisualPlayPositionTest, ForwardInterpolationUsesPositionStep) {
    VisualPlayPosition position;
    FixedVSyncProvider vsync;
    setPosition(&position, 0.4, 1.2, 0.01);

    // 5 ms is one half of the declared 10 ms audio buffer.
    EXPECT_NEAR(0.406, position.getAtNextVSync(&vsync), 1e-12);
}

TEST(VisualPlayPositionTest, ReverseInterpolationUsesSignedPlayRate) {
    VisualPlayPosition position;
    FixedVSyncProvider vsync;
    setPosition(&position, 0.4, -0.6, 0.01);

    EXPECT_NEAR(0.397, position.getAtNextVSync(&vsync), 1e-12);
}

TEST(VisualPlayPositionTest, LoopInterpolationWrapsForwardAndReverse) {
    VisualPlayPosition forward;
    VisualPlayPosition reverse;
    FixedVSyncProvider vsync;

    setPosition(&forward,
            0.59,
            1.0,
            0.04,
            0.0,
            0.0,
            SlipModeState::Disabled,
            true,
            false,
            false,
            0.2,
            0.6);
    setPosition(&reverse,
            0.21,
            -1.0,
            0.04,
            0.0,
            0.0,
            SlipModeState::Disabled,
            true,
            false,
            false,
            0.2,
            0.6);

    EXPECT_NEAR(0.21, forward.getAtNextVSync(&vsync), 1e-12);
    EXPECT_NEAR(0.59, reverse.getAtNextVSync(&vsync), 1e-12);
}

TEST(VisualPlayPositionTest, SlipRunningUsesIndependentSlipClock) {
    VisualPlayPosition position;
    FixedVSyncProvider vsync;
    setPosition(&position,
            0.4,
            1.0,
            0.02,
            0.1,
            0.5,
            SlipModeState::Running);

    double playPosition = 0.0;
    double slipPosition = 0.0;
    position.getPlaySlipAtNextVSync(&vsync, &playPosition, &slipPosition);

    EXPECT_NEAR(0.41, playPosition, 1e-12);
    EXPECT_NEAR(0.105, slipPosition, 1e-12);
}

TEST(VisualPlayPositionTest, NoAudioBufferDoesNotInventTransportOffset) {
    VisualPlayPosition position;
    FixedVSyncProvider vsync;
    position.set(0.37,
            3.0,
            0.2,
            0.0,
            0.0,
            SlipModeState::Disabled,
            false,
            false,
            false,
            0.0,
            0.0,
            120.0,
            0.0);

    EXPECT_DOUBLE_EQ(0.37, position.getAtNextVSync(&vsync));
}
