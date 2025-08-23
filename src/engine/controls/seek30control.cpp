#include <cmath>
#include "engine/controls/seek30control.h"
#include "track/track.h"
#include "track/cue.h"

#include "moc_seek30control.cpp"

void Seek30Control::trackLoaded(TrackPointer pNewTrack) {
    m_pLoadedTrack = pNewTrack;
    rebuildMemoryCueCache();
}

void Seek30Control::rebuildMemoryCueCache() {
    m_memoryCues.clear();
    if (!m_pLoadedTrack) {
        return;
    }
    const QList<CuePointer> cues = m_pLoadedTrack->getCuePoints();
    for (const auto& pCue : cues) {
        if (!pCue) continue;
        if (pCue->getType() == mixxx::CueType::Memory) {
            m_memoryCues.append(pCue);
        }
    }
    sortCueCache();
}

void Seek30Control::sortCueCache() {
    // Optional: keep a stable order (by start position)
    std::sort(m_memoryCues.begin(), m_memoryCues.end(),
        [](const CuePointer& a, const CuePointer& b) {
            return a->getStartAndEndPosition().startPosition < b->getStartAndEndPosition().startPosition;
        });
}

int Seek30Control::nextFreeMemoryCueIndex() const {
    // Some codebases store a “hotcue index” even for Memory cues; others leave it -1.
    // We’ll accept either. If all are -1, we’ll just use size() as the next index.
    QSet<int> used;
    for (const auto& pCue : m_memoryCues) {
        if (!pCue) continue;

        // Prefer an explicit index if your Cue has one
        int idx = -1;

        // Try common fields in order of likelihood; keep whichever your Cue API supports:
        // If your Cue has getHotCue():
        if (pCue->getHotCue() >= 0) {
            idx = pCue->getHotCue();
        }

        if (idx >= 0) used.insert(idx);
    }

    if (used.isEmpty()) {
        // No explicit indices recorded: just append at the end.
        return m_memoryCues.size();
    }

    // Return the first gap in {0,1,2,...}
    int next = 0;
    while (used.contains(next)) {
        ++next;
    }
    return next;
}

int Seek30Control::createMemoryCueAt(const mixxx::audio::FramePos& pos) {
    if (!m_pLoadedTrack || !pos.isValid()) {
        return -1;
    }

    // Don't allow creating a second memory cue in the same position as another
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units
    double curPos = pos.toEngineSamplePos();
    for (const auto& pCue : m_memoryCues) {
        if (!pCue) continue;
        double testPos = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
        if (std::abs(curPos - testPos) < kEps) {
            return -1;
        }
    }

    const int index = nextFreeMemoryCueIndex();

    CuePointer pCue = m_pLoadedTrack->createAndAddCue(
            mixxx::CueType::Memory,
            Cue::kNoHotCue,
            pos,
            pos);

    // Mirror into our in-class cache so future index calculations see it.
    m_memoryCues.append(pCue);
    sortCueCache();

    return index;
}

void Seek30Control::clearAll(double v) {
    if (v <= 0 || !m_pLoadedTrack) return;
    m_pLoadedTrack->removeCuesOfType(mixxx::CueType::Memory);
    m_memoryCues.clear();
}

void Seek30Control::clearCurrent(double v) {
    if (v <= 0 || !m_pLoadedTrack) return;

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * m_sampleRate.toDouble();
    mixxx::audio::FramePos currentPos(currentFrames);
    double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (const auto& pCue : m_memoryCues) {
        if (!pCue) continue;
        double testPos = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
        if (std::abs(curPos - testPos) < kEps) {
            m_pLoadedTrack->removeCue(pCue);
            m_memoryCues.removeOne(pCue);
            break;
        }
    }
}

void Seek30Control::clearPrev(double v) {
    if (v <= 0 || !m_pLoadedTrack) return;

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * m_sampleRate.toDouble();
    mixxx::audio::FramePos currentPos(currentFrames);
    double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (int i = m_memoryCues.size() - 1; i >= 0; --i) {
        auto pCue = m_memoryCues.at(i);
        if (!pCue) continue;
        double testPos = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
        if ((curPos - testPos) > kEps) {
            m_pLoadedTrack->removeCue(pCue);
            m_memoryCues.removeOne(pCue);
            break;
        }
    }
}

void Seek30Control::clearNext(double v) {
    if (v <= 0 || !m_pLoadedTrack) return;

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * m_sampleRate.toDouble();
    mixxx::audio::FramePos currentPos(currentFrames);
    double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (const auto& pCue : m_memoryCues) {
        if (!pCue) continue;
        double testPos = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
        if ((testPos - curPos) > kEps) {
            m_pLoadedTrack->removeCue(pCue);
            m_memoryCues.removeOne(pCue);
            break;
        }
    }
}

void Seek30Control::createAtCurrent(double v) {
    if (v <= 0) return;

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * m_sampleRate.toDouble();
    mixxx::audio::FramePos currentPos(currentFrames);

    createMemoryCueAt(currentPos);

}

void Seek30Control::slotSeek30(double v) {
    if (v <= 0 || !m_pLoadedTrack) return;

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * m_sampleRate.toDouble();
    mixxx::audio::FramePos currentPos(currentFrames);
    double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (const auto& pCue : m_memoryCues) {
        if (!pCue) continue;
        double testPos = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
        if ((testPos - curPos) > kEps) {
            getEngineBuffer()->seekAbs(pCue->getStartAndEndPosition().startPosition);
            break;
        }
    }
}

void Seek30Control::slotSeek30Prev(double v) {
    if (v <= 0 || !m_pLoadedTrack) return;

    // Read normalized play position [0..1] and track duration [s] from COs.
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!(durationSec > 0.0)) {
        return; // nothing loaded or unknown duration
    }

    // Convert to current absolute frame position.
    // frames = seconds * sampleRate; (frames are per-frame, independent of channels)
    const double currentFrames = playpos * durationSec * m_sampleRate.toDouble();
    mixxx::audio::FramePos currentPos(currentFrames);
    double curPos = currentPos.toEngineSamplePos();

    // Avoid floating point errors
    constexpr double kEps = 0.5; // half a sample in "engine sample pos" units

    for (int i = m_memoryCues.size() - 1; i >= 0; --i) {
        auto pCue = m_memoryCues.at(i);
        if (!pCue) continue;
        double testPos = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
        if ((curPos - testPos) > kEps) {
            // Allow skipping the current memory cue if it's within 1.5 seconds
            if(std::abs(curPos - testPos) < (m_sampleRate.toDouble() * 1.5)) continue;

            getEngineBuffer()->seekAbs(pCue->getStartAndEndPosition().startPosition);
            break;
        }
    }
}
