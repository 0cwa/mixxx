#include "engine/controls/seek30control.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

#include "moc_seek30control.cpp"
#include "track/cue.h"
#include "track/track.h"

Seek30Control::StateUpdateGuard::~StateUpdateGuard() {
    if (m_pControl) {
        m_pControl->finishStateUpdate();
    }
}

Seek30Control::StateUpdateGuard Seek30Control::tryBeginStateUpdate() {
    // Direct callbacks deliberately fail fast when a track-state publish is
    // in progress. Retrying here would require waiting or queueing the signal.
    if (m_trackStateUpdateInProgress.test_and_set(std::memory_order_acquire)) {
        return {};
    }
    return StateUpdateGuard(this);
}

Seek30Control::StateUpdateGuard Seek30Control::beginStateUpdate() {
    for (;;) {
        auto update = tryBeginStateUpdate();
        if (update) {
            return update;
        }
        // Track loading is off the real-time path. The direct control callbacks
        // use tryBeginStateUpdate() and never wait for this writer.
        std::this_thread::yield();
    }
}

void Seek30Control::finishStateUpdate() {
    m_trackStateUpdateInProgress.clear(std::memory_order_release);
}

bool Seek30Control::tryGetTrackState(TrackState* pState) const {
    VERIFY_OR_DEBUG_ASSERT(pState) {
        return false;
    }

    if (m_trackStateUpdateInProgress.test(std::memory_order_acquire)) {
        return false;
    }
    *pState = m_trackState.getValue();
    return !m_trackStateUpdateInProgress.test(std::memory_order_acquire);
}

void Seek30Control::trackLoaded(TrackPointer pNewTrack) {
    auto update = beginStateUpdate();
    TrackState state;
    state.pLoadedTrack = std::move(pNewTrack);
    state.memoryCues = rebuildMemoryCueCache(state.pLoadedTrack);
    m_trackState.setValue(state);
}

QList<CuePointer> Seek30Control::rebuildMemoryCueCache(
        const TrackPointer& pTrack) const {
    QList<CuePointer> memoryCues;
    if (!pTrack) {
        return memoryCues;
    }

    const QList<CuePointer> cues = pTrack->getCuePoints();
    for (const auto& pCue : cues) {
        if (pCue && pCue->getType() == mixxx::CueType::Memory) {
            memoryCues.append(pCue);
        }
    }
    sortCueCache(&memoryCues);
    return memoryCues;
}

void Seek30Control::sortCueCache(QList<CuePointer>* pMemoryCues) {
    DEBUG_ASSERT(pMemoryCues);
    std::sort(pMemoryCues->begin(),
            pMemoryCues->end(),
            [](const CuePointer& a, const CuePointer& b) {
                return a->getStartAndEndPosition().startPosition <
                        b->getStartAndEndPosition().startPosition;
            });
}

int Seek30Control::nextFreeMemoryCueIndex(
        const QList<CuePointer>& memoryCues) {
    // Some codebases store a “hotcue index” even for Memory cues; others leave it -1.
    // We’ll accept either. If all are -1, we’ll just use size() as the next index.
    QSet<int> used;
    for (const auto& pCue : memoryCues) {
        if (!pCue) {
            continue;
        }

        // Prefer an explicit index if your Cue has one
        int idx = -1;

        // Try common fields in order of likelihood; keep whichever your Cue API supports:
        // If your Cue has getHotCue():
        if (pCue->getHotCue() >= 0) {
            idx = pCue->getHotCue();
        }

        if (idx >= 0) {
            used.insert(idx);
        }
    }

    if (used.isEmpty()) {
        // No explicit indices recorded: just append at the end.
        return memoryCues.size();
    }

    // Return the first gap in {0,1,2,...}
    int next = 0;
    while (used.contains(next)) {
        ++next;
    }
    return next;
}

int Seek30Control::createMemoryCueAt(
        TrackState* pState, const mixxx::audio::FramePos& pos) {
    VERIFY_OR_DEBUG_ASSERT(pState) {
        return -1;
    }
    if (!pState->pLoadedTrack || !pos.isValid()) {
        return -1;
    }

    // Don't allow creating a second memory cue in the same position as another
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units
    const double curPos = pos.toEngineSamplePos();
    for (const auto& pCue : std::as_const(pState->memoryCues)) {
        if (!pCue) {
            continue;
        }
        const double testPos = pCue->getStartAndEndPosition()
                                       .startPosition.toEngineSamplePos();
        if (std::abs(curPos - testPos) < kEps) {
            return -1;
        }
    }

    const int index = nextFreeMemoryCueIndex(pState->memoryCues);

    CuePointer pCue = pState->pLoadedTrack->createAndAddCue(
            mixxx::CueType::Memory,
            Cue::kNoHotCue,
            pos,
            pos);
    if (!pCue) {
        return -1;
    }

    // Mirror into the local snapshot so future index calculations see it.
    pState->memoryCues.append(pCue);
    sortCueCache(&pState->memoryCues);

    return index;
}

void Seek30Control::clearAll(double v) {
    if (v <= 0) {
        return;
    }

    auto update = tryBeginStateUpdate();
    if (!update) {
        return;
    }
    TrackState state = m_trackState.getValue();
    if (!state.pLoadedTrack) {
        return;
    }
    state.pLoadedTrack->removeCuesOfType(mixxx::CueType::Memory);
    state.memoryCues.clear();
    m_trackState.setValue(state);
}

void Seek30Control::clearCurrent(double v) {
    if (v <= 0) {
        return;
    }

    auto update = tryBeginStateUpdate();
    if (!update) {
        return;
    }
    TrackState state = m_trackState.getValue();
    if (!state.pLoadedTrack) {
        return;
    }

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * sampleRate();
    const mixxx::audio::FramePos currentPos(currentFrames);
    const double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (const auto& pCue : std::as_const(state.memoryCues)) {
        if (!pCue) {
            continue;
        }
        const double testPos = pCue->getStartAndEndPosition()
                                       .startPosition.toEngineSamplePos();
        if (std::abs(curPos - testPos) < kEps) {
            state.pLoadedTrack->removeCue(pCue);
            state.memoryCues.removeOne(pCue);
            m_trackState.setValue(state);
            break;
        }
    }
}

void Seek30Control::clearPrev(double v) {
    if (v <= 0) {
        return;
    }

    auto update = tryBeginStateUpdate();
    if (!update) {
        return;
    }
    TrackState state = m_trackState.getValue();
    if (!state.pLoadedTrack) {
        return;
    }

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * sampleRate();
    const mixxx::audio::FramePos currentPos(currentFrames);
    const double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (int i = state.memoryCues.size() - 1; i >= 0; --i) {
        const auto pCue = state.memoryCues.at(i);
        if (!pCue) {
            continue;
        }
        const double testPos = pCue->getStartAndEndPosition()
                                       .startPosition.toEngineSamplePos();
        if ((curPos - testPos) > kEps) {
            state.pLoadedTrack->removeCue(pCue);
            state.memoryCues.removeOne(pCue);
            m_trackState.setValue(state);
            break;
        }
    }
}

void Seek30Control::clearNext(double v) {
    if (v <= 0) {
        return;
    }

    auto update = tryBeginStateUpdate();
    if (!update) {
        return;
    }
    TrackState state = m_trackState.getValue();
    if (!state.pLoadedTrack) {
        return;
    }

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * sampleRate();
    const mixxx::audio::FramePos currentPos(currentFrames);
    const double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (const auto& pCue : std::as_const(state.memoryCues)) {
        if (!pCue) {
            continue;
        }
        const double testPos = pCue->getStartAndEndPosition()
                                       .startPosition.toEngineSamplePos();
        if ((testPos - curPos) > kEps) {
            state.pLoadedTrack->removeCue(pCue);
            state.memoryCues.removeOne(pCue);
            m_trackState.setValue(state);
            break;
        }
    }
}

void Seek30Control::clearNearest(double v) {
    if (v <= 0) {
        return;
    }

    auto update = tryBeginStateUpdate();
    if (!update) {
        return;
    }
    TrackState state = m_trackState.getValue();
    if (!state.pLoadedTrack || state.memoryCues.isEmpty()) {
        return;
    }

    // Read normalized play position [0..1] and track duration [s].
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!std::isfinite(playpos) || !std::isfinite(durationSec) || durationSec <= 0.0) {
        return; // nothing to do if play position or duration is invalid
    }

    // Convert to current absolute frame position.
    const double sampleRateHz = sampleRate();
    const double currentFrames = playpos * durationSec * sampleRateHz;
    const mixxx::audio::FramePos currentPos(currentFrames);
    const double curPos = currentPos.toEngineSamplePos();

    // Find the memory cue with the smallest distance to the playhead.
    constexpr double kCueToleranceSeconds = 1.0;
    const double cueTolerance = sampleRateHz * kCueToleranceSeconds;
    CuePointer bestCue;
    double bestDiff = std::numeric_limits<double>::max();

    for (const auto& pCue : std::as_const(state.memoryCues)) {
        if (!pCue) {
            continue;
        }
        const double cuePos = pCue->getStartAndEndPosition()
                                      .startPosition.toEngineSamplePos();
        const double diff = std::abs(cuePos - curPos);
        if (diff >= cueTolerance) {
            continue;
        }

        // Prefer smaller diff; on ties, prefer the cue at or behind the playhead.
        if (diff < bestDiff - 1e-9 ||
                (std::abs(diff - bestDiff) <= 1e-9 && cuePos <= curPos)) {
            bestDiff = diff;
            bestCue = pCue;
        }
    }

    if (bestCue) {
        state.pLoadedTrack->removeCue(bestCue);
        state.memoryCues.removeOne(bestCue); // keep cache in sync
        m_trackState.setValue(state);
    }
}

void Seek30Control::createAtCurrent(double v) {
    if (v <= 0) {
        return;
    }

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * sampleRate();
    mixxx::audio::FramePos currentPos(currentFrames);

    const auto pQuantizeEnabled = ControlObject::getControl(
            ConfigKey(m_group, "quantize"));
    if (pQuantizeEnabled->toBool()) {
        const auto pClosestBeat = ControlObject::getControl(
                ConfigKey(m_group, "beat_closest"));
        const auto closestBeat =
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                        pClosestBeat->get());
        currentPos = closestBeat;
    }

    auto update = tryBeginStateUpdate();
    if (!update) {
        return;
    }
    TrackState state = m_trackState.getValue();
    if (createMemoryCueAt(&state, currentPos) >= 0) {
        m_trackState.setValue(state);
    }
}

void Seek30Control::slotSeek30(double v) {
    if (v <= 0) {
        return;
    }

    TrackState state;
    if (!tryGetTrackState(&state)) {
        return;
    }
    if (!state.pLoadedTrack) {
        return;
    }

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * sampleRate();
    const mixxx::audio::FramePos currentPos(currentFrames);
    const double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (const auto& pCue : std::as_const(state.memoryCues)) {
        if (!pCue) {
            continue;
        }
        const double testPos = pCue->getStartAndEndPosition()
                                       .startPosition.toEngineSamplePos();
        if ((testPos - curPos) > kEps) {
            getEngineBuffer()->seekAbs(pCue->getStartAndEndPosition().startPosition);
            break;
        }
    }
}

void Seek30Control::slotSeek30Prev(double v) {
    if (v <= 0) {
        return;
    }

    TrackState state;
    if (!tryGetTrackState(&state)) {
        return;
    }
    if (!state.pLoadedTrack) {
        return;
    }

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * sampleRate();
    const mixxx::audio::FramePos currentPos(currentFrames);
    const double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (int i = state.memoryCues.size() - 1; i >= 0; --i) {
        const auto pCue = state.memoryCues.at(i);
        if (!pCue) {
            continue;
        }
        const double testPos = pCue->getStartAndEndPosition()
                                       .startPosition.toEngineSamplePos();
        if ((curPos - testPos) > kEps) {
            // Allow skipping the current memory cue if it's within 1.5 seconds
            const auto playControl =
                    ControlObject::getControl(ConfigKey(m_group, "play"));
            const bool isPlaying = playControl->get() > 0.0;
            if (isPlaying && std::abs(curPos - testPos) < (sampleRate() * 1.5)) {
                continue;
            }

            getEngineBuffer()->seekAbs(pCue->getStartAndEndPosition().startPosition);
            break;
        }
    }
}
