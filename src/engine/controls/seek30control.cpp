#include "engine/controls/seek30control.h"

#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

#include "moc_seek30control.cpp"
#include "track/track.h"

Seek30Control::Seek30Control(const QString& group, UserSettingsPointer pConfig)
        : EngineControl(group, pConfig),
          m_seekRequest(SeekRequest{}),
          m_createRequest(CreateRequest{}) {
    createControls();
}

void Seek30Control::createControls() {
    m_pMemoryCue = std::make_unique<ControlObject>(ConfigKey(m_group, "memory_cue"));
    m_pMemoryCue->set(0.f);

    m_pSeek30 = std::make_unique<ControlPushButton>(ConfigKey(m_group, "seek_30s"));
    m_pSeek30->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pSeek30.get(),
            &ControlObject::valueChanged,
            this,
            &Seek30Control::slotSeek30,
            Qt::DirectConnection);

    m_pSeek30Prev = std::make_unique<ControlPushButton>(ConfigKey(m_group, "seek_30Prev"));
    m_pSeek30Prev->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pSeek30Prev.get(),
            &ControlObject::valueChanged,
            this,
            &Seek30Control::slotSeek30Prev,
            Qt::DirectConnection);

    m_pMemoryCreateAtCurrent = std::make_unique<ControlPushButton>(
            ConfigKey(m_group, "memory_create_at_current"));
    m_pMemoryCreateAtCurrent->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pMemoryCreateAtCurrent.get(),
            &ControlObject::valueChanged,
            this,
            &Seek30Control::createAtCurrent,
            Qt::DirectConnection);

    m_pMemoryClearAll = std::make_unique<ControlPushButton>(
            ConfigKey(m_group, "memory_clear_all"));
    m_pMemoryClearAll->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pMemoryClearAll.get(),
            &ControlObject::valueChanged,
            this,
            &Seek30Control::clearAll,
            Qt::DirectConnection);

    m_pMemoryClearCurrent = std::make_unique<ControlPushButton>(
            ConfigKey(m_group, "memory_clear_current"));
    m_pMemoryClearCurrent->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pMemoryClearCurrent.get(),
            &ControlObject::valueChanged,
            this,
            &Seek30Control::clearCurrent,
            Qt::DirectConnection);

    m_pMemoryClearPrev = std::make_unique<ControlPushButton>(
            ConfigKey(m_group, "memory_clear_prev"));
    m_pMemoryClearPrev->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pMemoryClearPrev.get(),
            &ControlObject::valueChanged,
            this,
            &Seek30Control::clearPrev,
            Qt::DirectConnection);

    m_pMemoryClearNext = std::make_unique<ControlPushButton>(
            ConfigKey(m_group, "memory_clear_next"));
    m_pMemoryClearNext->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pMemoryClearNext.get(),
            &ControlObject::valueChanged,
            this,
            &Seek30Control::clearNext,
            Qt::DirectConnection);

    m_pMemoryClearNearest = std::make_unique<ControlPushButton>(
            ConfigKey(m_group, "memory_clear_nearest"));
    m_pMemoryClearNearest->setButtonMode(mixxx::control::ButtonMode::Trigger);
    connect(m_pMemoryClearNearest.get(),
            &ControlObject::valueChanged,
            this,
            &Seek30Control::clearNearest,
            Qt::DirectConnection);
}

void Seek30Control::trackLoaded(TrackPointer pNewTrack) {
    // EngineBuffer has already installed pNewTrack as m_pCurrentTrack before
    // this callback and retains it until the loading gate is closed. Do not
    // retain another TrackPointer here: this is only a borrowed publication.
    m_pPublishedTrack.store(pNewTrack.get(), std::memory_order_release);
    m_trackGeneration.fetch_add(1, std::memory_order_release);
}

void Seek30Control::process(const double,
        mixxx::audio::FramePos,
        const std::size_t) {
    const std::uint64_t generation = m_trackGeneration.load(std::memory_order_acquire);
    if (generation != m_processedTrackGeneration) {
        // EngineBuffer's m_pCurrentTrack remains the lifetime authority while
        // its loading gate is clear and process() holds the pause lock.
        m_pLoadedTrack = m_pPublishedTrack.load(std::memory_order_acquire);
        rebuildMemoryCueCache();
        m_processedTrackGeneration = generation;
    }

    const std::uint32_t pendingClearOperations =
            m_pendingClearOperations.exchange(0, std::memory_order_acq_rel);
    if ((pendingClearOperations & static_cast<std::uint32_t>(ClearOperation::All)) != 0) {
        const auto request = m_clearRequests[clearOperationIndex(ClearOperation::All)].getValue();
        if (request.generation == generation && m_pLoadedTrack) {
            processClearAll();
        }
    }
    if ((pendingClearOperations & static_cast<std::uint32_t>(ClearOperation::Previous)) != 0) {
        const auto request =
                m_clearRequests[clearOperationIndex(ClearOperation::Previous)].getValue();
        if (request.generation == generation && m_pLoadedTrack) {
            processClearPrev();
        }
    }
    if ((pendingClearOperations & static_cast<std::uint32_t>(ClearOperation::Next)) != 0) {
        const auto request = m_clearRequests[clearOperationIndex(ClearOperation::Next)].getValue();
        if (request.generation == generation && m_pLoadedTrack) {
            processClearNext();
        }
    }
    if ((pendingClearOperations & static_cast<std::uint32_t>(ClearOperation::Current)) != 0) {
        const auto request =
                m_clearRequests[clearOperationIndex(ClearOperation::Current)].getValue();
        if (request.generation == generation && m_pLoadedTrack) {
            processClearCurrent();
        }
    }
    if ((pendingClearOperations & static_cast<std::uint32_t>(ClearOperation::Nearest)) != 0) {
        const auto request =
                m_clearRequests[clearOperationIndex(ClearOperation::Nearest)].getValue();
        if (request.generation == generation && m_pLoadedTrack) {
            processClearNearest();
        }
    }

    const auto createRequest = m_createRequest.getValue();
    if (createRequest.sequence != m_lastCreateRequestSequence) {
        m_lastCreateRequestSequence = createRequest.sequence;
        if (createRequest.generation == generation && m_pLoadedTrack) {
            processCreateAtCurrent();
        }
    }

    const auto seekRequest = m_seekRequest.getValue();
    if (seekRequest.sequence != m_lastSeekRequestSequence) {
        m_lastSeekRequestSequence = seekRequest.sequence;
        if (seekRequest.generation == generation && m_pLoadedTrack) {
            if (seekRequest.direction == SeekDirection::Next) {
                processSeek30();
            } else {
                processSeek30Prev();
            }
        }
    }
}

void Seek30Control::publishSeek(SeekDirection direction) {
    const std::uint64_t sequence =
            m_nextRequestSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    m_seekRequest.setValue(SeekRequest{
            m_trackGeneration.load(std::memory_order_acquire), sequence, direction});
}

void Seek30Control::publishCreate() {
    const std::uint64_t sequence =
            m_nextRequestSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    m_createRequest.setValue(CreateRequest{
            m_trackGeneration.load(std::memory_order_acquire), sequence});
}

void Seek30Control::publishClear(ClearOperation operation) {
    const std::size_t index = clearOperationIndex(operation);
    m_clearRequests[index].setValue(ClearRequest{
            m_trackGeneration.load(std::memory_order_acquire)});
    m_pendingClearOperations.fetch_or(
            static_cast<std::uint32_t>(operation), std::memory_order_release);
}

std::size_t Seek30Control::clearOperationIndex(ClearOperation operation) {
    switch (operation) {
    case ClearOperation::All:
        return 0;
    case ClearOperation::Previous:
        return 1;
    case ClearOperation::Next:
        return 2;
    case ClearOperation::Current:
        return 3;
    case ClearOperation::Nearest:
        return 4;
    }
    DEBUG_ASSERT(!"Unhandled clear operation");
    return 0;
}

void Seek30Control::createAtCurrent(double v) {
    if (v > 0) {
        publishCreate();
    }
}

void Seek30Control::clearAll(double v) {
    if (v > 0) {
        publishClear(ClearOperation::All);
    }
}

void Seek30Control::clearPrev(double v) {
    if (v > 0) {
        publishClear(ClearOperation::Previous);
    }
}

void Seek30Control::clearNext(double v) {
    if (v > 0) {
        publishClear(ClearOperation::Next);
    }
}

void Seek30Control::clearCurrent(double v) {
    if (v > 0) {
        publishClear(ClearOperation::Current);
    }
}

void Seek30Control::clearNearest(double v) {
    if (v > 0) {
        publishClear(ClearOperation::Nearest);
    }
}

void Seek30Control::slotSeek30(double v) {
    if (v > 0) {
        publishSeek(SeekDirection::Next);
    }
}

void Seek30Control::slotSeek30Prev(double v) {
    if (v > 0) {
        publishSeek(SeekDirection::Previous);
    }
}

void Seek30Control::rebuildMemoryCueCache() {
    m_memoryCues.clear();
    if (!m_pLoadedTrack) {
        return;
    }
    const QList<CuePointer> cues = m_pLoadedTrack->getCuePoints();
    for (const auto& pCue : cues) {
        if (pCue && pCue->getType() == mixxx::CueType::Memory) {
            m_memoryCues.append(pCue);
        }
    }
    sortCueCache();
}

void Seek30Control::sortCueCache() {
    std::sort(m_memoryCues.begin(),
            m_memoryCues.end(),
            [](const CuePointer& a, const CuePointer& b) {
                return a->getStartAndEndPosition().startPosition <
                        b->getStartAndEndPosition().startPosition;
            });
}

int Seek30Control::nextFreeMemoryCueIndex() const {
    QSet<int> used;
    for (const auto& pCue : m_memoryCues) {
        if (pCue && pCue->getHotCue() >= 0) {
            used.insert(pCue->getHotCue());
        }
    }

    if (used.isEmpty()) {
        return m_memoryCues.size();
    }

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

    constexpr double kEps = 0.5;
    const double curPos = pos.toEngineSamplePos();
    for (const auto& pCue : m_memoryCues) {
        if (pCue) {
            const double testPos =
                    pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
            if (std::abs(curPos - testPos) < kEps) {
                return -1;
            }
        }
    }

    const int index = nextFreeMemoryCueIndex();
    CuePointer pCue = m_pLoadedTrack->createAndAddCue(
            mixxx::CueType::Memory,
            Cue::kNoHotCue,
            pos,
            pos);
    m_memoryCues.append(pCue);
    sortCueCache();
    return index;
}

bool Seek30Control::currentFramePosition(mixxx::audio::FramePos* pPosition) const {
    const auto info = frameInfo();
    const double sampleRate = info.sampleRate.toDouble();
    const double playpos = ControlObject::get(ConfigKey(m_group, "playposition"));
    const double durationSec = ControlObject::get(ConfigKey(m_group, "duration"));
    if (!info.sampleRate.isValid() || !std::isfinite(playpos) ||
            !std::isfinite(durationSec) || durationSec <= 0.0) {
        return false;
    }

    *pPosition = mixxx::audio::FramePos(playpos * durationSec * sampleRate);
    return pPosition->isValid();
}

void Seek30Control::processClearAll() {
    m_pLoadedTrack->removeCuesOfType(mixxx::CueType::Memory);
    m_memoryCues.clear();
}

void Seek30Control::processClearCurrent() {
    mixxx::audio::FramePos currentPos;
    if (!currentFramePosition(&currentPos)) {
        return;
    }

    constexpr double kEps = 0.5;
    const double curPos = currentPos.toEngineSamplePos();
    for (const auto& pCue : m_memoryCues) {
        if (pCue) {
            const double testPos =
                    pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
            if (std::abs(curPos - testPos) < kEps) {
                m_pLoadedTrack->removeCue(pCue);
                m_memoryCues.removeOne(pCue);
                break;
            }
        }
    }
}

void Seek30Control::processClearPrev() {
    mixxx::audio::FramePos currentPos;
    if (!currentFramePosition(&currentPos)) {
        return;
    }

    constexpr double kEps = 0.5;
    const double curPos = currentPos.toEngineSamplePos();
    for (int i = m_memoryCues.size() - 1; i >= 0; --i) {
        const auto pCue = m_memoryCues.at(i);
        if (pCue) {
            const double testPos =
                    pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
            if ((curPos - testPos) > kEps) {
                m_pLoadedTrack->removeCue(pCue);
                m_memoryCues.removeOne(pCue);
                break;
            }
        }
    }
}

void Seek30Control::processClearNext() {
    mixxx::audio::FramePos currentPos;
    if (!currentFramePosition(&currentPos)) {
        return;
    }

    constexpr double kEps = 0.5;
    const double curPos = currentPos.toEngineSamplePos();
    for (const auto& pCue : m_memoryCues) {
        if (pCue) {
            const double testPos =
                    pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
            if ((testPos - curPos) > kEps) {
                m_pLoadedTrack->removeCue(pCue);
                m_memoryCues.removeOne(pCue);
                break;
            }
        }
    }
}

void Seek30Control::processClearNearest() {
    mixxx::audio::FramePos currentPos;
    if (!currentFramePosition(&currentPos) || m_memoryCues.isEmpty()) {
        return;
    }

    const double curPos = currentPos.toEngineSamplePos();
    constexpr double kCueToleranceSeconds = 1.0;
    const double cueTolerance =
            frameInfo().sampleRate.toDouble() * kCueToleranceSeconds;
    CuePointer bestCue;
    double bestDiff = std::numeric_limits<double>::max() / 2.0;

    for (const auto& pCue : m_memoryCues) {
        if (!pCue) {
            continue;
        }
        const double cuePos = pCue->getStartAndEndPosition().startPosition.toEngineSamplePos();
        const double diff = std::abs(cuePos - curPos);
        if (diff >= cueTolerance) {
            continue;
        }
        if (diff < bestDiff - 1e-9 ||
                (std::abs(diff - bestDiff) <= 1e-9 && cuePos <= curPos)) {
            bestDiff = diff;
            bestCue = pCue;
        }
    }

    if (bestCue) {
        m_pLoadedTrack->removeCue(bestCue);
        m_memoryCues.removeOne(bestCue);
    }
}

void Seek30Control::processCreateAtCurrent() {
    mixxx::audio::FramePos currentPos;
    if (!currentFramePosition(&currentPos)) {
        return;
    }

    const auto pQuantizeEnabled = ControlObject::getControl(
            ConfigKey(m_group, "quantize"));
    if (pQuantizeEnabled->toBool()) {
        const auto pClosestBeat = ControlObject::getControl(
                ConfigKey(m_group, "beat_closest"));
        currentPos = mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                pClosestBeat->get());
    }

    createMemoryCueAt(currentPos);
}

void Seek30Control::processSeek30() {
    mixxx::audio::FramePos currentPos;
    if (!currentFramePosition(&currentPos)) {
        return;
    }

    constexpr double kEps = 0.5;
    const double curPos = currentPos.toEngineSamplePos();
    for (const auto& pCue : m_memoryCues) {
        if (pCue) {
            const auto cuePosition = pCue->getStartAndEndPosition().startPosition;
            const double testPos = cuePosition.toEngineSamplePos();
            if ((testPos - curPos) > kEps) {
                getEngineBuffer()->seekAbs(cuePosition);
                break;
            }
        }
    }
}

void Seek30Control::processSeek30Prev() {
    mixxx::audio::FramePos currentPos;
    if (!currentFramePosition(&currentPos)) {
        return;
    }

    constexpr double kEps = 0.5;
    const double curPos = currentPos.toEngineSamplePos();
    const double sampleRate = frameInfo().sampleRate.toDouble();
    for (int i = m_memoryCues.size() - 1; i >= 0; --i) {
        const auto pCue = m_memoryCues.at(i);
        if (pCue) {
            const auto cuePosition = pCue->getStartAndEndPosition().startPosition;
            const double testPos = cuePosition.toEngineSamplePos();
            if ((curPos - testPos) > kEps) {
                const auto pPlayControl = ControlObject::getControl(
                        ConfigKey(m_group, "play"));
                const bool isPlaying = pPlayControl->get() > 0.0;
                if (isPlaying && std::abs(curPos - testPos) < sampleRate * 1.5) {
                    continue;
                }

                getEngineBuffer()->seekAbs(cuePosition);
                break;
            }
        }
    }
}
