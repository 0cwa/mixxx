#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QScopedPointer>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QtDebug>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "control/controlproxy.h"
#include "control/pollingcontrolproxy.h"
#include "engine/cachingreader/cachingreaderworker.h"
#include "gtest/gtest.h"
#include "mixxxtest.h"
#include "preferences/usersettings.h"
#include "test/signalpathtest.h"

namespace {
const std::vector<std::string> supportedCodecs = {
        "AAC_256kbps_VBR",
        "ALAC_24bit"};

struct StemFileInfo {
    QString dir;
    QString title;
};

static const std::array<StemFileInfo, 2> kStemFileInfos = {
        StemFileInfo{QStringLiteral("stem01"), QStringLiteral("sin")},
        StemFileInfo{QStringLiteral("stem02"), QStringLiteral("trance")}};

struct StemAutoResetConfigGuard {
    UserSettingsPointer config;
    bool originalValue;

    ~StemAutoResetConfigGuard() {
        config->setValue(
                ConfigKey("[Mixer Profile]", "stem_auto_reset"), originalValue);
    }
};

QString trackPointerAddress(const TrackPointer& pTrack) {
    return QStringLiteral("0x%1")
            .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(pTrack.get())),
                    0,
                    16);
}

std::string describeSafeLoadEpochState(Deck* pDeck, const TrackPointer& pExpectedTrack) {
    EngineBuffer* pBuffer = pDeck->getEngineDeck()->getEngineBuffer();
    return QStringLiteral("expected=%1 loading=%2 generation=%3")
            .arg(trackPointerAddress(pExpectedTrack))
            .arg(pBuffer->isTrackLoadingForTest()
                            ? QStringLiteral("true")
                            : QStringLiteral("false"))
            .arg(pBuffer->currentTrackLoadGenerationForTest())
            .toStdString();
}

class ScopedReaderWorkerLifecycleHook {
  public:
    enum class PauseAt {
        None,
        FirstRequestDequeued,
        FirstStartAccepted,
        SecondStartAccepted,
    };

    ScopedReaderWorkerLifecycleHook(const QString& group, PauseAt pauseAt)
            : m_group(group),
              m_pauseAt(pauseAt) {
        CachingReaderWorker::setTestTrackLifecycleHook(
                QString(), &ScopedReaderWorkerLifecycleHook::notify, this);
    }

    ~ScopedReaderWorkerLifecycleHook() {
        release();
        CachingReaderWorker::clearTestTrackLifecycleHook(this);
    }

    bool waitForFirstRequestDequeued() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
                lock, std::chrono::seconds(5), [this] { return m_firstRequestDequeued; });
    }

    bool waitForFirstStartAccepted() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
                lock, std::chrono::seconds(5), [this] { return m_firstStartAccepted; });
    }

    bool waitForSecondStartAccepted() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
                lock, std::chrono::seconds(5), [this] { return m_secondStartAccepted; });
    }

    bool waitForNullTrackUnloaded() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
                lock, std::chrono::seconds(5), [this] { return m_nullTrackUnloaded; });
    }

    std::string diagnostics() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_events.isEmpty()
                ? QStringLiteral("no reader worker lifecycle callbacks observed").toStdString()
                : m_events.join(QStringLiteral("; ")).toStdString();
    }

    void release() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_releasePausedCallback = true;
        }
        m_changed.notify_all();
    }

  private:
    static QString eventName(CachingReaderWorker::TestTrackLifecycleEvent event) {
        switch (event) {
        case CachingReaderWorker::TestTrackLifecycleEvent::RequestDequeued:
            return QStringLiteral("RequestDequeued");
        case CachingReaderWorker::TestTrackLifecycleEvent::TrackLoadingAccepted:
            return QStringLiteral("TrackLoadingAccepted");
        case CachingReaderWorker::TestTrackLifecycleEvent::NullTrackUnloaded:
            return QStringLiteral("NullTrackUnloaded");
        }
        return QStringLiteral("unknown");
    }

    static void notify(
            const QString& group,
            CachingReaderWorker::TestTrackLifecycleEvent event,
            quint64 generation,
            void* context) {
        static_cast<ScopedReaderWorkerLifecycleHook*>(context)->onEvent(
                group, event, generation);
    }

    void onEvent(
            const QString& group,
            CachingReaderWorker::TestTrackLifecycleEvent event,
            quint64 generation) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_events.size() < 64) {
            m_events.append(QStringLiteral("group=%1 event=%2 generation=%3")
                            .arg(group, eventName(event))
                            .arg(generation));
        }
        if (group != m_group) {
            return;
        }
        if (event == CachingReaderWorker::TestTrackLifecycleEvent::RequestDequeued &&
                m_pauseAt == PauseAt::FirstRequestDequeued &&
                !m_firstRequestDequeued) {
            m_firstRequestDequeued = true;
            m_changed.notify_all();
            m_changed.wait(lock, [this] { return m_releasePausedCallback; });
            return;
        }
        if (event == CachingReaderWorker::TestTrackLifecycleEvent::TrackLoadingAccepted) {
            ++m_acceptedStarts;
            if (m_acceptedStarts == 1) {
                m_firstStartAccepted = true;
                m_changed.notify_all();
            }
            if ((m_pauseAt == PauseAt::FirstStartAccepted && m_acceptedStarts == 1) ||
                    (m_pauseAt == PauseAt::SecondStartAccepted && m_acceptedStarts == 2)) {
                if (m_acceptedStarts == 2) {
                    m_secondStartAccepted = true;
                }
                m_changed.notify_all();
                m_changed.wait(lock, [this] { return m_releasePausedCallback; });
                return;
            }
        }
        if (event == CachingReaderWorker::TestTrackLifecycleEvent::NullTrackUnloaded) {
            m_nullTrackUnloaded = true;
            m_changed.notify_all();
        }
    }

    const QString m_group;
    const PauseAt m_pauseAt;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    QStringList m_events;
    int m_acceptedStarts{0};
    bool m_firstRequestDequeued{false};
    bool m_firstStartAccepted{false};
    bool m_secondStartAccepted{false};
    bool m_nullTrackUnloaded{false};
    bool m_releasePausedCallback{false};
};

} // namespace

// must be a std::tuple for std::combine in INSTANTIATE_TEST_SUITE_P
using StemParam = std::tuple<std::string, StemFileInfo>;

class StemControlFixture : public BaseSignalPathTest,
                           public ::testing::WithParamInterface<StemParam> {
  public:
    QString getGroupForStem(QStringView deckGroup, int stemNr) {
        DEBUG_ASSERT(deckGroup.endsWith(QChar(']')) && stemNr <= 4);
        return deckGroup.chopped(1) + QStringLiteral("_Stem") + QChar('0' + stemNr) + QChar(']');
    }
    QString getFxGroupForStem(const QString& deckGroup, int stemNr) {
        return QStringLiteral("[QuickEffectRack1_%1]")
                .arg(getGroupForStem(deckGroup, stemNr));
    }
    QString GetStemFilePath() {
        const auto& [codec, info] = GetParam();
        return getTestDir().filePath(getTestDir()
                        .filePath("stems/%1/%2_%3.stem.mp4")
                        .arg(info.dir,
                                info.title,
                                QString::fromStdString(codec)));
    }

    void SetUp() override {
        BaseSignalPathTest::SetUp();

        for (int i = 1; i <= 4; i++) {
            ChannelHandleAndGroup stemHandleGroup =
                    m_pEngineMixer->registerChannelGroup(getGroupForStem(m_sGroup1, i));
            m_pChannel1->addStemHandle(stemHandleGroup);
            m_pEffectsManager->addStem(stemHandleGroup);
        }
        for (int i = 1; i <= 4; i++) {
            ChannelHandleAndGroup stemHandleGroup =
                    m_pEngineMixer->registerChannelGroup(getGroupForStem(m_sGroup2, i));
            m_pChannel2->addStemHandle(stemHandleGroup);
            m_pEffectsManager->addStem(stemHandleGroup);
        }
        for (int i = 1; i <= 4; i++) {
            ChannelHandleAndGroup stemHandleGroup =
                    m_pEngineMixer->registerChannelGroup(getGroupForStem(m_sGroup3, i));
            m_pChannel3->addStemHandle(stemHandleGroup);
            m_pEffectsManager->addStem(stemHandleGroup);
        }

        const QString sourceStemPath = GetStemFilePath();
        TrackPointer pStemFile(Track::newTemporary(sourceStemPath));

        loadTrack(m_pMixerDeck1.get(), pStemFile);
        loadTrack(m_pMixerDeck3.get(), pStemFile);

        m_pPlay = std::make_unique<PollingControlProxy>(m_sGroup1, "play");

        m_pStem1Volume = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 1), "volume");
        m_pStem2Volume = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 2), "volume");
        m_pStem3Volume = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 3), "volume");
        m_pStem4Volume = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 4), "volume");
        m_pStem1Mute = std::make_unique<PollingControlProxy>(getGroupForStem(m_sGroup1, 1), "mute");
        m_pStem2Mute = std::make_unique<PollingControlProxy>(getGroupForStem(m_sGroup1, 2), "mute");
        m_pStem3Mute = std::make_unique<PollingControlProxy>(getGroupForStem(m_sGroup1, 3), "mute");
        m_pStem4Mute = std::make_unique<PollingControlProxy>(getGroupForStem(m_sGroup1, 4), "mute");
        m_pStem1Color = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 1), "color");
        m_pStem2Color = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 2), "color");
        m_pStem3Color = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 3), "color");
        m_pStem4Color = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 4), "color");
        m_pStem1FXEnabled = std::make_unique<PollingControlProxy>(
                getFxGroupForStem(m_sGroup1, 1), "enabled");
        m_pStem2FXEnabled = std::make_unique<PollingControlProxy>(
                getFxGroupForStem(m_sGroup1, 2), "enabled");
        m_pStem3FXEnabled = std::make_unique<PollingControlProxy>(
                getFxGroupForStem(m_sGroup1, 3), "enabled");
        m_pStem4FXEnabled = std::make_unique<PollingControlProxy>(
                getFxGroupForStem(m_sGroup1, 4), "enabled");

        m_pStem1FXEnabled->set(0.0);
        m_pStem2FXEnabled->set(0.0);
        m_pStem3FXEnabled->set(0.0);
        m_pStem4FXEnabled->set(0.0);

        m_pStem1VuMeter = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 1), "vu_meter");
        m_pStem2VuMeter = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 2), "vu_meter");
        m_pStem3VuMeter = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 3), "vu_meter");
        m_pStem4VuMeter = std::make_unique<PollingControlProxy>(
                getGroupForStem(m_sGroup1, 4), "vu_meter");

        m_pStemCount = std::make_unique<PollingControlProxy>(m_sGroup1, "stem_count");
    }

    void setCurrentPosition(mixxx::audio::FramePos position) {
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(position, EngineBuffer::SEEK_STANDARD);
        ProcessBuffer();
    }

    void loadTrack(Deck* pDeck, TrackPointer pTrack) {
        // Because there is connection across the main thread in caching reader
        // thread, we need to manually process the Qt event loop to trigger
        // `BaseTrackPlayerImpl::slotTrackLoaded` Here is the chain of
        // connections (Symbol (thread)) EngineDeck::slotLoadTrack (main) ->
        // EngineBuffer::loadTrack (main) -> CachingReader*::newTrack (main) ->
        // CachingReaderWorker::trackLoaded (CachingReader) ->
        // EngineBuffer::loaded (CachingReader, direct) ->
        // BaseTrackPlayerImpl::slotTrackLoaded  (main)

        TrackPointer pLoadedTrack;
        QMetaObject::Connection connection = QObject::connect(pDeck,
                &BaseTrackPlayerImpl::newTrackLoaded,
                [&pLoadedTrack]( // clazy:exclude=lambda-in-connect
                        TrackPointer pNewTrack) { pLoadedTrack = pNewTrack; });
        BaseSignalPathTest::loadTrack(pDeck, pTrack);

        for (int i = 0; i < 10000; ++i) {
            if (pLoadedTrack == pTrack) {
                break;
            }
            int maxtime = 1; // ms
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, maxtime);
            // 1 ms for waiting 10 s at max
        }
        QObject::disconnect(connection);
        if (pLoadedTrack != pTrack) {
            qWarning() << "Timeout: failed loading track" << pTrack->getLocation();
        }
    }

    std::unique_ptr<PollingControlProxy> m_pPlay;
    std::unique_ptr<PollingControlProxy> m_pStem1Volume;
    std::unique_ptr<PollingControlProxy> m_pStem2Volume;
    std::unique_ptr<PollingControlProxy> m_pStem3Volume;
    std::unique_ptr<PollingControlProxy> m_pStem4Volume;
    std::unique_ptr<PollingControlProxy> m_pStem1Mute;
    std::unique_ptr<PollingControlProxy> m_pStem2Mute;
    std::unique_ptr<PollingControlProxy> m_pStem3Mute;
    std::unique_ptr<PollingControlProxy> m_pStem4Mute;
    std::unique_ptr<PollingControlProxy> m_pStem1Color;
    std::unique_ptr<PollingControlProxy> m_pStem2Color;
    std::unique_ptr<PollingControlProxy> m_pStem3Color;
    std::unique_ptr<PollingControlProxy> m_pStem4Color;
    std::unique_ptr<PollingControlProxy> m_pStem1FXEnabled;
    std::unique_ptr<PollingControlProxy> m_pStem2FXEnabled;
    std::unique_ptr<PollingControlProxy> m_pStem3FXEnabled;
    std::unique_ptr<PollingControlProxy> m_pStem4FXEnabled;
    std::unique_ptr<PollingControlProxy> m_pStem1VuMeter;
    std::unique_ptr<PollingControlProxy> m_pStem2VuMeter;
    std::unique_ptr<PollingControlProxy> m_pStem3VuMeter;
    std::unique_ptr<PollingControlProxy> m_pStem4VuMeter;
    std::unique_ptr<PollingControlProxy> m_pStemCount;
};

// Keep the focused ordering checks active without enabling the broad stem
// audio/reference suite below. The readiness observer is a synthetic writer;
// it does not model ControllerManager input or incident causality.
class StemFirstLoadControlFixture : public BaseSignalPathTest,
                                    public ::testing::WithParamInterface<StemParam> {
  public:
    QString getGroupForStem(QStringView deckGroup, int stemNr) {
        DEBUG_ASSERT(deckGroup.endsWith(QChar(']')) && stemNr <= 4);
        return deckGroup.chopped(1) + QStringLiteral("_Stem") + QChar('0' + stemNr) + QChar(']');
    }

    QString GetStemFilePath() {
        const auto& [codec, info] = GetParam();
        return getTestDir().filePath(getTestDir()
                        .filePath("stems/%1/%2_%3.stem.mp4")
                        .arg(info.dir,
                                info.title,
                                QString::fromStdString(codec)));
    }

    void SetUp() override {
        BaseSignalPathTest::SetUp();
        QObject::connect(m_pMixerDeck2.get(),
                &BaseTrackPlayerImpl::loadingTrack,
                &m_signalObserver,
                [this](TrackPointer pNewTrack, TrackPointer pOldTrack) {
                    if (m_playerLifecycleEvents.size() < 64) {
                        m_playerLifecycleEvents.append(
                                QStringLiteral("loadingTrack new=%1 old=%2")
                                        .arg(trackPointerAddress(pNewTrack),
                                                trackPointerAddress(pOldTrack)));
                    }
                });
        QObject::connect(m_pMixerDeck2.get(),
                &BaseTrackPlayerImpl::newTrackLoaded,
                &m_signalObserver,
                [this](TrackPointer pTrack) {
                    if (m_playerLifecycleEvents.size() < 64) {
                        m_playerLifecycleEvents.append(
                                QStringLiteral("newTrackLoaded track=%1")
                                        .arg(trackPointerAddress(pTrack)));
                    }
                });
        for (int stemIdx = 1; stemIdx <= 4; ++stemIdx) {
            ChannelHandleAndGroup sourceStemHandleGroup =
                    m_pEngineMixer->registerChannelGroup(
                            getGroupForStem(m_sGroup1, stemIdx));
            m_pChannel1->addStemHandle(sourceStemHandleGroup);
            m_pEffectsManager->addStem(sourceStemHandleGroup);

            ChannelHandleAndGroup stemHandleGroup =
                    m_pEngineMixer->registerChannelGroup(
                            getGroupForStem(m_sGroup2, stemIdx));
            m_pChannel2->addStemHandle(stemHandleGroup);
            m_pEffectsManager->addStem(stemHandleGroup);
        }
    }

    std::string playerLifecycleDiagnostics() const {
        return m_playerLifecycleEvents.isEmpty()
                ? QStringLiteral("no player track signals observed").toStdString()
                : m_playerLifecycleEvents.join(QStringLiteral("; ")).toStdString();
    }

    bool waitForTrackLoaded(
            Deck* pDeck,
            TrackPointer pTrack,
            bool requestLoad,
            QString* diagnostics = nullptr) {
        TrackPointer pLoadedTrack;
        PollingControlProxy stemCount(pDeck->getGroup(), "stem_count");
        EngineBuffer* pBuffer = pDeck->getEngineDeck()->getEngineBuffer();
        QMetaObject::Connection connection = QObject::connect(pDeck,
                &BaseTrackPlayerImpl::newTrackLoaded,
                [&pLoadedTrack]( // clazy:exclude=lambda-in-connect
                        TrackPointer pNewTrack) { pLoadedTrack = pNewTrack; });
        if (requestLoad) {
            BaseSignalPathTest::loadTrack(pDeck, pTrack);
        }

        // Event-poll counts are not elapsed-time bounds: processEvents may
        // return immediately when idle. Give the reader a real deadline.
        QElapsedTimer loadDeadline;
        loadDeadline.start();
        while (loadDeadline.elapsed() < 10000) {
            if (pLoadedTrack == pTrack && pBuffer->isTrackLoaded() &&
                    stemCount.get() == pTrack->getStemInfo().size()) {
                break;
            }
            QTest::qWait(1);
        }
        QObject::disconnect(connection);
        const bool loadedSignalMatches = pLoadedTrack == pTrack;
        bool engineBufferLoadedChecked = false;
        bool engineBufferLoaded = false;
        bool stemCountChecked = false;
        bool stemCountMatches = false;
        bool ready = loadedSignalMatches;
        if (ready) {
            engineBufferLoadedChecked = true;
            engineBufferLoaded = pBuffer->isTrackLoaded();
            ready = engineBufferLoaded;
        }
        if (ready) {
            stemCountChecked = true;
            stemCountMatches = stemCount.get() == pTrack->getStemInfo().size();
            ready = stemCountMatches;
        }
        if (diagnostics) {
            const auto predicateResult = [](bool checked, bool value) {
                if (!checked) {
                    return QStringLiteral("not checked");
                }
                return value ? QStringLiteral("true") : QStringLiteral("false");
            };
            *diagnostics = QStringLiteral(
                    "loadedSignalMatches=%1 engineBufferLoaded=%2 stemCountMatches=%3; %4")
                                   .arg(loadedSignalMatches ? QStringLiteral("true")
                                                            : QStringLiteral("false"))
                                   .arg(predicateResult(
                                           engineBufferLoadedChecked,
                                           engineBufferLoaded))
                                   .arg(predicateResult(stemCountChecked, stemCountMatches))
                                   .arg(QString::fromStdString(playerLifecycleDiagnostics()));
        }
        return ready;
    }

    QObject m_signalObserver;
    QStringList m_playerLifecycleEvents;
};

// Keep worker scheduling regressions to one representative stem asset. The
// ordering tests above still cover the codec and stem-file matrix.
class StemFirstLoadSchedulingFixture : public StemFirstLoadControlFixture {};

TEST_P(StemControlFixture, StemCount) {
    EXPECT_EQ(m_pStemCount->get(), 4.0);

    QString kTrackLocationTest = getTestDir().filePath(QStringLiteral("sine-30.wav"));
    TrackPointer pTrack(Track::newTemporary(kTrackLocationTest));
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStemCount->get(), 0.0);

    auto sourceStemPath = GetStemFilePath();
    kTrackLocationTest = getTestDir().filePath(sourceStemPath);
    pTrack = Track::newTemporary(kTrackLocationTest);
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStemCount->get(), 4.0);
}

TEST_P(StemControlFixture, StemColor) {
    auto sourceStemPath = GetStemFilePath();
    EXPECT_EQ(m_pStem1Color->get(), 0xfd << 16 | 0x4a << 8 | 0x4a);
    EXPECT_EQ(m_pStem2Color->get(), 0xff << 16 | 0xff << 8 | 0x00);
    EXPECT_EQ(m_pStem3Color->get(), 0x00 << 16 | 0xe8 << 8 | 0xe8);
    EXPECT_EQ(m_pStem4Color->get(), 0xad << 16 | 0x65 << 8 | 0xff);

    QString kTrackLocationTest = getTestDir().filePath(QStringLiteral("sine-30.wav"));
    TrackPointer pTrack(Track::newTemporary(kTrackLocationTest));
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1Color->get(), -1.0);
    EXPECT_EQ(m_pStem2Color->get(), -1.0);
    EXPECT_EQ(m_pStem3Color->get(), -1.0);
    EXPECT_EQ(m_pStem4Color->get(), -1.0);

    kTrackLocationTest = getTestDir().filePath(sourceStemPath);
    pTrack = Track::newTemporary(kTrackLocationTest);
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1Color->get(), 0xfd << 16 | 0x4a << 8 | 0x4a);
    EXPECT_EQ(m_pStem2Color->get(), 0xff << 16 | 0xff << 8 | 0x00);
    EXPECT_EQ(m_pStem3Color->get(), 0x00 << 16 | 0xe8 << 8 | 0xe8);
    EXPECT_EQ(m_pStem4Color->get(), 0xad << 16 | 0x65 << 8 | 0xff);
}

TEST_P(StemControlFixture, Volume) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    m_pStem1Volume->set(0.0);
    m_pStem2Volume->set(0.0);
    m_pStem3Volume->set(0.0);
    m_pStem4Volume->set(0.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlSilence"));

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem1Volume->set(1.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlDrumOnly"));

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem2Volume->set(0.8);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlDrumAndBass"));

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem1Volume->set(0.5);
    m_pStem3Volume->set(0.2);
    m_pStem4Volume->set(0.4);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlFull"));
}

TEST_P(StemControlFixture, VolumeResetOnLoad) {
    m_pStem1Volume->set(0.1);
    m_pStem2Volume->set(0.2);
    m_pStem3Volume->set(0.3);
    m_pStem4Volume->set(0.4);
    m_pStem1Mute->set(1.0);
    m_pStem2Mute->set(1.0);
    m_pStem3Mute->set(0.0);
    m_pStem4Mute->set(1.0);
    m_pConfig->setValue(
            ConfigKey("[Mixer Profile]", "stem_auto_reset"), false);

    QString kTrackLocationTest = getTestDir().filePath(QStringLiteral("sine-30.wav"));
    TrackPointer pTrack(Track::newTemporary(kTrackLocationTest));
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1Volume->get(), 0.1);
    EXPECT_EQ(m_pStem2Volume->get(), 0.2);
    EXPECT_EQ(m_pStem3Volume->get(), 0.3);
    EXPECT_EQ(m_pStem4Volume->get(), 0.4);
    EXPECT_EQ(m_pStem1Mute->get(), 1.0);
    EXPECT_EQ(m_pStem2Mute->get(), 1.0);
    EXPECT_EQ(m_pStem3Mute->get(), 0.0);
    EXPECT_EQ(m_pStem4Mute->get(), 1.0);

    m_pConfig->setValue(
            ConfigKey("[Mixer Profile]", "stem_auto_reset"), true);
    loadTrack(m_pMixerDeck1.get(), pTrack);

    EXPECT_EQ(m_pStem1Volume->get(), 1.0);
    EXPECT_EQ(m_pStem2Volume->get(), 1.0);
    EXPECT_EQ(m_pStem3Volume->get(), 1.0);
    EXPECT_EQ(m_pStem4Volume->get(), 1.0);
    EXPECT_EQ(m_pStem1Mute->get(), 0.0);
    EXPECT_EQ(m_pStem2Mute->get(), 0.0);
    EXPECT_EQ(m_pStem3Mute->get(), 0.0);
    EXPECT_EQ(m_pStem4Mute->get(), 0.0);
}

TEST_P(StemControlFixture, Mute) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);
    m_pStem1Mute->set(1.0);
    m_pStem2Mute->set(1.0);
    m_pStem3Mute->set(1.0);
    m_pStem4Mute->set(1.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlSilence")); // Same than volume test

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem1Mute->set(0.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemVolumeControlDrumOnly")); // Same than volume test

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem2Mute->set(0.0);

    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemMuteControlDrumAndBass"));

    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pStem3Mute->set(0.0);
    m_pStem4Mute->set(0.0);

    // We need to allow a bigger bigger delta than the 0.0001 default
    // to cover the difference of different AAC decoder.
    // aac and libfdk_aac have a difference of 0.00017 in tests with FFmpeg 4.4.2
    double acceptableDelta = 0.0002; // -74 dB
    // Proceed the buffer a first time to proceed the ramping gain
    m_pEngineMixer->process(kProcessBufferSize);
    m_pEngineMixer->process(kProcessBufferSize);
    assertBufferMatchesReference(m_pEngineMixer->getMainBuffer(),
            QStringLiteral("StemMuteControlFull"),
            acceptableDelta);
}

TEST_P(StemControlFixture, VuMeter) {
    m_pChannel1->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
    m_pPlay->set(1.0);

    // Initial check: silence
    EXPECT_EQ(m_pStem1VuMeter->get(), 0.0);

    // Process buffer to play sound
    // Run enough cycles to trigger VU meter update (30Hz update rate vs ~44kHz/buffer)
    for (int i = 0; i < 50; ++i) {
        m_pEngineMixer->process(kProcessBufferSize);
    }

    // Check if VU meters picked up the signal
    EXPECT_GT(m_pStem1VuMeter->get(), 0.0);
    EXPECT_GT(m_pStem2VuMeter->get(), 0.0);

    // Mute Stem 1
    m_pStem1Mute->set(1.0);

    // Process enough buffers to allow VU meter to decay to 0
    // Decay is exponential, so it takes time.
    for (int i = 0; i < 600; ++i) {
        m_pEngineMixer->process(kProcessBufferSize);
    }

    // VU Meter should be near zero (allow small epsilon for imperfect decay)
    EXPECT_NEAR(m_pStem1VuMeter->get(), 0.0, 0.001);

    // Stem 2 should still be playing
    EXPECT_GT(m_pStem2VuMeter->get(), 0.0);
}

TEST_P(StemFirstLoadControlFixture, ResetCompletesBeforeReadinessWrite) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, true);

    PollingControlProxy stemMute(getGroupForStem(m_sGroup2, 1), "mute");
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    ASSERT_EQ(trackLoaded.get(), 0.0);
    stemMute.set(1.0);

    bool readinessObserved = false;
    double muteBeforeReadinessWrite = -1.0;
    QObject trackLoadedObserver;
    ASSERT_TRUE(trackLoaded.connectValueChanged(
            &trackLoadedObserver,
            [&stemMute, &readinessObserved, &muteBeforeReadinessWrite](double value) {
                if (value <= 0.0) {
                    return;
                }
                muteBeforeReadinessWrite = stemMute.get();
                stemMute.set(1.0);
                readinessObserved = true;
            },
            Qt::DirectConnection));

    TrackPointer pStemFile(Track::newTemporary(GetStemFilePath()));
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pStemFile, true))
            << "timed out waiting for newTrackLoaded";
    ASSERT_TRUE(readinessObserved);
    EXPECT_EQ(muteBeforeReadinessWrite, 0.0);
    EXPECT_EQ(stemMute.get(), 1.0);
}

TEST_P(StemFirstLoadControlFixture, DisabledAutoResetLeavesReadyMuteForWriter) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, false);

    PollingControlProxy stemMute(getGroupForStem(m_sGroup2, 1), "mute");
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    ASSERT_EQ(trackLoaded.get(), 0.0);
    stemMute.set(1.0);

    bool readinessObserved = false;
    double muteBeforeReadinessWrite = -1.0;
    QObject trackLoadedObserver;
    ASSERT_TRUE(trackLoaded.connectValueChanged(
            &trackLoadedObserver,
            [&stemMute, &readinessObserved, &muteBeforeReadinessWrite](double value) {
                if (value <= 0.0) {
                    return;
                }
                muteBeforeReadinessWrite = stemMute.get();
                stemMute.set(0.0);
                readinessObserved = true;
            },
            Qt::DirectConnection));

    TrackPointer pStemFile(Track::newTemporary(GetStemFilePath()));
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pStemFile, true))
            << "timed out waiting for newTrackLoaded";
    ASSERT_TRUE(readinessObserved);
    EXPECT_EQ(muteBeforeReadinessWrite, 1.0);
    EXPECT_EQ(stemMute.get(), 0.0);
}

TEST_P(StemFirstLoadControlFixture, CloneStemControlsSurviveReadinessResetBoundary) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, true);

    TrackPointer pStemFile(Track::newTemporary(GetStemFilePath()));
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck1.get(), pStemFile, true))
            << "timed out waiting for source deck load";

    PollingControlProxy sourceGain(getGroupForStem(m_sGroup1, 1), "volume");
    PollingControlProxy sourceMute(getGroupForStem(m_sGroup1, 1), "mute");
    PollingControlProxy targetGain(getGroupForStem(m_sGroup2, 1), "volume");
    PollingControlProxy targetMute(getGroupForStem(m_sGroup2, 1), "mute");
    sourceGain.set(0.25);
    sourceMute.set(1.0);

    m_pMixerDeck2->slotCloneFromGroup(m_sGroup1);
    ProcessBuffer();
    QString loadDiagnostics;
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pStemFile, false, &loadDiagnostics))
            << "timed out waiting for cloned track load: " << loadDiagnostics.toStdString();
    EXPECT_EQ(targetGain.get(), 0.25);
    EXPECT_EQ(targetMute.get(), 1.0);
}

TEST_P(StemFirstLoadControlFixture, StaleReadyCallbackDoesNotResetOrPublish) {
    EngineBuffer* pBuffer = m_pChannel2->getEngineBuffer();
    const quint64 staleGeneration = pBuffer->beginTrackLoad();
    pBuffer->beginTrackLoad();

    PollingControlProxy stemMute(getGroupForStem(m_sGroup2, 1), "mute");
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    stemMute.set(1.0);
    ASSERT_EQ(trackLoaded.get(), 0.0);

    ASSERT_TRUE(QMetaObject::invokeMethod(pBuffer,
            "slotPublishTrackLoaded",
            Qt::QueuedConnection,
            Q_ARG(TrackPointer, TrackPointer()),
            Q_ARG(TrackPointer, TrackPointer()),
            Q_ARG(quint64, staleGeneration)));
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    EXPECT_EQ(trackLoaded.get(), 0.0);
    EXPECT_EQ(stemMute.get(), 1.0);
}

TEST_P(StemFirstLoadControlFixture, StaleWorkerSuccessAfterEjectIsDiscarded) {
    EngineBuffer* pBuffer = m_pChannel2->getEngineBuffer();
    const quint64 staleGeneration = pBuffer->beginTrackLoad();

    // This advances the generation before clearing EngineBuffer state. A
    // delayed reader success carrying the older token must not repopulate the
    // current track after the eject returns.
    pBuffer->ejectTrack();
    ASSERT_FALSE(pBuffer->isTrackLoaded());

    TrackPointer pLateTrack(Track::newTemporary(GetStemFilePath()));
    const mixxx::audio::SampleRate sampleRate = pLateTrack->getSampleRate();
    const mixxx::audio::ChannelCount channelCount = pLateTrack->getChannels();
    const mixxx::audio::FramePos frameCount =
            mixxx::audio::FramePos::fromEngineSamplePos(
                    sampleRate * pLateTrack->getDuration());
    bool callbackInvoked = false;
    std::thread readerCallback([&] {
        callbackInvoked = QMetaObject::invokeMethod(pBuffer,
                "slotReaderTrackLoaded",
                Qt::DirectConnection,
                Q_ARG(TrackPointer, pLateTrack),
                Q_ARG(mixxx::audio::SampleRate, sampleRate),
                Q_ARG(mixxx::audio::ChannelCount, channelCount),
                Q_ARG(mixxx::audio::FramePos, frameCount),
                Q_ARG(quint64, staleGeneration));
    });
    readerCallback.join();
    ASSERT_TRUE(callbackInvoked);

    EXPECT_FALSE(pBuffer->isTrackLoaded());
    EXPECT_FALSE(pBuffer->getLoadedTrack());
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    EXPECT_EQ(trackLoaded.get(), 0.0);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

TEST_P(StemFirstLoadSchedulingFixture, EjectRejectsDequeuedLateStartAndNullUnload) {
    EngineBuffer* pBuffer = m_pChannel2->getEngineBuffer();
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    PollingControlProxy play(m_sGroup2, "play");
    ASSERT_FALSE(pBuffer->isTrackLoaded());
    ASSERT_FALSE(pBuffer->isTrackLoadingForTest());
    ASSERT_EQ(trackLoaded.get(), 0.0);

    ScopedReaderWorkerLifecycleHook workerHook(
            m_sGroup2,
            ScopedReaderWorkerLifecycleHook::PauseAt::FirstRequestDequeued);
    TrackPointer pLateTrack(Track::newTemporary(GetStemFilePath()));
    m_pMixerDeck2->slotLoadTrack(
            pLateTrack, mixxx::StemChannelSelection(), true);
    ProcessBuffer();
    ASSERT_TRUE(workerHook.waitForFirstRequestDequeued())
            << "reader worker did not dequeue the first request"
            << "\n"
            << workerHook.diagnostics()
            << "\n"
            << playerLifecycleDiagnostics()
            << "\n"
            << describeSafeLoadEpochState(m_pMixerDeck2.get(), pLateTrack);

    pBuffer->ejectTrack();
    ProcessBuffer();
    ASSERT_FALSE(pBuffer->isTrackLoaded());
    ASSERT_FALSE(pBuffer->isTrackLoadingForTest());
    ASSERT_FALSE(pBuffer->getLoadedTrack());
    ASSERT_EQ(trackLoaded.get(), 0.0);
    ASSERT_EQ(play.get(), 0.0);
    ASSERT_FALSE(pBuffer->getTrackEndPosition().isValid());

    workerHook.release();
    ASSERT_TRUE(workerHook.waitForNullTrackUnloaded())
            << "reader worker did not finish the queued null unload";
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    EXPECT_FALSE(pBuffer->isTrackLoaded());
    EXPECT_FALSE(pBuffer->isTrackLoadingForTest());
    EXPECT_FALSE(pBuffer->getLoadedTrack());
    EXPECT_EQ(trackLoaded.get(), 0.0);
    EXPECT_EQ(play.get(), 0.0);
    EXPECT_FALSE(pBuffer->getTrackEndPosition().isValid());

    TrackPointer pNextTrack(Track::newTemporary(GetStemFilePath()));
    m_pMixerDeck2->slotLoadTrack(
            pNextTrack, mixxx::StemChannelSelection(), false);
    ProcessBuffer();
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pNextTrack, false))
            << "a valid request after the stale start did not become ready";
    EXPECT_TRUE(pBuffer->isTrackLoaded());
    EXPECT_FALSE(pBuffer->isTrackLoadingForTest());
    EXPECT_EQ(trackLoaded.get(), 1.0);
}

TEST_P(StemFirstLoadSchedulingFixture, ResetObserverSupersedesWithClonedRequest) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, true);

    EngineBuffer* pBuffer = m_pChannel2->getEngineBuffer();
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    ControlProxy targetGain(getGroupForStem(m_sGroup2, 1), "volume");
    PollingControlProxy targetMute(getGroupForStem(m_sGroup2, 1), "mute");
    PollingControlProxy sourceGain(getGroupForStem(m_sGroup1, 1), "volume");
    PollingControlProxy sourceMute(getGroupForStem(m_sGroup1, 1), "mute");
    TrackPointer pCloneTrack(Track::newTemporary(GetStemFilePath()));
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck1.get(), pCloneTrack, true))
            << "source track did not load before its controls were read";
    ASSERT_EQ(trackLoaded.get(), 0.0);

    sourceGain.set(0.37);
    sourceMute.set(1.0);
    targetGain.set(0.2);
    targetMute.set(0.0);

    ScopedReaderWorkerLifecycleHook workerHook(
            m_sGroup2,
            ScopedReaderWorkerLifecycleHook::PauseAt::SecondStartAccepted);
    bool cloneRequested = false;
    bool secondStartAccepted = false;
    QObject targetGainObserver;
    ASSERT_TRUE(targetGain.connectValueChanged(
            &targetGainObserver,
            [this, &workerHook, &cloneRequested, &secondStartAccepted](double value) {
                if (value < 0.999 || cloneRequested) {
                    return;
                }
                cloneRequested = true;
                m_pMixerDeck2->slotCloneFromGroup(m_sGroup1);
                ProcessBuffer();
                secondStartAccepted = workerHook.waitForSecondStartAccepted();
            },
            Qt::DirectConnection));

    TrackPointer pFirstTrack(Track::newTemporary(GetStemFilePath()));
    m_pMixerDeck2->slotLoadTrack(
            pFirstTrack, mixxx::StemChannelSelection(), false);
    ProcessBuffer();
    QElapsedTimer cloneDeadline;
    cloneDeadline.start();
    while (!secondStartAccepted && cloneDeadline.elapsed() < 10000) {
        QTest::qWait(1);
    }

    ASSERT_TRUE(cloneRequested)
            << "stem reset observer did not request a clone"
            << "\n"
            << workerHook.diagnostics()
            << "\n"
            << playerLifecycleDiagnostics()
            << "\n"
            << describeSafeLoadEpochState(m_pMixerDeck2.get(), pFirstTrack);
    ASSERT_TRUE(secondStartAccepted)
            << "cloned request did not reach the accepted reader start"
            << "\n"
            << workerHook.diagnostics()
            << "\n"
            << playerLifecycleDiagnostics()
            << "\n"
            << describeSafeLoadEpochState(m_pMixerDeck2.get(), pCloneTrack);
    EXPECT_EQ(trackLoaded.get(), 0.0)
            << "superseded request published readiness";
    EXPECT_TRUE(pBuffer->isTrackLoadingForTest())
            << "superseded request released the cloned request's loading gate";

    workerHook.release();
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pCloneTrack, false))
            << "cloned replacement request did not become ready";
    EXPECT_EQ(targetGain.get(), 0.37);
    EXPECT_EQ(targetMute.get(), 1.0);
    EXPECT_EQ(trackLoaded.get(), 1.0);
    EXPECT_FALSE(pBuffer->isTrackLoadingForTest());
}

TEST_P(StemFirstLoadSchedulingFixture, CloneObserverSupersedesDuringCloneCopy) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, true);

    TrackPointer pSourceTrack(Track::newTemporary(GetStemFilePath()));
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck1.get(), pSourceTrack, true))
            << "source track did not load before its controls were cloned";

    ControlProxy targetGain1(getGroupForStem(m_sGroup2, 1), "volume");
    PollingControlProxy targetMute1(getGroupForStem(m_sGroup2, 1), "mute");
    ControlProxy targetGain2(getGroupForStem(m_sGroup2, 2), "volume");
    PollingControlProxy targetMute2(getGroupForStem(m_sGroup2, 2), "mute");
    PollingControlProxy sourceGain1(getGroupForStem(m_sGroup1, 1), "volume");
    PollingControlProxy sourceMute1(getGroupForStem(m_sGroup1, 1), "mute");
    PollingControlProxy sourceGain2(getGroupForStem(m_sGroup1, 2), "volume");
    PollingControlProxy sourceMute2(getGroupForStem(m_sGroup1, 2), "mute");
    sourceGain1.set(0.37);
    sourceMute1.set(1.0);
    sourceGain2.set(0.42);
    sourceMute2.set(1.0);
    targetGain1.set(0.2);
    targetMute1.set(0.0);
    targetGain2.set(0.2);
    targetMute2.set(0.0);

    ScopedReaderWorkerLifecycleHook workerHook(
            m_sGroup2,
            ScopedReaderWorkerLifecycleHook::PauseAt::FirstStartAccepted);
    bool cloneRequested = false;
    bool startAccepted = false;
    int selectedStemsNotifications = 0;
    std::vector<TrackPointer> loadingTracks;
    QObject observer;
    QObject signalContext;
    ASSERT_TRUE(targetGain1.connectValueChanged(
            &observer,
            [this,
                    &workerHook,
                    &cloneRequested,
                    &startAccepted,
                    &sourceGain2,
                    &sourceMute2](double value) {
                if (value < 0.3 || cloneRequested) {
                    return;
                }
                cloneRequested = true;
                // This nested request copies the complete source state. The
                // later source edit makes any stale outer-copy continuation
                // visible in stem 2.
                m_pMixerDeck2->slotCloneFromGroup(m_sGroup1);
                sourceGain2.set(0.66);
                sourceMute2.set(0.0);
                ProcessBuffer();
                startAccepted = workerHook.waitForFirstStartAccepted();
            },
            Qt::DirectConnection));
    QObject::connect(
            m_pMixerDeck2.get(),
            &BaseTrackPlayerImpl::loadingTrack,
            &signalContext,
            [&loadingTracks](TrackPointer pNewTrack, TrackPointer) {
                loadingTracks.push_back(std::move(pNewTrack));
            },
            Qt::DirectConnection);
    QObject::connect(
            m_pMixerDeck2.get(),
            &BaseTrackPlayerImpl::selectedStems,
            &signalContext,
            [&selectedStemsNotifications](mixxx::StemChannelSelection) {
                ++selectedStemsNotifications;
            },
            Qt::DirectConnection);

    m_pMixerDeck2->slotCloneFromGroup(m_sGroup1);

    ASSERT_TRUE(cloneRequested)
            << "clone-copy observer did not start a newer request";
    ASSERT_TRUE(startAccepted)
            << "replacement clone did not reach the accepted reader start"
            << "\n"
            << workerHook.diagnostics()
            << "\n"
            << playerLifecycleDiagnostics()
            << "\n"
            << describeSafeLoadEpochState(m_pMixerDeck2.get(), pSourceTrack);
    ASSERT_EQ(loadingTracks.size(), 1u)
            << "stale request emitted loading metadata after the replacement";
    EXPECT_EQ(loadingTracks.front(), pSourceTrack);
    EXPECT_EQ(selectedStemsNotifications, 1)
            << "stale request published a stem selection after the replacement";

    workerHook.release();
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pSourceTrack, false))
            << "replacement clone did not become ready";
    EXPECT_EQ(targetGain1.get(), 0.37);
    EXPECT_EQ(targetMute1.get(), 1.0);
    EXPECT_EQ(targetGain2.get(), 0.42);
    EXPECT_EQ(targetMute2.get(), 1.0);
    EXPECT_EQ(m_pMixerDeck2->getLoadedTrack(), pSourceTrack);
}

TEST_P(StemFirstLoadSchedulingFixture, UnloadControlObserverSupersedesRequest) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, true);

    TrackPointer pOldTrack(Track::newTemporary(GetStemFilePath()));
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pOldTrack, true))
            << "initial track did not load before replacing it";

    ControlProxy targetStemColor(getGroupForStem(m_sGroup2, 1), "color");
    targetStemColor.set(0.25);
    ScopedReaderWorkerLifecycleHook workerHook(
            m_sGroup2,
            ScopedReaderWorkerLifecycleHook::PauseAt::FirstStartAccepted);
    bool replacementRequested = false;
    bool startAccepted = false;
    std::vector<TrackPointer> loadingTracks;
    QObject observer;
    QObject signalContext;
    TrackPointer pReplacementTrack(Track::newTemporary(GetStemFilePath()));
    ASSERT_TRUE(targetStemColor.connectValueChanged(
            &observer,
            [this, &workerHook, &replacementRequested, &startAccepted, pReplacementTrack](double) {
                if (replacementRequested) {
                    return;
                }
                replacementRequested = true;
                m_pMixerDeck2->slotLoadTrack(
                        pReplacementTrack, mixxx::StemChannelSelection(), false);
                ProcessBuffer();
                startAccepted = workerHook.waitForFirstStartAccepted();
            },
            Qt::DirectConnection));
    QObject::connect(
            m_pMixerDeck2.get(),
            &BaseTrackPlayerImpl::loadingTrack,
            &signalContext,
            [&loadingTracks](TrackPointer pNewTrack, TrackPointer) {
                loadingTracks.push_back(std::move(pNewTrack));
            },
            Qt::DirectConnection);

    TrackPointer pSupersededTrack(Track::newTemporary(GetStemFilePath()));
    m_pMixerDeck2->slotLoadTrack(
            pSupersededTrack, mixxx::StemChannelSelection(), false);

    ASSERT_TRUE(replacementRequested)
            << "unload control observer did not start a newer request";
    ASSERT_TRUE(startAccepted)
            << "replacement request did not reach the accepted reader start"
            << "\n"
            << workerHook.diagnostics()
            << "\n"
            << playerLifecycleDiagnostics()
            << "\n"
            << describeSafeLoadEpochState(m_pMixerDeck2.get(), pReplacementTrack);
    ASSERT_EQ(loadingTracks.size(), 1u)
            << "superseded request emitted loading metadata after the replacement";
    EXPECT_EQ(loadingTracks.front(), pReplacementTrack);

    workerHook.release();
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pReplacementTrack, false))
            << "replacement request did not become ready";
    EXPECT_EQ(m_pMixerDeck2->getLoadedTrack(), pReplacementTrack);
}

TEST_P(StemFirstLoadSchedulingFixture, NullRequestSelectionRespectsGeneration) {
    EngineBuffer* pBuffer = m_pChannel2->getEngineBuffer();
    int selectedStemsNotifications = 0;
    QObject signalContext;
    QObject::connect(
            m_pMixerDeck2.get(),
            &BaseTrackPlayerImpl::selectedStems,
            &signalContext,
            [&selectedStemsNotifications](mixxx::StemChannelSelection) {
                ++selectedStemsNotifications;
            },
            Qt::DirectConnection);

    // A null request with no old track keeps the existing selection event.
    m_pMixerDeck2->slotLoadTrack(
            TrackPointer(), mixxx::StemChannelSelection(), false);
    EXPECT_EQ(selectedStemsNotifications, 1);
    EXPECT_FALSE(pBuffer->getLoadedTrack());
    EXPECT_FALSE(pBuffer->isTrackLoaded());

    TrackPointer pOldTrack(Track::newTemporary(GetStemFilePath()));
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pOldTrack, true))
            << "track did not load before the superseded null request";
    selectedStemsNotifications = 0;

    ScopedReaderWorkerLifecycleHook workerHook(
            m_sGroup2,
            ScopedReaderWorkerLifecycleHook::PauseAt::FirstStartAccepted);
    bool replacementRequested = false;
    bool startAccepted = false;
    TrackPointer pReplacementTrack(Track::newTemporary(GetStemFilePath()));
    QObject requestObserver;
    QObject::connect(
            pBuffer,
            &EngineBuffer::trackLoaded,
            &requestObserver,
            [this,
                    &workerHook,
                    &replacementRequested,
                    &startAccepted,
                    pReplacementTrack](
                    TrackPointer pNewTrack, TrackPointer pUnloadedTrack) {
                if (pNewTrack || !pUnloadedTrack || replacementRequested) {
                    return;
                }
                replacementRequested = true;
                m_pMixerDeck2->slotLoadTrack(
                        pReplacementTrack,
                        mixxx::StemChannelSelection(),
                        false);
                ProcessBuffer();
                startAccepted = workerHook.waitForFirstStartAccepted();
            },
            Qt::DirectConnection);

    m_pMixerDeck2->slotLoadTrack(
            TrackPointer(), mixxx::StemChannelSelection(), false);

    ASSERT_TRUE(replacementRequested)
            << "legacy eject completion did not start the replacement request";
    ASSERT_TRUE(startAccepted)
            << "replacement request did not reach the accepted reader start"
            << "\n"
            << workerHook.diagnostics()
            << "\n"
            << playerLifecycleDiagnostics()
            << "\n"
            << describeSafeLoadEpochState(m_pMixerDeck2.get(), pReplacementTrack);
    EXPECT_EQ(selectedStemsNotifications, 1)
            << "superseded null request emitted selectedStems after replacement";
    EXPECT_EQ(m_pMixerDeck2->getLoadedTrack(), pReplacementTrack);
    EXPECT_TRUE(pBuffer->isTrackLoadingForTest())
            << "superseded null eject cleared the replacement loading gate";

    workerHook.release();
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pReplacementTrack, false))
            << "replacement request did not become ready";
    EXPECT_FALSE(pBuffer->isTrackLoadingForTest());
}

TEST_P(StemFirstLoadSchedulingFixture, ReadinessObserverCannotReleaseNewerLoadingGate) {
    EngineBuffer* pBuffer = m_pChannel2->getEngineBuffer();
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    ASSERT_EQ(trackLoaded.get(), 0.0);

    ScopedReaderWorkerLifecycleHook workerHook(
            m_sGroup2,
            ScopedReaderWorkerLifecycleHook::PauseAt::SecondStartAccepted);
    TrackPointer pFirstTrack(Track::newTemporary(GetStemFilePath()));
    TrackPointer pSecondTrack(Track::newTemporary(GetStemFilePath()));
    bool readinessObserverRan = false;
    bool secondStartAccepted = false;
    bool publisherReturned = false;
    bool loadingAtPublisherReturn = false;
    QObject publicationCheck;
    QObject trackLoadedObserver;
    ASSERT_TRUE(trackLoaded.connectValueChanged(
            &trackLoadedObserver,
            [this,
                    pBuffer,
                    pSecondTrack,
                    &workerHook,
                    &readinessObserverRan,
                    &secondStartAccepted,
                    &publisherReturned,
                    &loadingAtPublisherReturn,
                    &publicationCheck](double value) {
                if (value <= 0.0 || readinessObserverRan) {
                    return;
                }
                readinessObserverRan = true;
                m_pMixerDeck2->slotLoadTrack(
                        pSecondTrack, mixxx::StemChannelSelection(), false);
                ProcessBuffer();
                secondStartAccepted = workerHook.waitForSecondStartAccepted();
                QTimer::singleShot(0,
                        &publicationCheck,
                        [pBuffer,
                                &publisherReturned,
                                &loadingAtPublisherReturn] {
                            loadingAtPublisherReturn =
                                    pBuffer->isTrackLoadingForTest();
                            publisherReturned = true;
                        });
            },
            Qt::DirectConnection));

    m_pMixerDeck2->slotLoadTrack(
            pFirstTrack, mixxx::StemChannelSelection(), false);
    ProcessBuffer();
    QElapsedTimer publicationDeadline;
    publicationDeadline.start();
    while (!publisherReturned && publicationDeadline.elapsed() < 10000) {
        QTest::qWait(1);
    }

    ASSERT_TRUE(readinessObserverRan)
            << "readiness observer did not run for the first request"
            << "\n"
            << workerHook.diagnostics()
            << "\n"
            << playerLifecycleDiagnostics()
            << "\n"
            << describeSafeLoadEpochState(m_pMixerDeck2.get(), pFirstTrack);
    ASSERT_TRUE(secondStartAccepted)
            << "new request did not start before the first publisher returned";
    ASSERT_TRUE(publisherReturned)
            << "first publication did not return to the GUI event loop";
    EXPECT_TRUE(loadingAtPublisherReturn)
            << "old publication cleared the newer request's loading flag";

    workerHook.release();
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pSecondTrack, false))
            << "new request did not become ready after its start was released";
    EXPECT_TRUE(pBuffer->isTrackLoaded());
    EXPECT_FALSE(pBuffer->isTrackLoadingForTest());
}

TEST_P(StemFirstLoadSchedulingFixture, AutoResetSettingChangedDuringLoadIsSampledAtPublication) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, false);

    PollingControlProxy stemMute(getGroupForStem(m_sGroup2, 1), "mute");
    stemMute.set(1.0);
    ScopedReaderWorkerLifecycleHook workerHook(
            m_sGroup2,
            ScopedReaderWorkerLifecycleHook::PauseAt::FirstStartAccepted);
    TrackPointer pStemFile(Track::newTemporary(GetStemFilePath()));
    m_pMixerDeck2->slotLoadTrack(
            pStemFile, mixxx::StemChannelSelection(), false);
    ProcessBuffer();
    ASSERT_TRUE(workerHook.waitForFirstStartAccepted())
            << "reader worker did not accept the request start"
            << "\n"
            << workerHook.diagnostics()
            << "\n"
            << playerLifecycleDiagnostics()
            << "\n"
            << describeSafeLoadEpochState(m_pMixerDeck2.get(), pStemFile);

    m_pConfig->setValue(autoResetKey, true);
    stemMute.set(1.0);
    workerHook.release();
    ASSERT_TRUE(waitForTrackLoaded(m_pMixerDeck2.get(), pStemFile, false))
            << "request did not become ready after the setting change";
    EXPECT_EQ(stemMute.get(), 0.0);
}

TEST_P(StemFirstLoadSchedulingFixture, LegacyEjectAndFakeTrackKeepStemCleanupRoutes) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, true);

    EngineBuffer* pBuffer = m_pChannel2->getEngineBuffer();
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    PollingControlProxy stemCount(m_sGroup2, "stem_count");
    PollingControlProxy stemMute(getGroupForStem(m_sGroup2, 1), "mute");
    ASSERT_FALSE(pBuffer->getLoadedTrack());
    stemMute.set(1.0);

    // An eject with no engine track has no legacy trackLoaded completion and
    // therefore must not reset the target deck's stem controls.
    pBuffer->ejectTrack();
    EXPECT_EQ(trackLoaded.get(), 0.0);
    EXPECT_EQ(stemMute.get(), 1.0);

    TrackPointer pFakeTrack(Track::newTemporary(
            getTestDir().filePath(QStringLiteral("sine-30.wav"))));
    pBuffer->loadFakeTrack(pFakeTrack, false);
    EXPECT_TRUE(pBuffer->isTrackLoaded());
    EXPECT_EQ(trackLoaded.get(), 1.0);
    EXPECT_EQ(stemMute.get(), 0.0);
    EXPECT_EQ(stemCount.get(), 0.0);

    pBuffer->ejectTrack();
    EXPECT_FALSE(pBuffer->isTrackLoaded());
    EXPECT_EQ(trackLoaded.get(), 0.0);
    EXPECT_EQ(stemMute.get(), 0.0);
    EXPECT_EQ(stemCount.get(), 0.0);
}

TEST_P(StemFirstLoadSchedulingFixture, CurrentFailureKeepsOldAndNoOldCleanupBehavior) {
    const ConfigKey autoResetKey("[Mixer Profile]", "stem_auto_reset");
    StemAutoResetConfigGuard restoreAutoReset{
            m_pConfig, m_pConfig->getValue(autoResetKey, true)};
    m_pConfig->setValue(autoResetKey, true);

    EngineBuffer* pBuffer = m_pChannel2->getEngineBuffer();
    ControlProxy trackLoaded(m_sGroup2, "track_loaded");
    PollingControlProxy stemCount(m_sGroup2, "stem_count");
    PollingControlProxy stemMute(getGroupForStem(m_sGroup2, 1), "mute");
    TrackPointer pOldTrack(Track::newTemporary(GetStemFilePath()));
    m_pMixerDeck2->slotLoadTrack(
            pOldTrack, mixxx::StemChannelSelection(), false);
    ProcessBuffer();
    QString oldTrackLoadDiagnostics;
    ASSERT_TRUE(waitForTrackLoaded(
            m_pMixerDeck2.get(), pOldTrack, false, &oldTrackLoadDiagnostics))
            << "old track did not load: " << oldTrackLoadDiagnostics.toStdString();
    stemMute.set(1.0);

    // Suppress only the user-facing failure dialog route. The legacy track
    // completion from eject remains connected to EngineDeck and is exercised.
    QObject::disconnect(pBuffer,
            &EngineBuffer::trackLoadFailed,
            m_pMixerDeck2.get(),
            &BaseTrackPlayerImpl::slotLoadFailed);
    auto failFromWorker = [](EngineBuffer* pTargetBuffer, TrackPointer pFailedTrack) {
        bool invoked = false;
        std::thread worker([&] {
            invoked = QMetaObject::invokeMethod(
                    pTargetBuffer,
                    "slotTrackLoadFailed",
                    Qt::DirectConnection,
                    Q_ARG(TrackPointer, pFailedTrack),
                    Q_ARG(QString, QStringLiteral("test failure")));
        });
        worker.join();
        return invoked;
    };

    TrackPointer pFailedTrack(Track::newTemporary(
            getTestDir().filePath(QStringLiteral("missing-stem-track.mp4"))));
    ASSERT_TRUE(failFromWorker(pBuffer, pFailedTrack));
    QElapsedTimer failureDeadline;
    failureDeadline.start();
    while (stemMute.get() != 0.0 && failureDeadline.elapsed() < 10000) {
        QTest::qWait(1);
    }
    EXPECT_FALSE(pBuffer->isTrackLoaded());
    EXPECT_FALSE(pBuffer->isTrackLoadingForTest());
    EXPECT_EQ(trackLoaded.get(), 0.0);
    EXPECT_EQ(stemMute.get(), 0.0)
            << "failure with an old engine track must use legacy reset cleanup";
    EXPECT_EQ(stemCount.get(), 0.0);

    stemMute.set(1.0);
    ASSERT_TRUE(failFromWorker(pBuffer, pFailedTrack));
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_FALSE(pBuffer->isTrackLoaded());
    EXPECT_FALSE(pBuffer->isTrackLoadingForTest());
    EXPECT_EQ(trackLoaded.get(), 0.0);
    EXPECT_EQ(stemMute.get(), 1.0)
            << "failure with no old engine track must not synthesize legacy reset";
    EXPECT_EQ(stemCount.get(), 0.0);

    QObject::connect(pBuffer,
            &EngineBuffer::trackLoadFailed,
            m_pMixerDeck2.get(),
            &BaseTrackPlayerImpl::slotLoadFailed);
}

INSTANTIATE_TEST_SUITE_P(
        DISABLED_StemControlTest,
        StemControlFixture,
        ::testing::Combine(
                ::testing::ValuesIn(supportedCodecs),
                ::testing::ValuesIn(kStemFileInfos)),
        [](const testing::TestParamInfo<StemControlFixture::ParamType>& info) {
            return std::get<0>(info.param) + "_" +
                    std::get<1>(info.param).title.toStdString();
        });

INSTANTIATE_TEST_SUITE_P(
        StemFirstLoadControlTest,
        StemFirstLoadControlFixture,
        ::testing::Combine(
                ::testing::ValuesIn(supportedCodecs),
                ::testing::ValuesIn(kStemFileInfos)),
        [](const testing::TestParamInfo<StemFirstLoadControlFixture::ParamType>& info) {
            return std::get<0>(info.param) + "_" +
                    std::get<1>(info.param).title.toStdString();
        });

INSTANTIATE_TEST_SUITE_P(
        StemFirstLoadSchedulingTest,
        StemFirstLoadSchedulingFixture,
        ::testing::Values(StemParam{supportedCodecs.front(), kStemFileInfos.front()}),
        [](const testing::TestParamInfo<StemFirstLoadSchedulingFixture::ParamType>& info) {
            return std::get<0>(info.param) + "_" +
                    std::get<1>(info.param).title.toStdString();
        });
