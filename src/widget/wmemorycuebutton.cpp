#include "widget/wmemorycuebutton.h"

#include <QMouseEvent>

#include "control/controlobject.h"
#include "mixer/playerinfo.h"
#include "moc_wmemorycuebutton.cpp"
#include "preferences/colorpalettesettings.h"
#include "skin/legacy/skincontext.h"
#include "track/cue.h"
#include "track/track.h"

WMemoryCueButton::WMemoryCueButton(QWidget* pParent, const QString& group)
        : WPushButton(pParent),
          m_group(group) {
    setFocusPolicy(Qt::NoFocus);
}

void WMemoryCueButton::setup(const QDomNode& node, const SkinContext& context) {
    // Pass through to base to pick up stylesheet, size, etc.
    WPushButton::setup(node, context);

    // Create the cue menu popup and apply the (hotcue) color palette, same as WHotcueButton.
    m_pCueMenuPopup = make_parented<WCueMenuPopup>(context.getConfig(), this);
    ColorPaletteSettings colorPaletteSettings(context.getConfig());
    auto colorPalette = colorPaletteSettings.getHotcueColorPalette();
    m_pCueMenuPopup->setColorPalette(colorPalette);
    // For memory-cue context, hide Delete & Saved-Loop actions.
    m_pCueMenuPopup->setDeleteCueVisible(false);
    m_pCueMenuPopup->setSavedLoopCueVisible(false);

    // No extra connections: left-click simply opens the popup (handled in mousePressEvent)
}

void WMemoryCueButton::mousePressEvent(QMouseEvent* pEvent) {
    // Only act on *left* clicks.
    if (pEvent->button() != Qt::LeftButton) {
        WPushButton::mousePressEvent(pEvent);
        return;
    }

    // Determine the currently loaded track for this deck/group.
    TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(m_group);
    if (!pTrack) {
        return;
    }

    // Read normalized play position [0..1] and the track length in engine samples.
    // We use engine-sample space here so we don't need the sample rate explicitly.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double trackSamples = ControlObject::get(ConfigKey(m_group, "track_samples"));
    if (!(trackSamples > 0.0)) {
        return; // track not ready
    }

    // Current engine-sample position.
    const double curPos = playpos * trackSamples;

    // Find the memory cue whose start position matches the current position
    constexpr double kEps = 50000.0; // within about a second with 44khz
    CuePointer pMemoryCue;
    const QList<CuePointer> cues = pTrack->getCuePoints();
    for (const auto& pCue : cues) {
        if (!pCue) {
            continue;
        }
        if (pCue->getType() != mixxx::CueType::Memory) {
            continue;
        }
        const double cueStart = pCue->getStartAndEndPosition()
                                        .startPosition
                                        .toEngineSamplePos();
        if (std::abs(curPos - cueStart) < kEps) {
            pMemoryCue = pCue;
            break;
        }
    }

    if (!pMemoryCue) {
        // No memory cue at the current position; nothing to show.
        return;
    }

    // Bind popup to this track/cue/group and show it just below the button.
    m_pCueMenuPopup->setTrackCueGroup(pTrack, pMemoryCue, m_group);
    m_pCueMenuPopup->popup(mapToGlobal(QPoint(0, height())));
}

