#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <gsl/pointers>
#include <memory>

#include "control/controlindicatortimer.h"
#include "effects/effectsmanager.h"
#include "engine/channelhandle.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "qml/qmlconfigproxy.h"
#include "qml/qmlcontrolproxy.h"
#include "qml/qmllibraryproxy.h"
#include "qml/qmlplayermanagerproxy.h"
#include "recording/recordingmanager.h"
#include "soundio/soundmanager.h"
#include "test/mixxxdbtest.h"
#include "track/track.h"

namespace {
const ConfigKey kMaxZoomOutKey(QStringLiteral("[Waveform]"),
        QStringLiteral("MaxZoomOut"));

class InterfaceQmlTest : public MixxxDbTest {
  protected:
    InterfaceQmlTest()
            : MixxxDbTest(true) {
    }
    void SetUp() override {
        mixxx::qml::QmlConfigProxy::registerUserSettings(config());
        m_engine.addImportPath(QStringLiteral(RESOURCE_FOLDER "/qml"));
        m_engine.addImportPath(
                QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("qml")));
    }

    void TearDown() override {
        m_root.reset();
        m_engine.clearSingletons();
        mixxx::qml::QmlLibraryProxy::registerLibrary(nullptr);
        mixxx::qml::QmlPlayerManagerProxy::registerPlayerManager(nullptr);
        if (m_testLibrary) {
            m_testLibrary->stopPendingTasks();
        }
        m_testSoundManager.reset();
        m_testPlayerManager.reset();
        m_testLibrary.reset();
        m_testRecordingManager.reset();
        m_testEngineMixer.reset();
        m_testEffectsManager.reset();
        m_testTrackCollectionManager.reset();
        m_testControlIndicatorTimer.reset();
        if (m_libraryCachesCreated) {
            CoverArtCache::destroy();
            PlayerInfo::destroy();
        }
    }

    void initializePlayerManager() {
        if (m_testPlayerManager) {
            return;
        }
        m_testChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_testEffectsManager = std::make_unique<EffectsManager>(
                config(), m_testChannelHandleFactory);
        m_testEngineMixer = std::make_unique<EngineMixer>(config(),
                QStringLiteral("[Master]"),
                m_testEffectsManager.get(),
                m_testChannelHandleFactory,
                false);
        m_testSoundManager = std::make_unique<SoundManager>(config(), m_testEngineMixer.get());
        m_testControlIndicatorTimer = std::make_unique<mixxx::ControlIndicatorTimer>();
        m_testEngineMixer->registerNonEngineChannelSoundIO(
                gsl::make_not_null(m_testSoundManager.get()));
        m_testPlayerManager = std::make_shared<PlayerManager>(
                config(),
                m_testSoundManager.get(),
                m_testEffectsManager.get(),
                m_testEngineMixer.get());
        m_testPlayerManager->addConfiguredDecks();
        m_testEffectsManager->setup();
        mixxx::qml::QmlPlayerManagerProxy::registerPlayerManager(m_testPlayerManager);
    }

    QObject* loadWaveformSettings() {
        initializePlayerManager();
        if (!MixxxDb::initDatabaseSchema(dbConnection())) {
            ADD_FAILURE() << "Failed to initialize the isolated waveform settings database";
            return nullptr;
        }
        CoverArtCache::createInstance();
        PlayerInfo::create();
        m_libraryCachesCreated = true;
        m_testTrackCollectionManager = std::make_unique<TrackCollectionManager>(
                nullptr,
                config(),
                dbConnectionPooler(),
                [](Track* pTrack) { delete pTrack; });
        m_testRecordingManager = std::make_unique<RecordingManager>(
                config(), m_testEngineMixer.get());
        m_testLibrary = std::make_shared<Library>(nullptr,
                config(),
                dbConnectionPooler(),
                m_testTrackCollectionManager.get(),
                m_testPlayerManager.get(),
                m_testRecordingManager.get());
        mixxx::qml::QmlLibraryProxy::registerLibrary(m_testLibrary);

        QQmlComponent component(&m_engine);
        component.setData(R"(
import QtQuick
import Mixxx 1.0 as Mixxx
import "Settings" as Settings

Item {
    property var configProxy: Mixxx.Config

    Mixxx.SettingParameterManager {
        Settings.Waveform {
            objectName: "waveformSettings"
        }
    }
}
)",
                QUrl::fromLocalFile(QStringLiteral(
                        RESOURCE_FOLDER "/qml/interfaceqml_test.qml")));

        m_root.reset(component.create());
        EXPECT_FALSE(component.isError()) << qPrintable(component.errorString());
        EXPECT_TRUE(m_root) << qPrintable(component.errorString());
        if (!m_root) {
            return nullptr;
        }
        return m_root->findChild<QObject*>(QStringLiteral("waveformSettings"));
    }

    QObject* loadWaveformDisplay() {
        initializePlayerManager();

        QQmlComponent component(&m_engine);
        const QString componentData =
                QStringLiteral(R"(
import QtQuick
import Mixxx 1.0 as Mixxx

Item {
    property var configProxy: Mixxx.Config

    Loader {
        id: waveformDisplayLoader
    }
    Component.onCompleted: waveformDisplayLoader.setSource(
        "%1",
        { group: "[Channel1]", objectName: "waveformDisplay" })
}
)")
                        .arg(QUrl::fromLocalFile(
                                QStringLiteral(RESOURCE_FOLDER
                                        "/qml/WaveformDisplay.qml"))
                                        .toString());
        component.setData(componentData.toUtf8(),
                QUrl::fromLocalFile(QStringLiteral(
                        RESOURCE_FOLDER "/qml/main.qml")));

        m_root.reset(component.create());
        EXPECT_FALSE(component.isError()) << qPrintable(component.errorString());
        EXPECT_TRUE(m_root) << qPrintable(component.errorString());
        if (!m_root) {
            return nullptr;
        }
        application()->processEvents();
        return m_root->findChild<QObject*>(QStringLiteral("waveformDisplay"));
    }

    static QObject* findMaxZoomOutInput(QObject* root) {
        const auto children = root->findChildren<QObject*>();
        for (QObject* child : children) {
            if (child->property("suffix").toString() == QStringLiteral("x") &&
                    child->property("min").toDouble() == 10.0 &&
                    child->property("max").toDouble() == 100.0) {
                return child;
            }
        }
        return nullptr;
    }

    static QObject* findButton(QObject* root, const QString& text) {
        const auto children = root->findChildren<QObject*>();
        for (QObject* child : children) {
            if (child->property("text").toString() == text) {
                return child;
            }
        }
        return nullptr;
    }

    static bool pressButton(QObject* root, const QString& text) {
        QObject* button = findButton(root, text);
        if (!button) {
            return false;
        }
        return QMetaObject::invokeMethod(button, "pressed");
    }

    bool m_libraryCachesCreated = false;
    std::unique_ptr<TrackCollectionManager> m_testTrackCollectionManager;
    std::unique_ptr<RecordingManager> m_testRecordingManager;
    std::shared_ptr<Library> m_testLibrary;
    std::unique_ptr<mixxx::ControlIndicatorTimer> m_testControlIndicatorTimer;
    std::shared_ptr<ChannelHandleFactory> m_testChannelHandleFactory;
    std::unique_ptr<EffectsManager> m_testEffectsManager;
    std::unique_ptr<EngineMixer> m_testEngineMixer;
    std::unique_ptr<SoundManager> m_testSoundManager;
    std::shared_ptr<PlayerManager> m_testPlayerManager;
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_root;
};

TEST_F(InterfaceQmlTest, EditResetCancelAndSaveKeepMaxZoomOutSynchronized) {
    config()->setValue(kMaxZoomOutKey, 20.0);
    auto root = loadWaveformSettings();
    ASSERT_NE(nullptr, root);
    QObject* maxZoomOutInput = findMaxZoomOutInput(root);
    ASSERT_NE(nullptr, maxZoomOutInput);

    application()->processEvents();
    EXPECT_DOUBLE_EQ(20.0, maxZoomOutInput->property("value").toDouble());

    maxZoomOutInput->setProperty("value", 25);
    EXPECT_DOUBLE_EQ(25.0, maxZoomOutInput->property("value").toDouble());
    ASSERT_TRUE(pressButton(root, QStringLiteral("Reset")));
    EXPECT_DOUBLE_EQ(10.0, maxZoomOutInput->property("value").toDouble());
    ASSERT_TRUE(pressButton(root, QStringLiteral("Save")));
    EXPECT_FALSE(config()->exists(kMaxZoomOutKey));
    EXPECT_DOUBLE_EQ(10.0, config()->getValue(kMaxZoomOutKey, 10.0));

    config()->setValue(kMaxZoomOutKey, 20.0);
    maxZoomOutInput->setProperty("value", 25);
    ASSERT_TRUE(pressButton(root, QStringLiteral("Cancel")));
    EXPECT_DOUBLE_EQ(20.0, maxZoomOutInput->property("value").toDouble());
    ASSERT_TRUE(pressButton(root, QStringLiteral("Save")));
    EXPECT_DOUBLE_EQ(20.0, config()->getValue(kMaxZoomOutKey, -1.0));

    maxZoomOutInput->setProperty("value", 25);
    ASSERT_TRUE(pressButton(root, QStringLiteral("Save")));
    EXPECT_DOUBLE_EQ(25.0, config()->getValue(kMaxZoomOutKey, -1.0));
    EXPECT_DOUBLE_EQ(25.0, maxZoomOutInput->property("value").toDouble());
}

TEST_F(InterfaceQmlTest, LoweringMaxZoomOutReclampsExistingWaveformDisplay) {
    QObject* waveformDisplay = loadWaveformDisplay();
    ASSERT_NE(nullptr, waveformDisplay);
    QObject* zoomControl = waveformDisplay->property("zoomControlProxy").value<QObject*>();
    ASSERT_NE(nullptr, zoomControl);
    auto* zoomControlProxy = qobject_cast<mixxx::qml::QmlControlProxy*>(zoomControl);
    ASSERT_NE(nullptr, zoomControlProxy);
    EXPECT_EQ(QStringLiteral("waveform_zoom"), zoomControlProxy->getKey());
    EXPECT_EQ(QStringLiteral("[Channel1]"), zoomControlProxy->getGroup());
    EXPECT_TRUE(zoomControlProxy->isInitialized());

    QObject* configProxy = m_root->property("configProxy").value<QObject*>();
    ASSERT_NE(nullptr, configProxy);
    configProxy->setProperty("waveformMaxZoomOut", 100.0);
    application()->processEvents();

    zoomControl->setProperty("value", 50.0);
    EXPECT_DOUBLE_EQ(50.0, zoomControl->property("value").toDouble());

    configProxy->setProperty("waveformMaxZoomOut", 20.0);
    application()->processEvents();
    EXPECT_DOUBLE_EQ(20.0, zoomControl->property("value").toDouble());

    m_root.reset();
}
} // namespace
