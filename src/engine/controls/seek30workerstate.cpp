#include "engine/controls/seek30workerstate.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "track/track.h"

namespace {

void sortMemoryCueSnapshots(
        QList<Seek30MemoryCueSnapshot>& memoryCues) {
    std::sort(
            memoryCues.begin(),
            memoryCues.end(),
            [](const Seek30MemoryCueSnapshot& left,
                    const Seek30MemoryCueSnapshot& right) {
                return left.startPosition < right.startPosition;
            });
}

} // namespace

QList<Seek30MemoryCueSnapshot> snapshotSeek30MemoryCues(
        const QList<CuePointer>& cuePoints) {
    QList<Seek30MemoryCueSnapshot> memoryCues;
    memoryCues.reserve(cuePoints.size());
    for (const auto& pCue : cuePoints) {
        if (!pCue) {
            continue;
        }

        const auto cueSnapshot = pCue->getTypeAndStartPosition();
        if (cueSnapshot.type == mixxx::CueType::Memory &&
                cueSnapshot.startPosition.isValid()) {
            memoryCues.append(
                    {pCue, cueSnapshot.type, cueSnapshot.startPosition});
        }
    }
    sortMemoryCueSnapshots(memoryCues);
    return memoryCues;
}

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
    m_memoryCues.clear();
}

void Seek30WorkerState::rebuildMemoryCueCache() {
    m_memoryCues.clear();
    if (!m_pLoadedTrack) {
        return;
    }

    m_memoryCues = snapshotSeek30MemoryCues(m_pLoadedTrack->getCuePoints());
}

void Seek30WorkerState::sortCueCache() {
    sortMemoryCueSnapshots(m_memoryCues);
}

void Seek30WorkerState::createMemoryCueAt(
        const mixxx::audio::FramePos& position) {
    if (!m_pLoadedTrack || !position.isValid()) {
        return;
    }

    constexpr double kEpsilon = 0.5;
    const double currentPosition = position.toEngineSamplePos();
    for (const auto& cue : std::as_const(m_memoryCues)) {
        const double cuePosition = cue.startPosition.toEngineSamplePos();
        if (std::abs(currentPosition - cuePosition) < kEpsilon) {
            return;
        }
    }

    const CuePointer pCue = m_pLoadedTrack->createAndAddCue(
            mixxx::CueType::Memory,
            Cue::kNoHotCue,
            position,
            position);
    m_memoryCues.append(
            {pCue, mixxx::CueType::Memory, position});
    sortCueCache();
}

void Seek30WorkerState::clearCurrent(
        const mixxx::audio::FramePos& currentPosition) {
    constexpr double kEpsilon = 0.5;
    const double current = currentPosition.toEngineSamplePos();
    for (int index = 0; index < m_memoryCues.size(); ++index) {
        const auto& cue = m_memoryCues.at(index);
        const double cuePosition = cue.startPosition.toEngineSamplePos();
        if (std::abs(current - cuePosition) < kEpsilon) {
            m_pLoadedTrack->removeCue(cue.pCue);
            m_memoryCues.removeAt(index);
            return;
        }
    }
}

void Seek30WorkerState::clearPrevious(
        const mixxx::audio::FramePos& currentPosition) {
    constexpr double kEpsilon = 0.5;
    const double current = currentPosition.toEngineSamplePos();
    for (int index = m_memoryCues.size() - 1; index >= 0; --index) {
        const auto& cue = m_memoryCues.at(index);
        const double cuePosition = cue.startPosition.toEngineSamplePos();
        if (current - cuePosition > kEpsilon) {
            m_pLoadedTrack->removeCue(cue.pCue);
            m_memoryCues.removeAt(index);
            return;
        }
    }
}

void Seek30WorkerState::clearNext(
        const mixxx::audio::FramePos& currentPosition) {
    constexpr double kEpsilon = 0.5;
    const double current = currentPosition.toEngineSamplePos();
    for (int index = 0; index < m_memoryCues.size(); ++index) {
        const auto& cue = m_memoryCues.at(index);
        const double cuePosition = cue.startPosition.toEngineSamplePos();
        if (cuePosition - current > kEpsilon) {
            m_pLoadedTrack->removeCue(cue.pCue);
            m_memoryCues.removeAt(index);
            return;
        }
    }
}

void Seek30WorkerState::clearNearest(const Seek30WorkerInputs& inputs) {
    if (!inputs.sampleRate.isValid() || m_memoryCues.isEmpty()) {
        return;
    }

    const double current = inputs.currentPosition.value();
    const double tolerance = inputs.sampleRate.toDouble();
    int nearestIndex = -1;
    double nearestDistance = std::numeric_limits<double>::max() / 2.0;
    for (int index = 0; index < m_memoryCues.size(); ++index) {
        const double cuePosition = m_memoryCues.at(index).startPosition.value();
        const double distance = std::abs(cuePosition - current);
        if (distance >= tolerance) {
            continue;
        }
        if (distance < nearestDistance - 1e-9 ||
                (std::abs(distance - nearestDistance) <= 1e-9 &&
                        cuePosition <= current)) {
            nearestDistance = distance;
            nearestIndex = index;
        }
    }

    if (nearestIndex >= 0) {
        m_pLoadedTrack->removeCue(m_memoryCues.at(nearestIndex).pCue);
        m_memoryCues.removeAt(nearestIndex);
    }
}

Seek30SeekTarget Seek30WorkerState::seekNext(
        const Seek30WorkerInputs& inputs) {
    const double current = inputs.currentPosition.toEngineSamplePos();
    constexpr double kEpsilon = 0.5;
    for (const auto& cue : std::as_const(m_memoryCues)) {
        const auto cuePosition = cue.startPosition;
        if (cuePosition.toEngineSamplePos() - current > kEpsilon) {
            return {cuePosition, m_generation, 0};
        }
    }
    return {{}, m_generation, 0};
}

Seek30SeekTarget Seek30WorkerState::seekPrevious(
        const Seek30WorkerInputs& inputs) {
    const double current = inputs.currentPosition.value();
    constexpr double kEpsilon = 0.5;
    for (int index = m_memoryCues.size() - 1; index >= 0; --index) {
        const auto cuePosition = m_memoryCues.at(index).startPosition;
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

    // Cues may be edited outside this worker after the track is loaded. Refresh
    // on the worker thread before each command uses the memory-cue cache.
    if (m_cueSnapshotDiagnosticHook) {
        m_cueSnapshotDiagnosticHook(m_cueSnapshotDiagnosticContext, true);
    }
    // The elapsed snapshot phase includes list-copy/sort work and possible
    // waits on the Track and Cue locks.
    rebuildMemoryCueCache();
    if (m_cueSnapshotDiagnosticHook) {
        m_cueSnapshotDiagnosticHook(m_cueSnapshotDiagnosticContext, false);
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
