#ifdef __BUNGEE__

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

#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>
#include <thread>

#include "control/controlobject.h"
#include "engine/bufferscalers/enginebufferscalebungee.h"
#include "engine/enginebuffer.h"
#include "test/mockedenginebackendtest.h"
#include "test/signalpathtest.h"

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
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        // Load a fake 128-BPM stereo track on deck 1 and let it play.
        m_pTrack1 = m_pMixerDeck1->loadFakeTrack(false, 128.0);
        ControlObject::set(ConfigKey(m_sGroup1, "play"), 1.0);
        ProcessBuffer();
    }

    void selectEngine(EngineBuffer::KeylockEngine eng) {
        ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock_engine")),
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

// When Bungee is the selected keylock engine and keylock is enabled, the
// callback must use the currently published immutable Bungee state.
TEST_F(EngineBufferBungeeTest, BungeeEngineSelected) {
    selectEngine(EngineBuffer::KeylockEngine::Bungee);
    ProcessBuffer();

    EngineBuffer* pEB = m_pChannel1->getEngineBuffer();

    setKeylock(true);
    ProcessBuffer();
    auto* pPublishedState = pEB->m_pBungeePublishedState.load(
            std::memory_order_seq_cst);
    ASSERT_NE(nullptr, pPublishedState);
    EXPECT_EQ(static_cast<EngineBufferScale*>(pPublishedState->pScaler),
            pEB->m_pScale);

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

// Switching the keylock engine from SoundTouch to Bungee and back while
// playing must use a coherent engine/state publication and produce clean audio.
TEST_F(EngineBufferBungeeTest, BungeeKeylockEngineSwitch) {
    // Start with SoundTouch.
    selectEngine(EngineBuffer::KeylockEngine::SoundTouch);
    setKeylock(true);
    EXPECT_TRUE(processFinite(4));

    EngineBuffer* pEB = m_pChannel1->getEngineBuffer();

    // Switch to Bungee mid-play.
    selectEngine(EngineBuffer::KeylockEngine::Bungee);
    EXPECT_TRUE(processFinite(1));
    auto* pPublishedState = pEB->m_pBungeePublishedState.load(
            std::memory_order_seq_cst);
    ASSERT_NE(nullptr, pPublishedState);
    EXPECT_EQ(static_cast<EngineBufferScale*>(pPublishedState->pScaler),
            pEB->m_pScale);
    EXPECT_TRUE(processFinite(4));

    // Switch back to SoundTouch.
    selectEngine(EngineBuffer::KeylockEngine::SoundTouch);
    EXPECT_TRUE(processFinite(1));
    EXPECT_NE(static_cast<EngineBufferScale*>(pPublishedState->pScaler),
            pEB->m_pScale);
    EXPECT_TRUE(processFinite(4));
}

// Repeatedly publish new sample-rate/channel-compatible Bungee states while
// changing the selected keylock engine. This drives the worker's retired-state
// acknowledgement path deterministically through callback boundaries.
TEST_F(EngineBufferBungeeTest, BungeeRapidReconfigurationAndEngineChanges) {
    setKeylock(true);

    const ConfigKey sampleRateKey(QStringLiteral("[App]"),
            QStringLiteral("samplerate"));
    EngineBuffer* pEB = m_pChannel1->getEngineBuffer();
    const auto* pInitialState = pEB->m_pBungeePublishedState.load(
            std::memory_order_seq_cst);
    ASSERT_NE(nullptr, pInitialState);
    const auto initialStateAddress =
            reinterpret_cast<std::uintptr_t>(pInitialState);

    for (int i = 0; i < 24; ++i) {
        ControlObject::set(sampleRateKey, i % 2 == 0 ? 44100.0 : 48000.0);
        selectEngine(i % 3 == 0
                        ? EngineBuffer::KeylockEngine::Bungee
                        : EngineBuffer::KeylockEngine::SoundTouch);
        EXPECT_TRUE(processFinite(2))
                << "Invalid audio during reconfiguration " << i;
    }

    // Keep the control object in the final state so the test teardown does
    // not enqueue an additional unobserved engine transition.
    selectEngine(EngineBuffer::KeylockEngine::Bungee);
    EXPECT_TRUE(processFinite(4));

    // Preparation runs asynchronously after callback boundaries. Wait on the
    // test thread for each publication, then process one callback to prove the
    // callback selected that same immutable state. The wait is deliberately
    // outside EngineBuffer::process; the audio callback never waits.
    const auto waitForPublishedState = [&](int expectedSampleRate) {
        for (int i = 0; i < 500; ++i) {
            const auto* pPublishedState = pEB->m_pBungeePublishedState.load(
                    std::memory_order_seq_cst);
            if (pPublishedState &&
                    pPublishedState->sampleRate == expectedSampleRate &&
                    pPublishedState->channelCount == 2) {
                ProcessBuffer();
                const auto* pSelectedState =
                        pEB->m_pBungeePublishedState.load(
                                std::memory_order_seq_cst);
                if (pSelectedState == pPublishedState &&
                        pEB->m_pScale == pPublishedState->pScaler) {
                    return true;
                }
            } else {
                ProcessBuffer();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };

    ASSERT_TRUE(waitForPublishedState(48000));
    const auto* pPublished48000 = pEB->m_pBungeePublishedState.load(
            std::memory_order_seq_cst);
    ASSERT_NE(nullptr, pPublished48000);
    const auto published48000Address =
            reinterpret_cast<std::uintptr_t>(pPublished48000);
    EXPECT_NE(initialStateAddress, published48000Address);

    // Publish another configuration after the callback has acknowledged the
    // first replacement. This exercises reclaim and reuse of the retired slot.
    ControlObject::set(sampleRateKey, 44100.0);
    selectEngine(EngineBuffer::KeylockEngine::Bungee);
    ASSERT_TRUE(waitForPublishedState(44100));
    const auto* pPublished44100 = pEB->m_pBungeePublishedState.load(
            std::memory_order_seq_cst);
    ASSERT_NE(nullptr, pPublished44100);
    const auto published44100Address =
            reinterpret_cast<std::uintptr_t>(pPublished44100);
    EXPECT_NE(published48000Address, published44100Address);
}

#endif // __BUNGEE__
