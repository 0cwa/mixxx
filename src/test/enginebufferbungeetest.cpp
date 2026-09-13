// Integration tests for EngineBuffer with the Bungee keylock engine.
//
// These tests exercise the REAL EngineBufferScaleBungee (not a mock scaler)
// so that engine-level state management — keylock enable/disable, engine
// switching, multi-buffer continuity — is validated end-to-end.
// setScalerForTest() is deliberately NOT called; m_bScalerOverride stays
// false so the normal keylock-engine selection code path is exercised.
//
// Higher-level regression coverage for keylock toggling with Bungee.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include "control/controlobject.h"
#include "engine/bufferscalers/enginebufferscalebungee.h"
#include "engine/enginebuffer.h"
#include "test/mockedenginebackendtest.h"
#include "test/signalpathtest.h"
#include "track/track.h"

#ifdef __STEM__
namespace {
constexpr std::size_t kLayoutTransitionFrames = 256;
constexpr std::size_t kStereoSamples = kLayoutTransitionFrames * 2;
constexpr CSAMPLE kPoison = 12345.0f;

class LayoutTestScaler final : public MockScaler {
  public:
    void resetCalls() {
        m_scaleBufferCalls = 0;
    }

    double scaleBuffer(CSAMPLE* pOutput, SINT bufferSize) override {
        ++m_scaleBufferCalls;
        std::fill_n(pOutput, bufferSize, 1.0f);
        return bufferSize /
                getOutputSignal().getChannelCount();
    }

    int scaleBufferCalls() const {
        return m_scaleBufferCalls;
    }

  private:
    int m_scaleBufferCalls = 0;
};
} // namespace

class EngineBufferBungeeLayoutTest : public BaseSignalPathTest {
  protected:
    static TrackPointer makeTrack(mixxx::audio::ChannelCount channelCount) {
        auto pTrack = Track::newTemporary();
        pTrack->setAudioProperties(
                channelCount,
                mixxx::audio::SampleRate(44100),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(60));
        pTrack->trySetBpm(128.0);
        return pTrack;
    }

    static bool hasOnlyFiniteNonPoisonSamples(
            std::span<const CSAMPLE> samples) {
        return std::all_of(samples.begin(), samples.end(), [](CSAMPLE sample) {
            return std::isfinite(sample) && sample != kPoison;
        });
    }

    void addStemHandles() {
        for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; ++stemIdx) {
            const auto stemHandleGroup = m_pEngineMixer->registerChannelGroup(
                    EngineDeck::getGroupForStem(m_pChannel1->getGroup(), stemIdx));
            m_pChannel1->addStemHandle(stemHandleGroup);
        }
    }

    void warmScaler(EngineBuffer* pEngineBuffer,
            LayoutTestScaler* pScaler,
            mixxx::audio::ChannelCount channelCount,
            std::size_t outputBufferSize) {
        pScaler->setSignal(mixxx::audio::SampleRate(44100), channelCount);
        pEngineBuffer->setScalerForTest(pScaler, pScaler);
        ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);
        std::array<CSAMPLE, kStereoSamples> output;
        std::fill(output.begin(), output.end(), kPoison);
        pScaler->resetCalls();
        m_pChannel1->process(output.data(), outputBufferSize);
        ASSERT_GT(pScaler->scaleBufferCalls(), 0);
        ASSERT_TRUE(hasOnlyFiniteNonPoisonSamples(
                std::span<const CSAMPLE>(output.data(), outputBufferSize)));
    }

    void assertStaleLayoutIsCleared(EngineBuffer* pEngineBuffer,
            LayoutTestScaler* pScaler,
            TrackPointer pNewTrack,
            mixxx::audio::ChannelCount oldChannelCount,
            mixxx::audio::ChannelCount newChannelCount,
            std::size_t outputBufferSize) {
        pEngineBuffer->loadFakeTrack(pNewTrack, true);
        ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);
        ASSERT_EQ(pEngineBuffer->getChannelCount(), newChannelCount);
        ASSERT_EQ(pScaler->getOutputSignal().getChannelCount(), oldChannelCount);

        std::array<CSAMPLE, kStereoSamples> output;
        std::fill(output.begin(), output.end(), kPoison);
        pScaler->resetCalls();
        const double playPosBefore = pEngineBuffer->getPlayPos().value();
        m_pChannel1->process(output.data(), outputBufferSize);
        const double playPosAfter = pEngineBuffer->getPlayPos().value();

        ASSERT_EQ(pScaler->scaleBufferCalls(), 0);
        ASSERT_TRUE(hasOnlyFiniteNonPoisonSamples(
                std::span<const CSAMPLE>(output.data(), outputBufferSize)));
        ASSERT_GE(playPosAfter, playPosBefore);
    }
};

TEST_F(EngineBufferBungeeLayoutTest, StaleScalerLayoutIsClearedBothDirections) {
    const auto pStereoTrack = makeTrack(mixxx::audio::ChannelCount::stereo());
    const auto pStemTrack = makeTrack(mixxx::audio::ChannelCount::stem());
    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    LayoutTestScaler scaler;

    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::Bungee));
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock")), 1.0);
    addStemHandles();

    pEngineBuffer->loadFakeTrack(pStereoTrack, true);
    warmScaler(pEngineBuffer,
            &scaler,
            mixxx::audio::ChannelCount::stereo(),
            kStereoSamples);
    assertStaleLayoutIsCleared(pEngineBuffer,
            &scaler,
            pStemTrack,
            mixxx::audio::ChannelCount::stereo(),
            mixxx::audio::ChannelCount::stem(),
            kStereoSamples);

    warmScaler(pEngineBuffer,
            &scaler,
            mixxx::audio::ChannelCount::stem(),
            kStereoSamples);
    assertStaleLayoutIsCleared(pEngineBuffer,
            &scaler,
            pStereoTrack,
            mixxx::audio::ChannelCount::stem(),
            mixxx::audio::ChannelCount::stereo(),
            kStereoSamples);
}
#endif // __STEM__

// Helper: scan a span for NaN / Inf.
static bool spanHasInvalidSamples(std::span<const CSAMPLE> buf) {
    for (const CSAMPLE s : buf) {
        if (!std::isfinite(s)) {
            return true;
        }
    }
    return false;
}

// EngineBufferBungeeTest — fixture that selects the real Bungee scaler.
//
// Inherits BaseSignalPathTest (real signal path, real CachingReader, real
// ReadAheadManager) but does NOT call setScalerForTest(), so the scalers
// live in their natural EngineBuffer slots.
class EngineBufferBungeeTest : public BaseSignalPathTest {
  protected:
    static const QString kAppGroup;

    void SetUp() override {
        BaseSignalPathTest::SetUp();
        // Load a fake 128-BPM stereo track on deck 1 and let it play.
        m_pTrack1 = m_pMixerDeck1->loadFakeTrack(false, 128.0);
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
        ProcessBuffer();
    }

    void selectEngine(EngineBuffer::KeylockEngine eng) {
        ControlObject::set(ConfigKey(kAppGroup, QStringLiteral("keylock_engine")),
                static_cast<double>(eng));
    }

    void setKeylock(bool on) {
        ControlObject::set(ConfigKey(m_sGroup1, "keylock"), on ? 1.0 : 0.0);
    }

    // Drive the engine for n buffers; return true iff all output was finite.
    bool processFinite(int n) {
        for (int i = 0; i < n; ++i) {
            ProcessBuffer();
            if (spanHasInvalidSamples(m_pEngineMixer->getMainBuffer())) {
                return false;
            }
        }
        return true;
    }

    TrackPointer m_pTrack1;
};

// static
const QString EngineBufferBungeeTest::kAppGroup = QStringLiteral("[App]");

// When Bungee is the selected keylock engine and keylock is
// enabled, m_pScaleKeylock must point at m_pScaleBungee and m_pScale must
// follow.
TEST_F(EngineBufferBungeeTest, BungeeEngineSelected) {
    selectEngine(EngineBuffer::KeylockEngine::Bungee);
    ProcessBuffer();

    EngineBuffer* pEB = m_pChannel1->getEngineBuffer();
    EXPECT_EQ(pEB->m_pScaleBungee, pEB->m_pScaleKeylock);

    setKeylock(true);
    ProcessBuffer();
    EXPECT_EQ(pEB->m_pScaleBungee, pEB->m_pScale);

    // Several more clean buffers while keylock is on.
    EXPECT_TRUE(processFinite(5));
}

// Rapidly toggling keylock on/off while Bungee is the active
// engine must not produce NaN/Inf output or crash.
TEST_F(EngineBufferBungeeTest, BungeeKeylockToggleDoesNotCrash) {
    selectEngine(EngineBuffer::KeylockEngine::Bungee);
    ProcessBuffer();

    for (int i = 0; i < 8; ++i) {
        setKeylock(i % 2 == 0);
        EXPECT_TRUE(processFinite(2))
                << "Invalid audio detected at toggle iteration " << i;
    }

    // Stabilize with keylock on.
    setKeylock(true);
    EXPECT_TRUE(processFinite(6));
}

// Switching the keylock engine from SoundTouch to Bungee and back
// while playing must keep m_pScaleKeylock consistent and produce clean audio.
TEST_F(EngineBufferBungeeTest, BungeeKeylockEngineSwitch) {
    // Start with SoundTouch.
    selectEngine(EngineBuffer::KeylockEngine::SoundTouch);
    setKeylock(true);
    EXPECT_TRUE(processFinite(4));

    EngineBuffer* pEB = m_pChannel1->getEngineBuffer();
    EXPECT_NE(pEB->m_pScaleBungee, pEB->m_pScaleKeylock);

    // Switch to Bungee mid-play.
    selectEngine(EngineBuffer::KeylockEngine::Bungee);
    EXPECT_TRUE(processFinite(1));
    EXPECT_EQ(pEB->m_pScaleBungee, pEB->m_pScaleKeylock);
    EXPECT_TRUE(processFinite(4));

    // Switch back to SoundTouch.
    selectEngine(EngineBuffer::KeylockEngine::SoundTouch);
    EXPECT_TRUE(processFinite(1));
    EXPECT_NE(pEB->m_pScaleBungee, pEB->m_pScaleKeylock);
    EXPECT_TRUE(processFinite(4));
}
