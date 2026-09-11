#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "util/valuetransformer.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/controlwidgetconnection.h"
#include "widget/woverview.h"

namespace {

void sendMousePress(QWidget* widget, const QPointF& position) {
    QMouseEvent event(
            QEvent::MouseButtonPress,
            position,
            position,
            position,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void sendMouseRelease(QWidget* widget, const QPointF& position) {
    QMouseEvent event(
            QEvent::MouseButtonRelease,
            position,
            position,
            position,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void sendWindowDeactivate(QWidget* widget) {
    QEvent event(QEvent::WindowDeactivate);
    QApplication::sendEvent(widget, &event);
}

} // namespace

class WidgetMouseCancellationTest : public MixxxTest {
  public:
    WidgetMouseCancellationTest() {
        if (!WaveformWidgetFactory::isCreated()) {
            WaveformWidgetFactory::createInstance();
            m_createdFactory = true;
        }
    }

    ~WidgetMouseCancellationTest() override {
        if (m_createdFactory) {
            WaveformWidgetFactory::destroy();
        }
    }

  private:
    ControlPushButton touchShift = ConfigKey(
            QStringLiteral("[Controls]"), QStringLiteral("touch_shift"));
    ControlObject trackSamples = ConfigKey(
            QStringLiteral("[Channel1]"), QStringLiteral("track_samples"));
    ControlObject trackSampleRate = ConfigKey(
            QStringLiteral("[Channel1]"), QStringLiteral("track_samplerate"));
    ControlObject playposition = ConfigKey(
            QStringLiteral("[Channel1]"), QStringLiteral("playposition"));
    ControlObject channelReplayGain = ConfigKey(
            QStringLiteral("[Channel1]"), QStringLiteral("replaygain"));
    ControlObject waveformOverviewType = ConfigKey(
            QStringLiteral("[Waveform]"), QStringLiteral("WaveformOverviewType"));
    ControlObject overviewStereoMode = ConfigKey(
            QStringLiteral("[Waveform]"), QStringLiteral("overview_stereo_mode"));
    ControlObject drawOverviewMinuteMarkers = ConfigKey(
            QStringLiteral("[Waveform]"), QStringLiteral("draw_overview_minute_markers"));
    ControlObject replayGain = ConfigKey(
            QStringLiteral("[ReplayGain]"), QStringLiteral("ReplayGain"));
    ControlObject replayGainEnabled = ConfigKey(
            QStringLiteral("[ReplayGain]"), QStringLiteral("ReplayGainEnabled"));
    ControlObject replayGainBoost = ConfigKey(
            QStringLiteral("[ReplayGain]"), QStringLiteral("ReplayGainBoost"));
    ControlObject defaultBoost = ConfigKey(
            QStringLiteral("[ReplayGain]"), QStringLiteral("DefaultBoost"));
    bool m_createdFactory = false;
};

TEST_F(WidgetMouseCancellationTest, OverviewDoesNotCommitReleaseAfterWindowDeactivation) {
    WOverview overview(
            QStringLiteral("[Channel1]"), nullptr, m_pConfig, nullptr);
    overview.resize(100, 50);
    overview.addConnection(
            std::make_unique<ControlParameterWidgetConnection>(
                    &overview,
                    ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("playposition")),
                    nullptr,
                    ControlParameterWidgetConnection::DIR_FROM_WIDGET,
                    ControlParameterWidgetConnection::EMIT_ON_RELEASE),
            WBaseWidget::ConnectionSide::None);
    overview.initWithTrack(Track::newTemporary());

    ControlProxy trackSamples(
            QStringLiteral("[Channel1]"), QStringLiteral("track_samples"));
    trackSamples.set(1000.0);
    ControlProxy playposition(
            QStringLiteral("[Channel1]"), QStringLiteral("playposition"));
    playposition.set(0.0);

    sendMousePress(&overview, QPointF(20, 25));
    sendWindowDeactivate(&overview);
    sendMouseRelease(&overview, QPointF(80, 25));

    EXPECT_DOUBLE_EQ(0.0, playposition.get());
}
