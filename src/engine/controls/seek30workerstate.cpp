#include "engine/controls/seek30workerstate.h"
#ifdef BUILD_TESTING
#include "test/f001probe.h"
#include "test/f001rtprobe.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "track/track.h"

Seek30CommandMailbox::Seek30CommandMailbox() {
    for (std::size_t index = 0; index < kCapacity; ++index) {
        m_slots[index].sequence.store(index, std::memory_order_relaxed);
    }
}

bool Seek30CommandMailbox::tryPush(Seek30Command command) {
    std::size_t position = m_enqueuePosition.load(std::memory_order_relaxed);
    for (;;) {
        Slot& slot = m_slots[position % kCapacity];
        const std::size_t sequence = slot.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position);
        if (difference == 0) {
            if (m_enqueuePosition.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                slot.command = command;
                slot.sequence.store(position + 1, std::memory_order_release);
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = m_enqueuePosition.load(std::memory_order_relaxed);
        }
    }
}

bool Seek30CommandMailbox::tryPop(Seek30Command* pCommand) {
    std::size_t position = m_dequeuePosition.load(std::memory_order_relaxed);
    for (;;) {
        Slot& slot = m_slots[position % kCapacity];
        const std::size_t sequence = slot.sequence.load(std::memory_order_acquire);
        const auto difference = static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position + 1);
        if (difference == 0) {
            if (m_dequeuePosition.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                *pCommand = slot.command;
                slot.sequence.store(position + kCapacity, std::memory_order_release);
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = m_dequeuePosition.load(std::memory_order_relaxed);
        }
    }
}

void Seek30WorkerState::trackLoaded(TrackPointer pNewTrack) {
    m_pLoadedTrack = std::move(pNewTrack);
    ++m_generation;
    rebuildMemoryCueCache();
}

#ifdef BUILD_TESTING
void Seek30WorkerState::recordUnavailableLoopCueObservationForTest() {
    auto* const pTrace = m_pLoopCueApplyTraceForTest.load(std::memory_order_acquire);
    if (!pTrace) {
        return;
    }
    LoopCueApplySnapshotForTest snapshot;
    snapshot.branch = LoopCueApplySnapshotForTest::Branch::StableObservationUnavailable;
    if (m_pLoadedTrack) {
        snapshot.trackIdentity = reinterpret_cast<std::uintptr_t>(m_pLoadedTrack.get());
        snapshot.trackIdValid = m_pLoadedTrack->getId().isValid();
    }
    pTrace->append(snapshot);
}
#endif

void Seek30WorkerState::updateLoopCue(
        const mixxx::audio::FramePos& startPosition,
        const mixxx::audio::FramePos& endPosition) {
#ifdef BUILD_TESTING
    F001RtProbeScopeGuardForTest f001RtScope(
            F001RtProbeScopeForTest::Seek30UpdateLoopCue, f001CaseTokenForTest(m_pF001ProbeForTest));
#endif
#ifdef BUILD_TESTING
    if (auto* pProbe = m_pF001ProbeForTest.load(std::memory_order_acquire)) {
        pProbe->record(F001ProbeEventForTest::Seek30LoopCueUpdate,
                F001ProbePhaseForTest::Before,
                F001ProbeMethodForTest::None,
                this,
                m_pLoadedTrack.get(),
                startPosition.isValid() ? startPosition.value() : 0,
                endPosition.isValid() ? endPosition.value() : 0,
                0,
                (startPosition.isValid() ? 1U : 0U) |
                        (endPosition.isValid() ? 2U : 0U));
    }
    auto* const pTrace = m_pLoopCueApplyTraceForTest.load(std::memory_order_acquire);
    LoopCueApplySnapshotForTest snapshot;
    if (pTrace) {
        snapshot.observedStartValid = startPosition.isValid();
        snapshot.observedEndValid = endPosition.isValid();
        if (snapshot.observedStartValid) {
            snapshot.observedStart = startPosition.value();
        }
        if (snapshot.observedEndValid) {
            snapshot.observedEnd = endPosition.value();
        }
        if (m_pLoadedTrack) {
            snapshot.trackIdentity = reinterpret_cast<std::uintptr_t>(m_pLoadedTrack.get());
            snapshot.trackIdValid = m_pLoadedTrack->getId().isValid();
        }
    }
    const auto recordCueRange = [](const CuePointer& cue,
                                        bool* pPresent,
                                        bool* pStartValid,
                                        bool* pEndValid,
                                        double* pStart,
                                        double* pEnd) {
        if (!cue) {
            *pPresent = false;
            return;
        }
        *pPresent = true;
        const auto range = cue->getStartAndEndPosition();
        *pStartValid = range.startPosition.isValid();
        *pEndValid = range.endPosition.isValid();
        if (*pStartValid) {
            *pStart = range.startPosition.value();
        }
        if (*pEndValid) {
            *pEnd = range.endPosition.value();
        }
    };
    const auto findTemporaryLoopCue = [](const QList<CuePointer>& cues) {
        for (const auto& cue : cues) {
            if (cue && cue->getType() == mixxx::CueType::Loop &&
                    cue->getHotCue() == Cue::kNoHotCue) {
                return cue;
            }
        }
        return CuePointer{};
    };
    const auto record = [&](LoopCueApplySnapshotForTest::Branch branch,
                                const CuePointer& beforeCue,
                                const CuePointer& afterCue,
                                bool captureBeforeRange) {
        if (!pTrace) {
            return;
        }
        snapshot.branch = branch;
        if (beforeCue) {
            snapshot.cueIdentity = reinterpret_cast<std::uintptr_t>(beforeCue.get());
        } else if (afterCue) {
            snapshot.cueIdentity = reinterpret_cast<std::uintptr_t>(afterCue.get());
        }
        if (captureBeforeRange) {
            recordCueRange(beforeCue,
                    &snapshot.cueBeforePresent,
                    &snapshot.cueBeforeStartValid,
                    &snapshot.cueBeforeEndValid,
                    &snapshot.cueBeforeStart,
                    &snapshot.cueBeforeEnd);
        }
        recordCueRange(afterCue,
                &snapshot.cueAfterPresent,
                &snapshot.cueAfterStartValid,
                &snapshot.cueAfterEndValid,
                &snapshot.cueAfterStart,
                &snapshot.cueAfterEnd);
        pTrace->append(snapshot);
    };
#endif
    if (!m_pLoadedTrack || m_pLoadedTrack->getLocation().isEmpty()) {
#ifdef BUILD_TESTING
        record(LoopCueApplySnapshotForTest::Branch::NoLoadedTrack, {}, {}, true);
#endif
        return;
    }

    if (!startPosition.isValid() || !endPosition.isValid() ||
            endPosition <= startPosition) {
#ifdef BUILD_TESTING
        const auto beforeCue = pTrace
                ? findTemporaryLoopCue(m_pLoadedTrack->getCuePoints())
                : CuePointer{};
#endif
        m_pLoadedTrack->removeTempLoopCue();
#ifdef BUILD_TESTING
        const auto afterCue = pTrace
                ? findTemporaryLoopCue(m_pLoadedTrack->getCuePoints())
                : CuePointer{};
        record(LoopCueApplySnapshotForTest::Branch::RemoveTemporaryCue,
                beforeCue,
                afterCue,
                true);
#endif
        return;
    }

    const QList<CuePointer> cuePoints = m_pLoadedTrack->getCuePoints();
    for (const auto& pCue : cuePoints) {
        if (pCue && pCue->getType() == mixxx::CueType::Loop &&
                pCue->getHotCue() == Cue::kNoHotCue) {
            const auto previous = pCue->getStartAndEndPosition();
            if (previous.startPosition != startPosition ||
                    previous.endPosition != endPosition) {
#ifdef BUILD_TESTING
                if (pTrace) {
                    snapshot.cueBeforePresent = true;
                    snapshot.cueBeforeStartValid = previous.startPosition.isValid();
                    snapshot.cueBeforeEndValid = previous.endPosition.isValid();
                    if (snapshot.cueBeforeStartValid) {
                        snapshot.cueBeforeStart = previous.startPosition.value();
                    }
                    if (snapshot.cueBeforeEndValid) {
                        snapshot.cueBeforeEnd = previous.endPosition.value();
                    }
                }
#endif
                pCue->setStartAndEndPosition(startPosition, endPosition);
                // Cue::updated may queue Track::slotCueUpdated to the GUI.
                // Make this worker's change save-visible before its drain
                // returns, including shutdown without another GUI event pass.
                m_pLoadedTrack->markDirty();
#ifdef BUILD_TESTING
                record(LoopCueApplySnapshotForTest::Branch::UpdateExistingCue,
                        pCue,
                        pCue,
                        false);
#endif
            } else {
#ifdef BUILD_TESTING
                record(LoopCueApplySnapshotForTest::Branch::ExistingCueUnchanged,
                        pCue,
                        pCue,
                        true);
#endif
            }
            return;
        }
    }

    m_pLoadedTrack->createAndAddCue(
            mixxx::CueType::Loop,
            Cue::kNoHotCue,
            startPosition,
            endPosition);
#ifdef BUILD_TESTING
    const auto afterCue = pTrace
            ? findTemporaryLoopCue(m_pLoadedTrack->getCuePoints())
            : CuePointer{};
    record(LoopCueApplySnapshotForTest::Branch::CreateTemporaryCue, {}, afterCue, true);
#endif
}

void Seek30WorkerState::rebuildMemoryCueCache() {
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

void Seek30WorkerState::sortCueCache() {
    std::sort(
            m_memoryCues.begin(),
            m_memoryCues.end(),
            [](const CuePointer& pLeft, const CuePointer& pRight) {
                return pLeft->getStartAndEndPosition().startPosition <
                        pRight->getStartAndEndPosition().startPosition;
            });
}

void Seek30WorkerState::createMemoryCueAt(
        const mixxx::audio::FramePos& position) {
    if (!m_pLoadedTrack || !position.isValid()) {
        return;
    }

    constexpr double kEpsilon = 0.5;
    const double currentPosition = position.toEngineSamplePos();
    for (const auto& pCue : std::as_const(m_memoryCues)) {
        if (pCue) {
            const double cuePosition = pCue->getStartAndEndPosition()
                                               .startPosition
                                               .toEngineSamplePos();
            if (std::abs(currentPosition - cuePosition) < kEpsilon) {
                return;
            }
        }
    }

    const CuePointer pCue = m_pLoadedTrack->createAndAddCue(
            mixxx::CueType::Memory,
            Cue::kNoHotCue,
            position,
            position);
    m_memoryCues.append(pCue);
    sortCueCache();
}

void Seek30WorkerState::clearCurrent(
        const mixxx::audio::FramePos& currentPosition) {
    constexpr double kEpsilon = 0.5;
    const double current = currentPosition.toEngineSamplePos();
    for (const auto& pCue : std::as_const(m_memoryCues)) {
        if (pCue) {
            const double cuePosition = pCue->getStartAndEndPosition()
                                               .startPosition
                                               .toEngineSamplePos();
            if (std::abs(current - cuePosition) < kEpsilon) {
                m_pLoadedTrack->removeCue(pCue);
                m_memoryCues.removeOne(pCue);
                return;
            }
        }
    }
}

void Seek30WorkerState::clearPrevious(
        const mixxx::audio::FramePos& currentPosition) {
    constexpr double kEpsilon = 0.5;
    const double current = currentPosition.toEngineSamplePos();
    for (int index = m_memoryCues.size() - 1; index >= 0; --index) {
        const auto pCue = m_memoryCues.at(index);
        if (pCue) {
            const double cuePosition = pCue->getStartAndEndPosition()
                                               .startPosition
                                               .toEngineSamplePos();
            if (current - cuePosition > kEpsilon) {
                m_pLoadedTrack->removeCue(pCue);
                m_memoryCues.removeOne(pCue);
                return;
            }
        }
    }
}

void Seek30WorkerState::clearNext(
        const mixxx::audio::FramePos& currentPosition) {
    constexpr double kEpsilon = 0.5;
    const double current = currentPosition.toEngineSamplePos();
    for (const auto& pCue : std::as_const(m_memoryCues)) {
        if (pCue) {
            const double cuePosition = pCue->getStartAndEndPosition()
                                               .startPosition
                                               .toEngineSamplePos();
            if (cuePosition - current > kEpsilon) {
                m_pLoadedTrack->removeCue(pCue);
                m_memoryCues.removeOne(pCue);
                return;
            }
        }
    }
}

void Seek30WorkerState::clearNearest(const Seek30WorkerInputs& inputs) {
    if (!inputs.sampleRate.isValid() || m_memoryCues.isEmpty()) {
        return;
    }

    const double current = inputs.currentPosition.value();
    const double tolerance = inputs.sampleRate.toDouble();
    CuePointer pNearestCue;
    double nearestDistance = std::numeric_limits<double>::max() / 2.0;
    for (const auto& pCue : std::as_const(m_memoryCues)) {
        if (!pCue) {
            continue;
        }

        const double cuePosition = pCue->getStartAndEndPosition()
                                           .startPosition
                                           .value();
        const double distance = std::abs(cuePosition - current);
        if (distance >= tolerance) {
            continue;
        }
        if (distance < nearestDistance - 1e-9 ||
                (std::abs(distance - nearestDistance) <= 1e-9 &&
                        cuePosition <= current)) {
            nearestDistance = distance;
            pNearestCue = pCue;
        }
    }

    if (pNearestCue) {
        m_pLoadedTrack->removeCue(pNearestCue);
        m_memoryCues.removeOne(pNearestCue);
    }
}

Seek30SeekTarget Seek30WorkerState::seekNext(
        const Seek30WorkerInputs& inputs) {
    const double current = inputs.currentPosition.toEngineSamplePos();
    constexpr double kEpsilon = 0.5;
    for (const auto& pCue : std::as_const(m_memoryCues)) {
        if (pCue) {
            const auto cuePosition = pCue->getStartAndEndPosition().startPosition;
            if (cuePosition.toEngineSamplePos() - current > kEpsilon) {
                return {cuePosition, m_generation, 0};
            }
        }
    }
    return {{}, m_generation, 0};
}

Seek30SeekTarget Seek30WorkerState::seekPrevious(
        const Seek30WorkerInputs& inputs) {
    const double current = inputs.currentPosition.value();
    constexpr double kEpsilon = 0.5;
    for (int index = m_memoryCues.size() - 1; index >= 0; --index) {
        const auto pCue = m_memoryCues.at(index);
        if (!pCue) {
            continue;
        }

        const auto cuePosition = pCue->getStartAndEndPosition().startPosition;
        const double distance = current - cuePosition.value();
        if (distance <= kEpsilon) {
            continue;
        }
        if (inputs.playing && inputs.sampleRate.isValid() &&
                distance < inputs.sampleRate.toDouble() * 1.5) {
            continue;
        }
        return {cuePosition, m_generation, 0};
    }
    return {{}, m_generation, 0};
}

Seek30SeekTarget Seek30WorkerState::process(
        const Seek30Command& command,
        const Seek30WorkerInputs& inputs) {
    if (command.generation != m_generation || !m_pLoadedTrack) {
        return {{}, m_generation, 0};
    }

    switch (command.operation) {
    case Seek30Operation::SeekNext:
        return seekNext(inputs);
    case Seek30Operation::SeekPrevious:
        return seekPrevious(inputs);
    case Seek30Operation::CreateAtCurrent: {
        auto position = inputs.currentPosition;
        if (inputs.quantize) {
            position = inputs.closestBeat;
        }
        createMemoryCueAt(position);
        return {{}, m_generation, 0};
    }
    case Seek30Operation::ClearAll:
        m_pLoadedTrack->removeCuesOfType(mixxx::CueType::Memory);
        m_memoryCues.clear();
        return {{}, m_generation, 0};
    case Seek30Operation::ClearPrevious:
        clearPrevious(inputs.currentPosition);
        return {{}, m_generation, 0};
    case Seek30Operation::ClearNext:
        clearNext(inputs.currentPosition);
        return {{}, m_generation, 0};
    case Seek30Operation::ClearCurrent:
        clearCurrent(inputs.currentPosition);
        return {{}, m_generation, 0};
    case Seek30Operation::ClearNearest:
        clearNearest(inputs);
        return {{}, m_generation, 0};
    }
    return {{}, m_generation, 0};
}
