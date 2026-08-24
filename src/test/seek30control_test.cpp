#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <functional>
#include <memory>

#include "control/controlobject.h"
#include "engine/controls/seek30control.h"
#include "test/mixxxtest.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

const QString kGroup = QStringLiteral("[Seek30ControlTest]");

class FunctionThread final : public QThread {
  public:
    explicit FunctionThread(std::function<void()> function)
            : m_function(std::move(function)) {
    }

  protected:
    void run() override {
        m_function();
    }

  private:
    std::function<void()> m_function;
};

class Seek30ControlTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pQuantize = std::make_unique<ControlObject>(ConfigKey(kGroup, "quantize"));
        m_pClosestBeat = std::make_unique<ControlObject>(ConfigKey(kGroup, "beat_closest"));
        m_pDuration = std::make_unique<ControlObject>(ConfigKey(kGroup, "duration"));
        m_pPlayPosition = std::make_unique<ControlObject>(ConfigKey(kGroup, "playposition"));
        m_pQuantize->set(0.0);
        m_pClosestBeat->set(0.0);
        m_pDuration->set(10.0);
        m_pPlayPosition->set(0.0);
        m_pSeek30Control = std::make_unique<Seek30Control>(kGroup, config());
    }

    std::unique_ptr<ControlObject> m_pQuantize;
    std::unique_ptr<ControlObject> m_pClosestBeat;
    std::unique_ptr<ControlObject> m_pDuration;
    std::unique_ptr<ControlObject> m_pPlayPosition;
    std::unique_ptr<Seek30Control> m_pSeek30Control;
};

void processQueuedEvents() {
    // Processing twice also handles signals emitted while processing the first
    // batch of queued events.
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

} // namespace

TEST_F(Seek30ControlTest, CrossThreadTrackAndCueCommandsUseOwnerThread) {
    const TrackPointer pTrack = Track::newTemporary();
    pTrack->setAudioProperties(
            mixxx::audio::ChannelCount(2),
            mixxx::audio::SampleRate(44100),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(10));

    FunctionThread loadThread([this, pTrack] {
        m_pSeek30Control->trackLoaded(pTrack);
    });
    loadThread.start();
    ASSERT_TRUE(loadThread.wait(2000));
    processQueuedEvents();

    FunctionThread commandThread([] {
        ControlObject::set(ConfigKey(kGroup, "memory_create_at_current"), 1.0);
    });
    commandThread.start();
    ASSERT_TRUE(commandThread.wait(2000));
    processQueuedEvents();

    const auto cuePoints = pTrack->getCuePoints();
    const auto memoryCue = std::find_if(
            cuePoints.cbegin(), cuePoints.cend(), [](const CuePointer& pCue) {
                return pCue && pCue->getType() == mixxx::CueType::Memory;
            });
    EXPECT_NE(memoryCue, cuePoints.cend());
}
