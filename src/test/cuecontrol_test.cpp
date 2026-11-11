#include "engine/controls/cuecontrol.h"

#include <QThread>
#include <semaphore>
#include <thread>

#include "engine/controls/bpmcontrol.h"
#include "engine/controls/seek30control.h"
#include "test/signalpathtest.h"

class CueControlTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();

        m_pQuantizeEnabled = std::make_unique<ControlProxy>(m_sGroup1, "quantize");
        m_pCuePoint = std::make_unique<ControlProxy>(m_sGroup1, "cue_point");
        m_pIntroStartPosition = std::make_unique<ControlProxy>(m_sGroup1, "intro_start_position");
        m_pIntroStartEnabled = std::make_unique<ControlProxy>(m_sGroup1, "intro_start_enabled");
        m_pIntroStartSet = std::make_unique<ControlProxy>(m_sGroup1, "intro_start_set");
        m_pIntroStartClear = std::make_unique<ControlProxy>(m_sGroup1, "intro_start_clear");
        m_pIntroEndPosition = std::make_unique<ControlProxy>(m_sGroup1, "intro_end_position");
        m_pIntroEndEnabled = std::make_unique<ControlProxy>(m_sGroup1, "intro_end_enabled");
        m_pIntroEndSet = std::make_unique<ControlProxy>(m_sGroup1, "intro_end_set");
        m_pIntroEndClear = std::make_unique<ControlProxy>(m_sGroup1, "intro_end_clear");
        m_pOutroStartPosition = std::make_unique<ControlProxy>(m_sGroup1, "outro_start_position");
        m_pOutroStartEnabled = std::make_unique<ControlProxy>(m_sGroup1, "outro_start_enabled");
        m_pOutroStartSet = std::make_unique<ControlProxy>(m_sGroup1, "outro_start_set");
        m_pOutroStartClear = std::make_unique<ControlProxy>(m_sGroup1, "outro_start_clear");
        m_pOutroEndPosition = std::make_unique<ControlProxy>(m_sGroup1, "outro_end_position");
        m_pOutroEndEnabled = std::make_unique<ControlProxy>(m_sGroup1, "outro_end_enabled");
        m_pOutroEndSet = std::make_unique<ControlProxy>(m_sGroup1, "outro_end_set");
        m_pOutroEndClear = std::make_unique<ControlProxy>(m_sGroup1, "outro_end_clear");
    }

    TrackPointer createTestTrack() const {
        const QString kTrackLocationTest = getTestDir().filePath(QStringLiteral("sine-30.wav"));
        const auto pTrack = Track::newTemporary(
                mixxx::FileAccess(mixxx::FileInfo(kTrackLocationTest)));
        pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(2),
                mixxx::audio::SampleRate(44100),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(180));
        return pTrack;
    }

    void loadTrack(TrackPointer pTrack) {
        BaseSignalPathTest::loadTrack(m_pMixerDeck1.get(), pTrack);
        ProcessBuffer();
    }

    TrackPointer createAndLoadFakeTrack() {
        return m_pMixerDeck1->loadFakeTrack(false, 0.0);
    }

    void unloadTrack() {
        m_pMixerDeck1->slotLoadTrack(TrackPointer(),
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                false);
    }

    mixxx::audio::FramePos getCurrentFramePos() {
        return m_pChannel1->getEngineBuffer()->m_pCueControl->frameInfo().currentPosition;
    }

    void setCurrentFramePos(mixxx::audio::FramePos position) {
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(position, EngineBuffer::SEEK_STANDARD);
        ProcessBuffer();
    }

    void processMemoryCommands() {
        QCoreApplication::sendPostedEvents();
        ASSERT_TRUE(QMetaObject::invokeMethod(
                m_pChannel1->getEngineBuffer()->m_pSeek30Control,
                "processPendingCommands",
                Qt::DirectConnection));
    }

    void reloadMemoryControl(TrackPointer pTrack) {
        m_pChannel1->getEngineBuffer()->m_pSeek30Control->trackLoaded(std::move(pTrack));
        processMemoryCommands();
    }

    void processDownbeatCommands() {
        QCoreApplication::sendPostedEvents();
        ASSERT_TRUE(QMetaObject::invokeMethod(
                m_pChannel1->getEngineBuffer()->m_pBpmControl,
                "processPendingDownbeatDelta",
                Qt::DirectConnection));
    }

    void triggerDownbeat(bool forward) {
        const ConfigKey key(m_sGroup1,
                forward ? QStringLiteral("beats_forward_down_beats_marker")
                        : QStringLiteral("beats_backward_down_beats_marker"));
        ControlObject::set(key, 1.0);
        ControlObject::set(key, 0.0);
    }

    std::unique_ptr<ControlProxy> m_pQuantizeEnabled;
    std::unique_ptr<ControlProxy> m_pCuePoint;
    std::unique_ptr<ControlProxy> m_pIntroStartPosition;
    std::unique_ptr<ControlProxy> m_pIntroStartEnabled;
    std::unique_ptr<ControlProxy> m_pIntroStartSet;
    std::unique_ptr<ControlProxy> m_pIntroStartClear;
    std::unique_ptr<ControlProxy> m_pIntroEndPosition;
    std::unique_ptr<ControlProxy> m_pIntroEndEnabled;
    std::unique_ptr<ControlProxy> m_pIntroEndSet;
    std::unique_ptr<ControlProxy> m_pIntroEndClear;
    std::unique_ptr<ControlProxy> m_pOutroStartPosition;
    std::unique_ptr<ControlProxy> m_pOutroStartEnabled;
    std::unique_ptr<ControlProxy> m_pOutroStartSet;
    std::unique_ptr<ControlProxy> m_pOutroStartClear;
    std::unique_ptr<ControlProxy> m_pOutroEndPosition;
    std::unique_ptr<ControlProxy> m_pOutroEndEnabled;
    std::unique_ptr<ControlProxy> m_pOutroEndSet;
    std::unique_ptr<ControlProxy> m_pOutroEndClear;
};

class MemoryCueControlTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pQuantize = std::make_unique<ControlObject>(ConfigKey(kGroup, "quantize"));
        m_pControl = std::make_unique<Seek30Control>(kGroup, config());
        m_pTrack = Track::newTemporary();
        m_pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
                mixxx::audio::SampleRate(48000),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(180));
        m_pControl->setFrameInfo(mixxx::audio::FramePos(48000),
                mixxx::audio::FramePos(8640000),
                mixxx::audio::SampleRate(48000));
        m_pControl->trackLoaded(m_pTrack);
    }

    void trigger(const char* key) {
        ControlObject::set(ConfigKey(kGroup, key), 1.0);
        ControlObject::set(ConfigKey(kGroup, key), 0.0);
    }

    void processPending() {
        ASSERT_TRUE(QMetaObject::invokeMethod(
                m_pControl.get(), "processPendingCommands", Qt::DirectConnection));
    }

    void setPosition(mixxx::audio::FramePos position) {
        m_pControl->setFrameInfo(position,
                mixxx::audio::FramePos(8640000),
                mixxx::audio::SampleRate(48000));
    }

    mixxx::audio::FramePos navigationPosition() const {
        return m_pControl->m_navigationPosition;
    }

    uint32_t currentGeneration() const {
        return m_pControl->m_trackGeneration.load(std::memory_order_acquire);
    }

    void enqueueCreateForGeneration(uint32_t generation) {
        m_pControl->enqueueCommand(Seek30Control::Command::Create, generation);
    }

    bool prepareBatchForGeneration(uint32_t generation) {
        return m_pControl->preparePendingCommandBatch(generation);
    }

    uint32_t droppedCommands() const {
        return m_pControl->m_droppedCommands.load(std::memory_order_relaxed);
    }

    uint32_t pendingCommands() {
        prepareBatchForGeneration(currentGeneration());
        return m_pControl->m_pendingBatchSize;
    }

    const QString kGroup = QStringLiteral("[MemoryCueTest]");
    TrackPointer m_pTrack;
    std::unique_ptr<ControlObject> m_pQuantize;
    std::unique_ptr<Seek30Control> m_pControl;
};

TEST_F(MemoryCueControlTest, CreatesPointWithoutHotSlotOrEnd) {
    trigger("memory_create_at_current");
    const auto cues = m_pTrack->getCuePoints();
    ASSERT_EQ(1, cues.size());
    EXPECT_EQ(mixxx::CueType::Memory, cues.front()->getType());
    EXPECT_EQ(Cue::kNoHotCue, cues.front()->getHotCue());
    EXPECT_EQ(mixxx::audio::FramePos(48000), cues.front()->getPosition());
    EXPECT_FALSE(cues.front()->getEndPosition().isValid());
}

TEST_F(MemoryCueControlTest, CreatesPointWhenQuantizationHasNoValidBeat) {
    ControlObject closestBeat(ConfigKey(kGroup, "beat_closest"));
    closestBeat.set(mixxx::audio::FramePos::kLegacyInvalidEnginePosition);
    m_pQuantize->set(1.0);
    trigger("memory_create_at_current");
    const auto cues = m_pTrack->getCuePoints();
    ASSERT_EQ(1, cues.size());
    EXPECT_EQ(mixxx::audio::FramePos(48000), cues.front()->getPosition());
}

TEST_F(MemoryCueControlTest, DeferredCreationUsesTriggerPosition) {
    std::thread worker([this]() {
        trigger("memory_create_at_current");
    });
    worker.join();
    setPosition(mixxx::audio::FramePos(96000));
    processPending();
    const auto cues = m_pTrack->getCuePoints();
    ASSERT_EQ(1, cues.size());
    EXPECT_EQ(mixxx::audio::FramePos(48000), cues.front()->getPosition());
}

TEST_F(MemoryCueControlTest, DeferredCreationQuantizesTriggerPosition) {
    ASSERT_TRUE(m_pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(48000), mixxx::audio::kStartFramePos, mixxx::Bpm(120.0))));
    m_pQuantize->set(1.0);
    setPosition(mixxx::audio::FramePos(50000));
    std::thread worker([this]() {
        trigger("memory_create_at_current");
    });
    worker.join();
    setPosition(mixxx::audio::FramePos(96000));
    processPending();
    const auto cues = m_pTrack->getCuePoints();
    ASSERT_EQ(1, cues.size());
    EXPECT_EQ(mixxx::audio::FramePos(48000), cues.front()->getPosition());
}

TEST_F(MemoryCueControlTest, DeferredDeletionUsesTriggerPosition) {
    m_pTrack->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(48000),
            mixxx::audio::kInvalidFramePos);
    const auto laterCue = m_pTrack->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(96000),
            mixxx::audio::kInvalidFramePos);
    std::thread worker([this]() {
        trigger("memory_clear_current");
    });
    worker.join();
    setPosition(mixxx::audio::FramePos(96000));
    processPending();
    const auto cues = m_pTrack->getCuePoints();
    ASSERT_EQ(1, cues.size());
    EXPECT_EQ(laterCue, cues.front());
}

TEST_F(MemoryCueControlTest, DeferredNavigationUsesTriggerPosition) {
    for (const auto position : {mixxx::audio::FramePos(72000),
                 mixxx::audio::FramePos(144000)}) {
        m_pTrack->createAndAddCue(mixxx::CueType::Memory,
                Cue::kNoHotCue,
                position,
                mixxx::audio::kInvalidFramePos);
    }
    std::thread worker([this]() {
        trigger("seek_30s");
    });
    worker.join();
    setPosition(mixxx::audio::FramePos(96000));
    processPending();
    EXPECT_EQ(mixxx::audio::FramePos(72000), navigationPosition());
}

TEST_F(MemoryCueControlTest, SeesImportedAndEditedCuesWithoutTreatingHighSlotsAsMemory) {
    const auto pHotCue = m_pTrack->createAndAddCue(mixxx::CueType::HotCue,
            16,
            mixxx::audio::FramePos(48000),
            mixxx::audio::kInvalidFramePos);
    const auto pMemory = m_pTrack->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(24000),
            mixxx::audio::kInvalidFramePos);
    pMemory->setStartPosition(mixxx::audio::FramePos(48000));
    pMemory->setLabel(QStringLiteral("edited after import"));
    pMemory->setColor(mixxx::RgbColor(0x123456));
    trigger("memory_clear_current");
    const auto cues = m_pTrack->getCuePoints();
    ASSERT_EQ(1, cues.size());
    EXPECT_EQ(pHotCue, cues.front());
    trigger("memory_clear_all");
    EXPECT_EQ(pHotCue, m_pTrack->findHotcueByIndex(16));
}

TEST_F(MemoryCueControlTest, RemovedCueDoesNotMaskReplacementAtSamePosition) {
    const auto pOldCue = m_pTrack->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(48000),
            mixxx::audio::kInvalidFramePos);
    m_pControl->trackLoaded(m_pTrack);
    m_pTrack->removeCue(pOldCue);
    m_pTrack->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(48000),
            mixxx::audio::kInvalidFramePos);
    trigger("memory_clear_nearest");
    EXPECT_TRUE(m_pTrack->getCuePoints().isEmpty());
}

TEST_F(MemoryCueControlTest, WorkerTriggerDefersMutationToOwnerThread) {
    Qt::HANDLE mutationThread = nullptr;
    QObject::connect(m_pTrack.get(), &Track::cuesUpdated, [&mutationThread]() {
        mutationThread = QThread::currentThreadId();
    });
    std::thread worker([this]() {
        trigger("memory_create_at_current");
    });
    worker.join();
    EXPECT_TRUE(m_pTrack->getCuePoints().isEmpty());
    EXPECT_EQ(nullptr, mutationThread);
    processPending();
    ASSERT_EQ(1, m_pTrack->getCuePoints().size());
    EXPECT_EQ(QThread::currentThreadId(), mutationThread);
}

TEST_F(MemoryCueControlTest, PendingCommandsDoNotMutateReplacementTrack) {
    std::thread worker([this]() {
        trigger("memory_clear_all");
    });
    worker.join();
    auto pReplacement = Track::newTemporary();
    pReplacement->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(48000),
            mixxx::audio::kInvalidFramePos);
    m_pControl->trackLoaded(pReplacement);
    processPending();
    EXPECT_EQ(1, pReplacement->getCuePoints().size());
}

TEST_F(MemoryCueControlTest, StaleProducerCannotReplaceNewTrackCommands) {
    const auto previousGeneration = currentGeneration();
    const auto pReplacement = Track::newTemporary();
    m_pControl->trackLoaded(pReplacement);
    std::thread worker([this]() {
        trigger("memory_create_at_current");
    });
    worker.join();
    enqueueCreateForGeneration(previousGeneration);
    processPending();
    EXPECT_EQ(1, pReplacement->getCuePoints().size());
    EXPECT_TRUE(m_pTrack->getCuePoints().isEmpty());
}

TEST_F(MemoryCueControlTest, StaleConsumerCannotRemoveNewTrackCommands) {
    const auto previousGeneration = currentGeneration();
    const auto pReplacement = Track::newTemporary();
    m_pControl->trackLoaded(pReplacement);
    std::thread worker([this]() {
        trigger("memory_create_at_current");
    });
    worker.join();
    EXPECT_FALSE(prepareBatchForGeneration(previousGeneration));
    processPending();
    EXPECT_EQ(1, pReplacement->getCuePoints().size());
    EXPECT_TRUE(m_pTrack->getCuePoints().isEmpty());
}

TEST_F(MemoryCueControlTest, WorkerClearThenCreateKeepsNewCue) {
    m_pTrack->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(24000),
            mixxx::audio::kInvalidFramePos);
    std::thread worker([this]() {
        trigger("memory_clear_all");
        trigger("memory_create_at_current");
    });
    worker.join();
    processPending();
    const auto cues = m_pTrack->getCuePoints();
    ASSERT_EQ(1, cues.size());
    EXPECT_EQ(mixxx::audio::FramePos(48000), cues.front()->getPosition());
}

TEST_F(MemoryCueControlTest, WorkerCreateThenClearRemovesNewCue) {
    std::thread worker([this]() {
        trigger("memory_create_at_current");
        trigger("memory_clear_all");
    });
    worker.join();
    processPending();
    EXPECT_TRUE(m_pTrack->getCuePoints().isEmpty());
}

TEST_F(MemoryCueControlTest, MixedProducersPreserveAcceptedCommandOrder) {
    std::binary_semaphore firstPublished{0};
    std::binary_semaphore secondPublished{0};
    std::thread first([&]() {
        trigger("memory_create_at_current");
        firstPublished.release();
        secondPublished.acquire();
        trigger("memory_create_at_current");
    });
    std::thread second([&]() {
        firstPublished.acquire();
        trigger("memory_clear_all");
        secondPublished.release();
    });
    first.join();
    second.join();
    EXPECT_TRUE(m_pTrack->getCuePoints().isEmpty());
    processPending();
    EXPECT_EQ(1, m_pTrack->getCuePoints().size());
}

TEST_F(MemoryCueControlTest, QueueBoundAndRecyclingPreserveAcceptedCommands) {
    std::thread worker([this]() {
        for (int i = 0; i < 32; ++i) {
            trigger("memory_create_at_current");
            trigger("memory_clear_all");
        }
        trigger("memory_create_at_current");
    });
    worker.join();
    EXPECT_EQ(1, droppedCommands());
    processPending();
    EXPECT_EQ(0, droppedCommands());
    EXPECT_TRUE(m_pTrack->getCuePoints().isEmpty());

    std::thread nextWorker([this]() {
        trigger("memory_create_at_current");
    });
    nextWorker.join();
    processPending();
    EXPECT_EQ(1, m_pTrack->getCuePoints().size());
}

TEST_F(MemoryCueControlTest, ConcurrentProducersPublishAndRecycleAllAvailableSlots) {
    const auto generation = currentGeneration();
    for (int round = 0; round < 4; ++round) {
        std::array<std::thread, 4> workers;
        for (auto& worker : workers) {
            worker = std::thread([this, generation]() {
                for (int i = 0; i < 16; ++i) {
                    enqueueCreateForGeneration(generation);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        EXPECT_EQ(0, droppedCommands());
        EXPECT_EQ(64, pendingCommands());
        processPending();
        EXPECT_EQ(1, m_pTrack->getCuePoints().size());
        trigger("memory_clear_all");
        EXPECT_TRUE(m_pTrack->getCuePoints().isEmpty());
    }
}

TEST_F(MemoryCueControlTest, PreparedBatchSurvivesTrackHandoffWithoutLosingNewCommands) {
    std::thread oldWorker([this]() {
        trigger("memory_clear_all");
    });
    oldWorker.join();
    ASSERT_TRUE(prepareBatchForGeneration(currentGeneration()));
    const auto pReplacement = Track::newTemporary();
    pReplacement->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(24000),
            mixxx::audio::kInvalidFramePos);
    m_pControl->trackLoaded(pReplacement);
    std::thread newWorker([this]() {
        trigger("memory_create_at_current");
    });
    newWorker.join();
    processPending();
    EXPECT_EQ(2, pReplacement->getCuePoints().size());
}

TEST_F(CueControlTest, MemoryNavigationRecallsLoopOnlyWithExplicitActiveIntent) {
    auto pTrack = createAndLoadFakeTrack();
    m_pQuantizeEnabled->set(0.0);
    const auto pLoop = pTrack->createAndAddCue(mixxx::CueType::Memory,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(44100),
            mixxx::audio::FramePos(88200));
    pLoop->setActiveLoopIntent(true);
    reloadMemoryControl(pTrack);
    EXPECT_DOUBLE_EQ(0.0, ControlObject::get(ConfigKey(m_sGroup1, "loop_enabled")));
    for (const auto intent : {std::optional<bool>{true},
                 std::optional<bool>{},
                 std::optional<bool>{true},
                 std::optional<bool>{false}}) {
        pLoop->setActiveLoopIntent(intent);
        setCurrentFramePos(mixxx::audio::kStartFramePos);
        ControlObject::set(ConfigKey(m_sGroup1, "seek_30s"), 1.0);
        ControlObject::set(ConfigKey(m_sGroup1, "seek_30s"), 0.0);
        processMemoryCommands();
        ProcessBuffer();
        EXPECT_DOUBLE_EQ(88200.0,
                ControlObject::get(ConfigKey(m_sGroup1, "loop_start_position")));
        EXPECT_DOUBLE_EQ(176400.0,
                ControlObject::get(ConfigKey(m_sGroup1, "loop_end_position")));
        EXPECT_DOUBLE_EQ(intent.value_or(false) ? 1.0 : 0.0,
                ControlObject::get(ConfigKey(m_sGroup1, "loop_enabled")));
    }
}

TEST_F(CueControlTest, RepeatedWorkerMemoryNavigationAdvancesEachStep) {
    auto pTrack = createAndLoadFakeTrack();
    m_pQuantizeEnabled->set(0.0);
    for (int i = 1; i <= 3; ++i) {
        pTrack->createAndAddCue(mixxx::CueType::Memory,
                Cue::kNoHotCue,
                mixxx::audio::FramePos(i * 44100),
                mixxx::audio::kInvalidFramePos);
    }
    processMemoryCommands();
    setCurrentFramePos(mixxx::audio::kStartFramePos);
    std::thread worker([this]() {
        for (int i = 0; i < 2; ++i) {
            ControlObject::set(ConfigKey(m_sGroup1, "seek_30s"), 1.0);
            ControlObject::set(ConfigKey(m_sGroup1, "seek_30s"), 0.0);
        }
    });
    worker.join();
    processMemoryCommands();
    ProcessBuffer();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(88200), getCurrentFramePos());
}

TEST_F(CueControlTest, RepeatedGuiMemoryNavigationAdvancesEachStep) {
    auto pTrack = createAndLoadFakeTrack();
    m_pQuantizeEnabled->set(0.0);
    for (int i = 1; i <= 3; ++i) {
        pTrack->createAndAddCue(mixxx::CueType::Memory,
                Cue::kNoHotCue,
                mixxx::audio::FramePos(i * 44100),
                mixxx::audio::kInvalidFramePos);
    }
    processMemoryCommands();
    setCurrentFramePos(mixxx::audio::kStartFramePos);
    for (int i = 0; i < 2; ++i) {
        ControlObject::set(ConfigKey(m_sGroup1, "seek_30s"), 1.0);
        ControlObject::set(ConfigKey(m_sGroup1, "seek_30s"), 0.0);
    }
    ProcessBuffer();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(88200), getCurrentFramePos());
}

TEST_F(CueControlTest, DownbeatWorkerCommandsRunOnOwnerThreadAndWrap) {
    const auto pTrack = createAndLoadFakeTrack();
    ASSERT_TRUE(pTrack->trySetBeats(
            mixxx::Beats::fromConstTempo(pTrack->getSampleRate(),
                    mixxx::audio::kStartFramePos,
                    mixxx::Bpm(120.0),
                    QString(),
                    15)));
    processDownbeatCommands();
    pTrack->markClean();
    Qt::HANDLE mutationThread = nullptr;
    int updates = 0;
    const auto connection = QObject::connect(pTrack.get(), &Track::beatsUpdated, [&]() {
        mutationThread = QThread::currentThreadId();
        ++updates;
    });
    std::thread worker([this]() {
        for (int i = 0; i < 19; ++i) {
            triggerDownbeat(true);
        }
        triggerDownbeat(false);
    });
    worker.join();
    EXPECT_EQ(15, pTrack->getDownbeatOffset());
    EXPECT_FALSE(pTrack->isDirty());
    EXPECT_EQ(0, updates);
    processDownbeatCommands();
    EXPECT_EQ(1, pTrack->getDownbeatOffset());
    EXPECT_TRUE(pTrack->isDirty());
    EXPECT_EQ(1, updates);
    EXPECT_EQ(QThread::currentThreadId(), mutationThread);
    QObject::disconnect(connection);
}

TEST_F(CueControlTest, DownbeatControlsRespectMissingAndLockedGrids) {
    const auto pTrack = createAndLoadFakeTrack();
    pTrack->trySetBeats(nullptr);
    processDownbeatCommands();
    pTrack->markClean();
    triggerDownbeat(true);
    EXPECT_FALSE(pTrack->getBeats());
    EXPECT_EQ(0, pTrack->getDownbeatOffset());
    EXPECT_FALSE(pTrack->isDirty());

    ASSERT_TRUE(pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
            pTrack->getSampleRate(), mixxx::audio::kStartFramePos, mixxx::Bpm(120.0))));
    pTrack->setBpmLocked(true);
    pTrack->markClean();
    std::thread worker([this]() {
        triggerDownbeat(false);
    });
    worker.join();
    processDownbeatCommands();
    EXPECT_EQ(0, pTrack->getDownbeatOffset());
    EXPECT_FALSE(pTrack->isDirty());

    pTrack->setBpmLocked(false);
    pTrack->markClean();
    triggerDownbeat(false);
    EXPECT_EQ(15, pTrack->getDownbeatOffset());
    EXPECT_TRUE(pTrack->isDirty());
}

TEST_F(CueControlTest, QueuedDownbeatEditDoesNotAffectReplacementTrack) {
    const auto pOldTrack = createAndLoadFakeTrack();
    ASSERT_TRUE(pOldTrack->trySetBeats(
            mixxx::Beats::fromConstTempo(pOldTrack->getSampleRate(),
                    mixxx::audio::kStartFramePos,
                    mixxx::Bpm(120.0),
                    QString(),
                    4)));
    processDownbeatCommands();
    std::thread worker([this]() {
        triggerDownbeat(true);
    });
    worker.join();
    const auto pReplacement = createAndLoadFakeTrack();
    ASSERT_NE(pOldTrack, pReplacement);
    ASSERT_TRUE(pReplacement->trySetBeats(
            mixxx::Beats::fromConstTempo(pReplacement->getSampleRate(),
                    mixxx::audio::kStartFramePos,
                    mixxx::Bpm(120.0),
                    QString(),
                    8)));
    pReplacement->markClean();
    processDownbeatCommands();
    EXPECT_EQ(4, pOldTrack->getDownbeatOffset());
    EXPECT_EQ(8, pReplacement->getDownbeatOffset());
    EXPECT_FALSE(pReplacement->isDirty());
}

TEST_F(CueControlTest, LoadUnloadTrack) {
    constexpr auto kCuePosition = mixxx::audio::FramePos(100);
    constexpr auto kIntroStartPosition = mixxx::audio::FramePos(150);
    constexpr auto kIntroEndPosition = mixxx::audio::FramePos(200);
    constexpr auto kOutroStartPosition = mixxx::audio::FramePos(250);
    constexpr auto kOutroEndPosition = mixxx::audio::FramePos(300);

    TrackPointer pTrack = createTestTrack();
    pTrack->setMainCuePosition(kCuePosition);
    auto pIntro = pTrack->createAndAddCue(
            mixxx::CueType::Intro,
            Cue::kNoHotCue,
            kIntroStartPosition,
            kIntroEndPosition);
    auto pOutro = pTrack->createAndAddCue(
            mixxx::CueType::Outro,
            Cue::kNoHotCue,
            kOutroStartPosition,
            kOutroEndPosition);

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(kCuePosition, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ_CONTROL(kIntroStartPosition, m_pIntroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kIntroEndPosition, m_pIntroEndPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kOutroStartPosition, m_pOutroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kOutroEndPosition, m_pOutroEndPosition);
    EXPECT_TRUE(m_pIntroStartEnabled->toBool());
    EXPECT_TRUE(m_pIntroEndEnabled->toBool());
    EXPECT_TRUE(m_pOutroStartEnabled->toBool());
    EXPECT_TRUE(m_pOutroEndEnabled->toBool());

    unloadTrack();

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pIntroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pIntroEndPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pOutroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pOutroEndPosition);
    EXPECT_FALSE(m_pIntroStartEnabled->toBool());
    EXPECT_FALSE(m_pIntroEndEnabled->toBool());
    EXPECT_FALSE(m_pOutroStartEnabled->toBool());
    EXPECT_FALSE(m_pOutroEndEnabled->toBool());
}

TEST_F(CueControlTest, LoadTrackWithDetectedCues) {
    constexpr auto kCuePosition = mixxx::audio::FramePos(100);
    constexpr auto kOutroEndPosition = mixxx::audio::FramePos(200);

    TrackPointer pTrack = createTestTrack();
    pTrack->setMainCuePosition(kCuePosition);
    auto pIntro = pTrack->createAndAddCue(
            mixxx::CueType::Intro,
            Cue::kNoHotCue,
            kCuePosition,
            mixxx::audio::kInvalidFramePos);
    auto pOutro = pTrack->createAndAddCue(
            mixxx::CueType::Outro,
            Cue::kNoHotCue,
            mixxx::audio::kInvalidFramePos,
            kOutroEndPosition);

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(kCuePosition, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ_CONTROL(kCuePosition, m_pIntroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pIntroEndPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pOutroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kOutroEndPosition, m_pOutroEndPosition);
    EXPECT_TRUE(m_pIntroStartEnabled->toBool());
    EXPECT_FALSE(m_pIntroEndEnabled->toBool());
    EXPECT_FALSE(m_pOutroStartEnabled->toBool());
    EXPECT_TRUE(m_pOutroEndEnabled->toBool());
}

TEST_F(CueControlTest, LoadTrackWithIntroEndAndOutroStart) {
    constexpr auto kIntroEndPosition = mixxx::audio::FramePos(150);
    constexpr auto kOutroStartPosition = mixxx::audio::FramePos(250);

    TrackPointer pTrack = createTestTrack();
    auto pIntro = pTrack->createAndAddCue(
            mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::kInvalidFramePos,
            kIntroEndPosition);
    auto pOutro = pTrack->createAndAddCue(
            mixxx::CueType::Outro,
            Cue::kNoHotCue,
            kOutroStartPosition,
            mixxx::audio::kInvalidFramePos);

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kStartFramePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pIntroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kIntroEndPosition, m_pIntroEndPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kOutroStartPosition, m_pOutroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pOutroEndPosition);
    EXPECT_FALSE(m_pIntroStartEnabled->toBool());
    EXPECT_TRUE(m_pIntroEndEnabled->toBool());
    EXPECT_TRUE(m_pOutroStartEnabled->toBool());
    EXPECT_FALSE(m_pOutroEndEnabled->toBool());
}

TEST_F(CueControlTest, LoadAutodetectedCues_QuantizeEnabled) {
    m_pQuantizeEnabled->set(1);

    TrackPointer pTrack = createTestTrack();
    pTrack->trySetBpm(120.0);

    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    const double bpm = pTrack->getBpm();
    const double beatLengthFrames = (60.0 * sampleRate / bpm);

    const auto kIntroStartPosition = mixxx::audio::FramePos(2.1 * beatLengthFrames);
    const auto kQuantizedIntroStartPosition = mixxx::audio::FramePos(2.0 * beatLengthFrames);
    const auto kIntroEndPosition = mixxx::audio::FramePos(3.7 * beatLengthFrames);
    const auto kQuantizedIntroEndPosition = mixxx::audio::FramePos(4.0 * beatLengthFrames);
    const auto kOutroStartPosition = mixxx::audio::FramePos(11.1 * beatLengthFrames);
    const auto kQuantizedOutroStartPosition = mixxx::audio::FramePos(11.0 * beatLengthFrames);
    const auto kOutroEndPosition = mixxx::audio::FramePos(15.5 * beatLengthFrames);
    const auto kQuantizedOutroEndPosition = mixxx::audio::FramePos(16.0 * beatLengthFrames);

    pTrack->setMainCuePosition(mixxx::audio::FramePos(1.9 * beatLengthFrames));

    auto pIntro = pTrack->createAndAddCue(
            mixxx::CueType::Intro,
            Cue::kNoHotCue,
            kIntroStartPosition,
            kIntroEndPosition);

    auto pOutro = pTrack->createAndAddCue(
            mixxx::CueType::Outro,
            Cue::kNoHotCue,
            kOutroStartPosition,
            kOutroEndPosition);

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(kQuantizedIntroStartPosition, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ_CONTROL(kQuantizedIntroStartPosition, m_pIntroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kQuantizedIntroEndPosition, m_pIntroEndPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kQuantizedOutroStartPosition, m_pOutroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(kQuantizedOutroEndPosition, m_pOutroEndPosition);
}

TEST_F(CueControlTest, LoadAutodetectedCues_QuantizeEnabledNoBeats) {
    m_pQuantizeEnabled->set(1);

    TrackPointer pTrack = createTestTrack();
    pTrack->trySetBpm(0.0);

    constexpr auto kCuePosition = mixxx::audio::FramePos(100);
    pTrack->setMainCuePosition(kCuePosition);

    auto pIntro = pTrack->createAndAddCue(
            mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(250.0),
            mixxx::audio::FramePos(400.0));

    auto pOutro = pTrack->createAndAddCue(
            mixxx::CueType::Outro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(550.0),
            mixxx::audio::FramePos(800.0));

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(kCuePosition, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(250.0), m_pIntroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(400.0), m_pIntroEndPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(550.0), m_pOutroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(800.0), m_pOutroEndPosition);
}

TEST_F(CueControlTest, LoadAutodetectedCues_QuantizeDisabled) {
    m_pQuantizeEnabled->set(0);

    TrackPointer pTrack = createTestTrack();
    pTrack->trySetBpm(120.0);

    pTrack->setMainCuePosition(mixxx::audio::FramePos(240.0));

    auto pIntro = pTrack->createAndAddCue(
            mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(210.0),
            mixxx::audio::FramePos(330.0));

    auto pOutro = pTrack->createAndAddCue(
            mixxx::CueType::Outro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(770.0),
            mixxx::audio::FramePos(990.0));

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(240.0), m_pCuePoint);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(210.0), m_pIntroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(330.0), m_pIntroEndPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(770.0), m_pOutroStartPosition);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(990.0), m_pOutroEndPosition);
}

TEST_F(CueControlTest, SeekOnLoadDefault) {
    // Default is to load at the intro start
    TrackPointer pTrack = createTestTrack();
    auto pIntro = pTrack->createAndAddCue(
            mixxx::CueType::Intro,
            Cue::kNoHotCue,
            mixxx::audio::FramePos(250.0),
            mixxx::audio::FramePos(400.0));

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(250.0), m_pIntroStartPosition);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(250.0), getCurrentFramePos());
}

TEST_F(CueControlTest, SeekOnLoadMainCue) {
    config()->set(ConfigKey("[Controls]", "CueRecall"),
            ConfigValue(static_cast<int>(SeekOnLoadMode::MainCue)));
    TrackPointer pTrack = createTestTrack();
    loadTrack(pTrack);

    // We expect a cue point at the very beginning
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kStartFramePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::kStartFramePos, getCurrentFramePos());

    // Move cue like silence analysis does and check if track is following it
    pTrack->setMainCuePosition(mixxx::audio::FramePos(200.0));
    pTrack->analysisFinished();
    ProcessBuffer();

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(200.0), m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200.0), getCurrentFramePos());
}

TEST_F(CueControlTest, DontSeekOnLoadMainCue) {
    config()->set(ConfigKey("[Controls]", "CueRecall"),
            ConfigValue(static_cast<int>(SeekOnLoadMode::MainCue)));
    TrackPointer pTrack = createTestTrack();
    pTrack->setMainCuePosition(mixxx::audio::FramePos(100.0));

    // The Track should not follow cue changes due to the analyzer if the
    // track has been manual seeked before.
    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(100.0), m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100.0), getCurrentFramePos());

    // Manually seek  the track
    setCurrentFramePos(mixxx::audio::FramePos(200.0));

    // Move cue like silence analysis does and check if track is following it
    pTrack->setMainCuePosition(mixxx::audio::FramePos(400.0));
    pTrack->analysisFinished();
    ProcessBuffer();

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(400.0), m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(200.0), getCurrentFramePos());
}

TEST_F(CueControlTest, SeekOnLoadDefault_CueInPreroll) {
    config()->set(ConfigKey("[Controls]", "CueRecall"),
            ConfigValue(static_cast<int>(SeekOnLoadMode::MainCue)));
    TrackPointer pTrack = createTestTrack();
    pTrack->setMainCuePosition(mixxx::audio::FramePos(-100.0));

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(-100.0), m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(-100.0), getCurrentFramePos());

    // Move cue like silence analysis does and check if track is following it
    pTrack->setMainCuePosition(mixxx::audio::FramePos(-200.0));
    pTrack->analysisFinished();
    ProcessBuffer();

    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(-200.0), m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(-200.0), getCurrentFramePos());
}

TEST_F(CueControlTest, FollowCueOnQuantize) {
    m_pQuantizeEnabled->set(0);
    config()->set(ConfigKey("[Controls]", "CueRecall"),
            ConfigValue(static_cast<int>(SeekOnLoadMode::MainCue)));
    TrackPointer pTrack = createTestTrack();
    pTrack->trySetBpm(120.0);

    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    const double bpm = pTrack->getBpm();
    const mixxx::audio::FrameDiff_t beatLengthFrames = (60.0 * sampleRate / bpm);
    const auto cuePos = mixxx::audio::FramePos(1.8 * beatLengthFrames);
    const auto quantizedCuePos = mixxx::audio::FramePos(2.0 * beatLengthFrames);
    pTrack->setMainCuePosition(cuePos);

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(cuePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(cuePos, getCurrentFramePos());

    // enable quantization and expect current position to follow
    m_pQuantizeEnabled->set(1);
    ProcessBuffer();
    EXPECT_FRAMEPOS_EQ_CONTROL(quantizedCuePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(quantizedCuePos, getCurrentFramePos());

    // move current position to track start
    m_pQuantizeEnabled->set(0);
    ProcessBuffer();
    setCurrentFramePos(mixxx::audio::kStartFramePos);
    ProcessBuffer();
    EXPECT_FRAMEPOS_EQ(mixxx::audio::kStartFramePos, getCurrentFramePos());

    // enable quantization again and expect play position to stay at track start
    m_pQuantizeEnabled->set(1);
    ProcessBuffer();
    EXPECT_FRAMEPOS_EQ_CONTROL(quantizedCuePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(mixxx::audio::kStartFramePos, getCurrentFramePos());
}

TEST_F(CueControlTest, SeekOnSetCueCDJ) {
    // Regression test for https://github.com/mixxxdj/mixxx/issues/10551
    config()->set(ConfigKey("[Controls]", "CueRecall"),
            ConfigValue(static_cast<int>(SeekOnLoadMode::MainCue)));
    m_pQuantizeEnabled->set(1);
    TrackPointer pTrack = createTestTrack();
    pTrack->trySetBpm(120.0);

    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    const double bpm = pTrack->getBpm();
    const mixxx::audio::FrameDiff_t beatLengthFrames = (60.0 * sampleRate / bpm);
    const auto cuePos = mixxx::audio::FramePos(10 * beatLengthFrames);
    pTrack->setMainCuePosition(cuePos);

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(cuePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(cuePos, getCurrentFramePos());

    // Change the playpos and move the cue point there.
    const auto newCuePos = mixxx::audio::FramePos(2.0 * beatLengthFrames);
    setCurrentFramePos(newCuePos);
    m_pChannel1->getEngineBuffer()->m_pCueControl->cueCDJ(1.0);
    ProcessBuffer();

    // Cue point and playpos should be at the same position.
    EXPECT_FRAMEPOS_EQ_CONTROL(newCuePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(newCuePos, getCurrentFramePos());
}

TEST_F(CueControlTest, SeekOnSetCuePlay) {
    // Regression test for https://github.com/mixxxdj/mixxx/issues/10551
    config()->set(ConfigKey("[Controls]", "CueRecall"),
            ConfigValue(static_cast<int>(SeekOnLoadMode::MainCue)));
    m_pQuantizeEnabled->set(1);
    TrackPointer pTrack = createTestTrack();
    pTrack->trySetBpm(120.0);

    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    const double bpm = pTrack->getBpm();
    const mixxx::audio::FrameDiff_t beatLengthFrames = (60.0 * sampleRate / bpm);
    const auto cuePos = mixxx::audio::FramePos(10 * beatLengthFrames);
    pTrack->setMainCuePosition(cuePos);

    loadTrack(pTrack);

    EXPECT_FRAMEPOS_EQ_CONTROL(cuePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(cuePos, getCurrentFramePos());

    // Change the playpos and do a cuePlay.
    const auto newCuePos = mixxx::audio::FramePos(2.0 * beatLengthFrames);
    setCurrentFramePos(newCuePos);
    m_pChannel1->getEngineBuffer()->m_pCueControl->cuePlay(1.0);
    ProcessBuffer();

    // Cue point and playpos should be at the same position.
    EXPECT_FRAMEPOS_EQ_CONTROL(newCuePos, m_pCuePoint);
    EXPECT_FRAMEPOS_EQ(newCuePos, getCurrentFramePos());
}

TEST_F(CueControlTest, IntroCue_SetStartEnd_ClearStartEnd) {
    TrackPointer pTrack = createAndLoadFakeTrack();

    // Set intro start cue
    setCurrentFramePos(mixxx::audio::FramePos(100.0));
    m_pIntroStartSet->set(1);
    m_pIntroStartSet->set(0);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(100.0), m_pIntroStartPosition);
    EXPECT_TRUE(m_pIntroStartEnabled->toBool());
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pIntroEndPosition);
    EXPECT_FALSE(m_pIntroEndEnabled->toBool());

    CuePointer pCue = pTrack->findCueByType(mixxx::CueType::Intro);
    EXPECT_NE(nullptr, pCue);
    if (pCue != nullptr) {
        EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100.0), pCue->getPosition());
        EXPECT_DOUBLE_EQ(0.0, pCue->getLengthFrames());
    }

    // Set intro end cue
    setCurrentFramePos(mixxx::audio::FramePos(500.0));
    m_pIntroEndSet->set(1);
    m_pIntroEndSet->set(0);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(100.0), m_pIntroStartPosition);
    EXPECT_TRUE(m_pIntroStartEnabled->toBool());
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(500.0), m_pIntroEndPosition);
    EXPECT_TRUE(m_pIntroEndEnabled->toBool());

    pCue = pTrack->findCueByType(mixxx::CueType::Intro);
    EXPECT_NE(nullptr, pCue);
    if (pCue != nullptr) {
        EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(100.0), pCue->getPosition());
        EXPECT_DOUBLE_EQ(400.0, pCue->getLengthFrames());
    }

    // Clear intro start cue
    m_pIntroStartClear->set(1);
    m_pIntroStartClear->set(0);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pIntroStartPosition);
    EXPECT_FALSE(m_pIntroStartEnabled->toBool());
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(500.0), m_pIntroEndPosition);
    EXPECT_TRUE(m_pIntroEndEnabled->toBool());

    pCue = pTrack->findCueByType(mixxx::CueType::Intro);
    EXPECT_NE(nullptr, pCue);
    if (pCue != nullptr) {
        EXPECT_FRAMEPOS_EQ(mixxx::audio::kInvalidFramePos, pCue->getPosition());
        EXPECT_DOUBLE_EQ(500.0, pCue->getLengthFrames());
    }

    // Clear intro end cue
    m_pIntroEndClear->set(1);
    m_pIntroEndClear->set(0);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pIntroStartPosition);
    EXPECT_FALSE(m_pIntroStartEnabled->toBool());
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pIntroEndPosition);
    EXPECT_FALSE(m_pIntroEndEnabled->toBool());

    EXPECT_EQ(nullptr, pTrack->findCueByType(mixxx::CueType::Intro));
}

TEST_F(CueControlTest, OutroCue_SetStartEnd_ClearStartEnd) {
    TrackPointer pTrack = createAndLoadFakeTrack();

    // Set outro start cue
    setCurrentFramePos(mixxx::audio::FramePos(750.0));
    m_pOutroStartSet->set(1);
    m_pOutroStartSet->set(0);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(750.0), m_pOutroStartPosition);
    EXPECT_TRUE(m_pOutroStartEnabled->toBool());
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pOutroEndPosition);
    EXPECT_FALSE(m_pOutroEndEnabled->toBool());

    CuePointer pCue = pTrack->findCueByType(mixxx::CueType::Outro);
    EXPECT_NE(nullptr, pCue);
    if (pCue != nullptr) {
        EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(750.0), pCue->getPosition());
        EXPECT_DOUBLE_EQ(0.0, pCue->getLengthFrames());
    }

    // Set outro end cue
    setCurrentFramePos(mixxx::audio::FramePos(1000.0));
    m_pOutroEndSet->set(1);
    m_pOutroEndSet->set(0);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(750.0), m_pOutroStartPosition);
    EXPECT_TRUE(m_pOutroStartEnabled->toBool());
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(1000.0), m_pOutroEndPosition);
    EXPECT_TRUE(m_pOutroEndEnabled->toBool());

    pCue = pTrack->findCueByType(mixxx::CueType::Outro);
    EXPECT_NE(nullptr, pCue);
    if (pCue != nullptr) {
        EXPECT_FRAMEPOS_EQ(mixxx::audio::FramePos(750.0), pCue->getPosition());
        EXPECT_DOUBLE_EQ(250.0, pCue->getLengthFrames());
    }

    // Clear outro start cue
    m_pOutroStartClear->set(1);
    m_pOutroStartClear->set(0);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pOutroStartPosition);
    EXPECT_FALSE(m_pOutroStartEnabled->toBool());
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::FramePos(1000.0), m_pOutroEndPosition);
    EXPECT_TRUE(m_pOutroEndEnabled->toBool());

    pCue = pTrack->findCueByType(mixxx::CueType::Outro);
    EXPECT_NE(nullptr, pCue);
    if (pCue != nullptr) {
        EXPECT_FRAMEPOS_EQ(mixxx::audio::kInvalidFramePos, pCue->getPosition());
        EXPECT_DOUBLE_EQ(1000.0, pCue->getLengthFrames());
    }

    // Clear outro end cue
    m_pOutroEndClear->set(1);
    m_pOutroEndClear->set(0);
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pOutroStartPosition);
    EXPECT_FALSE(m_pOutroStartEnabled->toBool());
    EXPECT_FRAMEPOS_EQ_CONTROL(mixxx::audio::kInvalidFramePos, m_pOutroEndPosition);
    EXPECT_FALSE(m_pOutroEndEnabled->toBool());

    EXPECT_EQ(nullptr, pTrack->findCueByType(mixxx::CueType::Outro));
}
