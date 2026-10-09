#include <gtest/gtest.h>

#include <chrono>

#include "test/visualplaypositiontestutils.h"
#include "util/performancetimer.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/visualplayposition.h"

#ifdef MIXXX_USE_QOPENGL
#include <cmath>
#include <vector>

#include "control/controlobject.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "waveform/renderers/allshader/waveformrendererrgb.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"
#endif

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
    EXPECT_NEAR(0.406, mixxx::test::playPositionAtNextVSync(position, &vsync), 1e-12);
}

TEST(VisualPlayPositionTest, ReverseInterpolationUsesSignedPlayRate) {
    VisualPlayPosition position;
    FixedVSyncProvider vsync;
    setPosition(&position, 0.4, -0.6, 0.01);

    EXPECT_NEAR(0.397, mixxx::test::playPositionAtNextVSync(position, &vsync), 1e-12);
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

    EXPECT_NEAR(0.21, mixxx::test::playPositionAtNextVSync(forward, &vsync), 1e-12);
    EXPECT_NEAR(0.59, mixxx::test::playPositionAtNextVSync(reverse, &vsync), 1e-12);
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

    EXPECT_DOUBLE_EQ(0.37, mixxx::test::playPositionAtNextVSync(position, &vsync));
}

#ifdef MIXXX_USE_QOPENGL
namespace {

class WaveformWidgetRendererTransitionTest
        : public MixxxTest,
          public testing::WithParamInterface<int> {
  protected:
    void SetUp() override {
        m_rateRatio.set(1.0);
        m_gain.set(1.0);
        ASSERT_TRUE(m_renderer.init());
        m_renderer.resizeRenderer(kWidth, 100, 1.0f);
        m_renderer.setPlayMarkerPosition(0.5);
        m_renderer.setZoom(1.0);

        // A stationary publication avoids wall-clock and global DAC timing.
        // No engine thread writes this private test group's visual position.
        m_position->set(kPlayPosition,
                0.0,
                0.0,
                0.0,
                0.0,
                SlipModeState::Disabled,
                false,
                false,
                false,
                0.0,
                0.0,
                kSeconds,
                0.0);

        // Preprocessing supports absent EQ proxies with unity gains.
        // Do not initialize a rendergraph engine or OpenGL resources.
        m_signal.setLowColor(QColor(Qt::red));
        m_signal.setMidColor(QColor(Qt::green));
        m_signal.setHighColor(QColor(Qt::blue));
    }

    TrackPointer selectColdTrack(int sampleRate) {
        auto track = Track::newTemporary();
        track->setAudioProperties(
                mixxx::audio::ChannelCount::stereo(),
                mixxx::audio::SampleRate(sampleRate),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(kSeconds));
        m_trackSamples.set(2.0 * sampleRate * kSeconds);
        m_renderer.setTrack(track);
        return track;
    }

    static WaveformPointer makePatternedWaveform(
            int sampleRate, int visualSampleRate) {
        WaveformPointer waveform(new Waveform(
                sampleRate,
                sampleRate * kSeconds,
                visualSampleRate,
                -1,
                0));
        for (int i = 0; i < waveform->getDataSize(); ++i) {
            auto& data = waveform->data()[i].filtered;
            data.low = 32 + (i / 2) % 31 * 7;
            data.mid = 20 + (i / 2) % 17 * 11;
            data.high = 16 + (i / 2) % 13 * 13;
            data.all = 32 + (i / 2) % 31 * 7;
        }
        waveform->setCompletion(waveform->getDataSize());
        return waveform;
    }

    void expectCoordinates(int sampleRate,
            double visualSamplesPerPixel,
            double audioVisualRatio) {
        // Independent input-derived oracles. Do not call waveformscale.h
        // or derive expected values from renderer/waveform getters.
        const double frames = sampleRate * kSeconds;
        const double audioSamplesPerPixel =
                visualSamplesPerPixel * audioVisualRatio;
        const double playFrame = kPlayPosition * frames;
        const double roundedPlayPixel =
                std::round(playFrame / audioSamplesPerPixel);
        const double expectedFirst =
                (roundedPlayPixel - kWidth * 0.5) *
                audioSamplesPerPixel / frames;
        const double neighbourSample = 2.0 * (playFrame + 1600.0);
        const double expectedNeighbourPixel =
                kWidth * 0.5 +
                (playFrame + 1600.0) / audioSamplesPerPixel -
                roundedPlayPixel;

        EXPECT_DOUBLE_EQ(2.0 * frames, m_renderer.getTrackSamples());
        EXPECT_DOUBLE_EQ(
                visualSamplesPerPixel, m_renderer.getVisualSamplePerPixel());
        EXPECT_NEAR(audioSamplesPerPixel,
                m_renderer.getAudioSamplePerPixel(),
                1e-9);
        EXPECT_NEAR(2.0 * playFrame, m_renderer.getTruePosSample(), 1e-9);
        EXPECT_NEAR(expectedFirst,
                m_renderer.getFirstDisplayedPosition(),
                1e-12);
        EXPECT_NEAR(kWidth * audioSamplesPerPixel / frames,
                m_renderer.getLastDisplayedPosition() -
                        m_renderer.getFirstDisplayedPosition(),
                1e-12);
        // This deliberately misses the transform's playhead special case.
        EXPECT_NEAR(expectedNeighbourPixel,
                m_renderer.transformSamplePositionInRendererWorld(
                        neighbourSample),
                1e-9);
    }

    void expectInstalledFrameMatchesWarmFrame() {
        // Crucially there is no onPreRender() between waveform installation
        // and this real production consumer of the cached cold geometry.
        m_signal.preprocess();
        auto& geometry = m_signal.geometry();
        ASSERT_EQ(6 * (kWidth + 1), geometry.vertexCount());
        using Vertex = rendergraph::Geometry::RGBColoredPoint2D;
        const auto* vertices = geometry.vertexDataAs<Vertex>();
        const std::vector<Vertex> installedFrame(
                vertices, vertices + geometry.vertexCount());

        // Reject a vacuous axis-only or flat-signal comparison.
        EXPECT_LT(installedFrame[6].position2D.y(), 50.0f);

        m_renderer.onPreRender(&m_vsync);
        m_signal.preprocess();
        ASSERT_EQ(static_cast<int>(installedFrame.size()),
                geometry.vertexCount());
        vertices = geometry.vertexDataAs<Vertex>();
        for (int i = 0; i < geometry.vertexCount(); ++i) {
            EXPECT_EQ(installedFrame[i].position2D, vertices[i].position2D)
                    << "vertex " << i;
            EXPECT_EQ(installedFrame[i].color3D, vertices[i].color3D)
                    << "vertex " << i;
        }
    }

    static constexpr int kWidth = 256;
    static constexpr int kSeconds = 10;
    static constexpr double kPlayPosition = 0.375;

    // Declaration order keeps controls and position alive until the
    // renderer is destroyed; the standalone signal node is destroyed first.
    const QString m_group = QStringLiteral("[WaveformTransitionTest]");
    ControlObject m_rateRatio{ConfigKey(m_group, QStringLiteral("rate_ratio"))};
    ControlObject m_gain{ConfigKey(m_group, QStringLiteral("total_gain"))};
    ControlObject m_trackSamples{
            ConfigKey(m_group, QStringLiteral("track_samples"))};
    QSharedPointer<VisualPlayPosition> m_position{
            VisualPlayPosition::getVisualPlayPosition(m_group)};
    WaveformWidgetRenderer m_renderer{m_group};
    FixedVSyncProvider m_vsync;
    allshader::WaveformRendererRGB m_signal{&m_renderer};
};

TEST_P(WaveformWidgetRendererTransitionTest,
        ColdInstallMatchesWarmPreprocessing) {
    const int sampleRate = GetParam();
    auto track = selectColdTrack(sampleRate);
    ASSERT_FALSE(track->getWaveform());

    m_renderer.onPreRender(&m_vsync);
    expectCoordinates(sampleRate, 1.0, sampleRate / 441.0);
    m_signal.preprocess();
    ASSERT_EQ(0, m_signal.geometry().vertexCount());

    track->setWaveform(makePatternedWaveform(sampleRate, 441));
    expectInstalledFrameMatchesWarmFrame();
    expectCoordinates(sampleRate, 1.0, sampleRate / 441.0);
}

TEST_P(WaveformWidgetRendererTransitionTest,
        StoredRatioAndWarmToColdTransitions) {
    const int sampleRate = GetParam();
    auto track = selectColdTrack(sampleRate);

    // A real serialization round-trip exercises stored waveform metadata.
    const auto original = makePatternedWaveform(sampleRate, 220);
    ConstWaveformPointer stored(new Waveform(original->toByteArray()));
    ASSERT_EQ(Waveform::SaveState::Saved, stored->saveState());
    ASSERT_DOUBLE_EQ(sampleRate / 220.0, stored->getAudioVisualRatio());
    track->setWaveform(stored);
    m_renderer.onPreRender(&m_vsync);
    expectCoordinates(sampleRate, 1.0, sampleRate / 220.0);
    m_signal.preprocess();
    ASSERT_EQ(6 * (kWidth + 1), m_signal.geometry().vertexCount());

    // Clear the same track, changing scale inputs without reinitializing
    // the renderer. The previous stored ratio must not survive.
    track->setWaveform(ConstWaveformPointer{});
    m_renderer.setZoom(3.0);
    m_rateRatio.set(1.25);
    m_renderer.onPreRender(&m_vsync);
    expectCoordinates(sampleRate, 3.75, sampleRate / 441.0);
    m_signal.preprocess();
    ASSERT_EQ(0, m_signal.geometry().vertexCount());
    track->setWaveform(makePatternedWaveform(sampleRate, 441));
    expectInstalledFrameMatchesWarmFrame();
    expectCoordinates(sampleRate, 3.75, sampleRate / 441.0);

    // Reuse the warm renderer for a cold track at the other sample rate.
    const int nextSampleRate = sampleRate == 44100 ? 48000 : 44100;
    auto nextTrack = selectColdTrack(nextSampleRate);
    ASSERT_FALSE(nextTrack->getWaveform());
    m_renderer.setZoom(2.0);
    m_rateRatio.set(0.75);
    m_renderer.onPreRender(&m_vsync);
    expectCoordinates(nextSampleRate, 1.5, nextSampleRate / 441.0);
    m_signal.preprocess();
    ASSERT_EQ(0, m_signal.geometry().vertexCount());
    nextTrack->setWaveform(makePatternedWaveform(nextSampleRate, 441));
    expectInstalledFrameMatchesWarmFrame();
    expectCoordinates(nextSampleRate, 1.5, nextSampleRate / 441.0);
}

INSTANTIATE_TEST_SUITE_P(SampleRates,
        WaveformWidgetRendererTransitionTest,
        testing::Values(44100, 48000));

} // namespace
#endif
