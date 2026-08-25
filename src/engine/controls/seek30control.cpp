#include "engine/controls/seek30control.h"

#include "engine/cachingreader/cachingreaderworker.h"
#include "moc_seek30control.cpp"

Seek30Control::Seek30Control(const QString& group,
        UserSettingsPointer pConfig)
        : EngineControl(group, pConfig),
          m_seekTarget(Seek30SeekTarget{}) {
    createControls();
}

void Seek30Control::setWorker(CachingReaderWorker* pWorker) {
    m_pWorker.store(pWorker, std::memory_order_release);
}

void Seek30Control::process(const double,
        mixxx::audio::FramePos,
        const std::size_t) {
    const auto target = m_seekTarget.getValue();
    if (target.sequence == m_consumedSeekSequence) {
        return;
    }
    m_consumedSeekSequence = target.sequence;

    CachingReaderWorker* const pWorker =
            m_pWorker.load(std::memory_order_acquire);
    if (!pWorker || target.generation != pWorker->seek30Generation() ||
            !target.position.isValid()) {
        return;
    }
    seekAbs(target.position);
}

void Seek30Control::enqueue(Seek30Operation operation) {
    CachingReaderWorker* const pWorker =
            m_pWorker.load(std::memory_order_acquire);
    if (pWorker) {
        pWorker->enqueueSeek30Command(operation);
    }
}

void Seek30Control::createAtCurrent(double v) {
    if (v > 0) {
        enqueue(Seek30Operation::CreateAtCurrent);
    }
}

void Seek30Control::clearAll(double v) {
    if (v > 0) {
        enqueue(Seek30Operation::ClearAll);
    }
}

void Seek30Control::clearPrev(double v) {
    if (v > 0) {
        enqueue(Seek30Operation::ClearPrevious);
    }
}

void Seek30Control::clearNext(double v) {
    if (v > 0) {
        enqueue(Seek30Operation::ClearNext);
    }
}

void Seek30Control::clearCurrent(double v) {
    if (v > 0) {
        enqueue(Seek30Operation::ClearCurrent);
    }
}

void Seek30Control::clearNearest(double v) {
    if (v > 0) {
        enqueue(Seek30Operation::ClearNearest);
    }
}

void Seek30Control::slotSeek30(double v) {
    if (v > 0) {
        enqueue(Seek30Operation::SeekNext);
    }
}

void Seek30Control::slotSeek30Prev(double v) {
    if (v > 0) {
        enqueue(Seek30Operation::SeekPrevious);
    }
}

Seek30WorkerInputs Seek30Control::seek30WorkerInputs() const {
    const auto info = frameInfo();
    const auto closestBeat =
            mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                    ControlObject::get(ConfigKey(m_group, "beat_closest")));
    return {
            info.currentPosition,
            info.sampleRate,
            ControlObject::get(ConfigKey(m_group, "quantize")) > 0.0,
            closestBeat,
            ControlObject::get(ConfigKey(m_group, "play")) > 0.0};
}

void Seek30Control::publishSeekTarget(const Seek30SeekTarget& target) {
    m_seekTarget.setValue(target);
}

void Seek30Control::createControls() {
    m_pMemoryCue = std::make_unique<ControlObject>(ConfigKey(m_group, "memory_cue"));
    m_pMemoryCue->set(0.0);

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
