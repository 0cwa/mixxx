#include "engine/controls/seek30control.h"

#include <gtest/gtest.h>

#include <QThread>
#include <algorithm>
#include <memory>
#include <utility>

#include "control/controlobject.h"
#include "test/mixxxtest.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

const QString kGroup = QStringLiteral("[Seek30ControlTest]");

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
        m_pPlayPosition->set(0.5);
        m_pSeek30Control = std::make_unique<Seek30Control>(kGroup, config());
    }

    std::unique_ptr<ControlObject> m_pQuantize;
    std::unique_ptr<ControlObject> m_pClosestBeat;
    std::unique_ptr<ControlObject> m_pDuration;
    std::unique_ptr<ControlObject> m_pPlayPosition;
    std::unique_ptr<Seek30Control> m_pSeek30Control;
};

class TrackLoadThread final : public QThread {
  public:
    TrackLoadThread(Seek30Control* pSeek30Control, TrackPointer pTrack)
            : m_pSeek30Control(pSeek30Control),
              m_pTrack(std::move(pTrack)) {
    }

  protected:
    void run() override {
        m_pSeek30Control->setFrameInfo(
                mixxx::audio::kStartFramePos,
                mixxx::audio::FramePos::fromEngineSamplePos(441000),
                mixxx::audio::SampleRate(44100));
        m_pSeek30Control->trackLoaded(std::move(m_pTrack));
    }

  private:
    Seek30Control* m_pSeek30Control;
    TrackPointer m_pTrack;
};

} // namespace

TEST_F(Seek30ControlTest, WorkerTrackLoadAndDirectCueMutation) {
    const TrackPointer pTrack = Track::newTemporary();
    pTrack->setAudioProperties(
            mixxx::audio::ChannelCount(2),
            mixxx::audio::SampleRate(44100),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(10));

    TrackLoadThread loadThread(m_pSeek30Control.get(), pTrack);
    loadThread.start();
    ASSERT_TRUE(loadThread.wait(2000));

    const ConfigKey createKey(kGroup, "memory_create_at_current");
    ControlObject::set(createKey, 0.0);
    ControlObject::set(createKey, 1.0);

    const auto cuePoints = pTrack->getCuePoints();
    const auto memoryCue = std::find_if(
            cuePoints.cbegin(), cuePoints.cend(), [](const CuePointer& pCue) {
                return pCue && pCue->getType() == mixxx::CueType::Memory;
            });
    ASSERT_NE(memoryCue, cuePoints.cend());
    EXPECT_EQ((*memoryCue)->getStartAndEndPosition().startPosition,
            mixxx::audio::FramePos(220500));

    ControlObject::set(ConfigKey(kGroup, "memory_clear_current"), 1.0);

    const auto remainingCuePoints = pTrack->getCuePoints();
    EXPECT_FALSE(std::any_of(
            remainingCuePoints.cbegin(),
            remainingCuePoints.cend(),
            [](const CuePointer& pCue) {
                return pCue && pCue->getType() == mixxx::CueType::Memory;
            }));
}
