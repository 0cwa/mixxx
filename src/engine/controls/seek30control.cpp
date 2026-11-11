#include "engine/controls/seek30control.h"

#include <QScopedValueRollback>
#include <QThread>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

#include "engine/enginebuffer.h"
#include "moc_seek30control.cpp"
#include "track/track.h"

namespace {
constexpr double kPositionToleranceFrames = 0.5;
} // namespace

Seek30Control::Seek30Control(const QString& group, UserSettingsPointer pConfig)
        : EngineControl(group, std::move(pConfig)),
          m_pMemoryCue(std::make_unique<ControlObject>(ConfigKey(group, "memory_cue"))),
          m_ownerThreadId(QThread::currentThreadId()) {
    constexpr std::array<const char*, kCommandCount> keys{"seek_30s",
            "seek_30Prev",
            "memory_create_at_current",
            "memory_clear_all",
            "memory_clear_current",
            "memory_clear_prev",
            "memory_clear_next",
            "memory_clear_nearest"};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        m_controls[i] = std::make_unique<ControlPushButton>(ConfigKey(group, keys[i]));
        m_controls[i]->setButtonMode(mixxx::control::ButtonMode::Trigger);
        connect(
                m_controls[i].get(),
                &ControlObject::valueChanged,
                this,
                [this, command = static_cast<Command>(i)](
                        double value) { requestCommand(command, value); },
                Qt::DirectConnection);
    }
    connect(&m_commandTimer, &QTimer::timeout, this, &Seek30Control::processPendingCommands);
    m_commandTimer.start(20);
}

void Seek30Control::trackLoaded(TrackPointer pNewTrack) {
    const auto generation = m_trackGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    QMetaObject::invokeMethod(this, [this, pNewTrack = std::move(pNewTrack), generation]() {
                if (generation != m_trackGeneration.load(std::memory_order_acquire)) {
                    return;
                }
                m_pLoadedTrack = pNewTrack;
                m_loadedGeneration = generation;
                m_navigationPosition = mixxx::audio::kInvalidFramePos;
                rebuildMemoryCueCache(); }, Qt::AutoConnection);
}

void Seek30Control::requestCommand(Command command, double value) {
    if (!(value > 0.0)) {
        return;
    }
    const auto generation = m_trackGeneration.load(std::memory_order_acquire);
    enqueueCommand(command, generation);
    if (QThread::currentThreadId() == m_ownerThreadId) {
        processPendingCommands();
    }
}

void Seek30Control::enqueueCommand(Command command, uint32_t generation) {
    const auto frame = frameInfo();
    const auto queuedSeekPosition = getEngineBuffer()
            ? getEngineBuffer()->queuedSeekPosition()
            : mixxx::audio::kInvalidFramePos;
    auto available = m_availableCommandSlots.load(std::memory_order_relaxed);
    uint32_t index;
    for (;;) {
        if (generation != m_trackGeneration.load(std::memory_order_acquire)) {
            return;
        }
        if (available == 0) {
            m_droppedCommands.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        index = std::countr_zero(available);
        if (m_availableCommandSlots.compare_exchange_weak(available,
                    available & ~(uint64_t{1} << index),
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
            break;
        }
    }
    if (generation != m_trackGeneration.load(std::memory_order_acquire)) {
        m_availableCommandSlots.fetch_or(uint64_t{1} << index, std::memory_order_release);
        return;
    }
    auto& pending = m_commandQueue[index];
    pending.command = command;
    pending.generation = generation;
    pending.frame = frame;
    pending.queuedSeekPosition = queuedSeekPosition;
    auto head = m_pendingCommandHead.load(std::memory_order_relaxed);
    do {
        pending.next = head;
    } while (!m_pendingCommandHead.compare_exchange_weak(
            head, index, std::memory_order_acq_rel, std::memory_order_relaxed));
}

bool Seek30Control::preparePendingCommandBatch(uint32_t generation) {
    if (m_loadedGeneration != generation ||
            generation != m_trackGeneration.load(std::memory_order_acquire)) {
        return false;
    }
    if (m_pendingBatchSize > 0) {
        return true;
    }
    // ChatGPT-generated: Acquire each published list, then consume its indices
    // backwards to preserve publication order. Held slots remain reserved across
    // track-load handoffs and are released only after copying their command.
    // End of ChatGPT-generated comment.
    auto index = m_pendingCommandHead.exchange(kNoPendingCommand, std::memory_order_acquire);
    while (index != kNoPendingCommand) {
        DEBUG_ASSERT(m_pendingBatchSize < kQueueCapacity);
        m_pendingBatch[m_pendingBatchSize++] = index;
        index = m_commandQueue[index].next;
    }
    return m_pendingBatchSize > 0;
}

void Seek30Control::processPendingCommands() {
    if (m_processingCommands) {
        return;
    }
    const QScopedValueRollback<bool> processingGuard(m_processingCommands, true);
    const auto dropped = m_droppedCommands.exchange(0, std::memory_order_relaxed);
    if (dropped) {
        qWarning() << "Memory cue command queue full; dropped" << dropped << "commands";
    }
    m_navigationPosition = mixxx::audio::kInvalidFramePos;
    for (uint32_t processed = 0; processed < kQueueCapacity; ++processed) {
        const auto generation = m_trackGeneration.load(std::memory_order_acquire);
        if (!preparePendingCommandBatch(generation) ||
                generation != m_trackGeneration.load(std::memory_order_acquire)) {
            return;
        }
        const auto index = m_pendingBatch[--m_pendingBatchSize];
        const auto pending = m_commandQueue[index];
        m_availableCommandSlots.fetch_or(uint64_t{1} << index, std::memory_order_release);
        if (pending.generation == generation &&
                generation == m_trackGeneration.load(std::memory_order_acquire)) {
            applyCommand(pending);
        }
    }
}

void Seek30Control::rebuildMemoryCueCache() {
    m_memoryCues.clear();
    if (!m_pLoadedTrack) {
        return;
    }
    const auto cues = m_pLoadedTrack->getCuePoints();
    for (const auto& pCue : cues) {
        if (pCue && pCue->getType() == mixxx::CueType::Memory && pCue->getPosition().isValid()) {
            m_memoryCues.append(pCue);
        }
    }
    std::stable_sort(m_memoryCues.begin(),
            m_memoryCues.end(),
            [](const CuePointer& a, const CuePointer& b) {
                return a->getPosition() < b->getPosition();
            });
}

void Seek30Control::applyCommand(const PendingCommand& pending) {
    if (!m_pLoadedTrack || !pending.frame.sampleRate.isValid()) {
        return;
    }
    rebuildMemoryCueCache();
    switch (pending.command) {
    case Command::Next:
        navigate(false, pending);
        break;
    case Command::Previous:
        navigate(true, pending);
        break;
    case Command::Create:
        createAtCurrent(pending.frame.currentPosition);
        break;
    case Command::ClearAll:
        m_pLoadedTrack->removeCuesOfType(mixxx::CueType::Memory);
        break;
    case Command::ClearCurrent:
    case Command::ClearPrevious:
    case Command::ClearNext:
    case Command::ClearNearest:
        clearAtCurrent(pending.command, pending.frame.currentPosition);
        break;
    case Command::Count:
        break;
    }
}

void Seek30Control::createAtCurrent(mixxx::audio::FramePos position) {
    const auto quantize = ControlObject::getControl(ConfigKey(m_group, "quantize"));
    if (quantize && quantize->toBool()) {
        const auto beats = m_pLoadedTrack->getBeats();
        if (beats) {
            const auto quantizedPosition = beats->findClosestBeat(position);
            if (quantizedPosition.isValid()) {
                position = quantizedPosition;
            }
        }
    }
    if (!position.isValid()) {
        return;
    }
    for (const auto& pCue : std::as_const(m_memoryCues)) {
        if (!pCue->getEndPosition().isValid() &&
                std::abs(position - pCue->getPosition()) < kPositionToleranceFrames) {
            return;
        }
    }
    m_pLoadedTrack->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            position,
            mixxx::audio::kInvalidFramePos);
}

void Seek30Control::clearAtCurrent(Command command, mixxx::audio::FramePos position) {
    CuePointer selected;
    double bestDistance = std::numeric_limits<double>::max();
    for (const auto& pCue : std::as_const(m_memoryCues)) {
        const double delta = pCue->getPosition() - position;
        const double distance = std::abs(delta);
        if ((command == Command::ClearCurrent && distance >= kPositionToleranceFrames) ||
                (command == Command::ClearPrevious && delta >= -kPositionToleranceFrames) ||
                (command == Command::ClearNext && delta <= kPositionToleranceFrames)) {
            continue;
        }
        if (distance < bestDistance) {
            bestDistance = distance;
            selected = pCue;
        }
    }
    if (selected) {
        m_pLoadedTrack->removeCue(selected);
    }
}

void Seek30Control::navigate(bool previous, const PendingCommand& pending) {
    const auto& frame = pending.frame;
    const auto queuedPosition = pending.queuedSeekPosition;
    const auto position = m_navigationPosition.isValid()
            ? m_navigationPosition
            : (queuedPosition.isValid() ? queuedPosition : frame.currentPosition);
    const auto play = ControlObject::getControl(ConfigKey(m_group, "play"));
    const bool skipNearPrevious = previous && play && play->toBool() &&
            !m_navigationPosition.isValid() && !queuedPosition.isValid();
    CuePointer selected;
    for (const auto& pCue : std::as_const(m_memoryCues)) {
        const double delta = pCue->getPosition() - position;
        if (previous) {
            if (delta >= -kPositionToleranceFrames ||
                    (skipNearPrevious && -delta < frame.sampleRate.toDouble() * 1.5)) {
                continue;
            }
            selected = pCue;
        } else if (delta > kPositionToleranceFrames) {
            selected = pCue;
            break;
        }
    }
    if (!selected) {
        return;
    }
    const auto interval = selected->getStartAndEndPosition();
    if (interval.endPosition.isValid() && interval.endPosition > interval.startPosition) {
        // Prepared by ChatGPT. Explicit memory-loop recall restores the interval;
        // only a known active intent enables it. Track load never activates it.
        // End of ChatGPT-generated comment.
        setLoop(interval.startPosition,
                interval.endPosition,
                selected->getActiveLoopIntent().value_or(false));
    }
    seekAbs(interval.startPosition);
    m_navigationPosition = interval.startPosition;
}
