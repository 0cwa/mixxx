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
#include "mixer/playermanager.h"
#include "qml/qmlconfigproxy.h"
#include "qml/qmlcontrolproxy.h"
#include "qml/qmlplayermanagerproxy.h"
#include "soundio/soundmanager.h"
#include "test/mixxxtest.h"

namespace {
const ConfigKey kMaxZoomOutKey(QStringLiteral("[Waveform]"),
        QStringLiteral("MaxZoomOut"));

class InterfaceQmlTest : public MixxxTest {
  protected:
    void SetUp() override {
        mixxx::qml::QmlConfigProxy::registerUserSettings(config());
        m_engine.addImportPath(QStringLiteral(RESOURCE_FOLDER "/qml"));
        m_engine.addImportPath(
                QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("qml")));
    }

    void TearDown() override {
        m_root.reset();
        mixxx::qml::QmlPlayerManagerProxy::registerPlayerManager(nullptr);
    }

    QObject* loadInterface() {
        QQmlComponent component(&m_engine);
        component.setData(R"(
import QtQuick
import Mixxx 1.0 as Mixxx
import "Settings" as Settings

Item {
    property var configProxy: Mixxx.Config

    Mixxx.SettingParameterManager {
        Settings.Interface {
            objectName: "interface"
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
        return m_root->findChild<QObject*>(QStringLiteral("interface"));
    }

    QObject* loadWaveformDisplay() {
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

        QQmlComponent component(&m_engine);
        component.setData(R"(
import QtQuick
import Mixxx 1.0 as Mixxx
import "."

Item {
    property var configProxy: Mixxx.Config

    WaveformDisplay {
        objectName: "waveformDisplay"
        group: "[Channel1]"
    }
}
)",
                QUrl::fromLocalFile(QStringLiteral(
                        RESOURCE_FOLDER "/qml/waveformdisplayqml_test.qml")));

        m_root.reset(component.create());
        EXPECT_FALSE(component.isError()) << qPrintable(component.errorString());
        EXPECT_TRUE(m_root) << qPrintable(component.errorString());
        if (!m_root) {
            return nullptr;
        }
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
    auto root = loadInterface();
    ASSERT_NE(nullptr, root);
    QObject* maxZoomOutInput = findMaxZoomOutInput(root);
    ASSERT_NE(nullptr, maxZoomOutInput);

    root->setProperty("selectedIndex", 1);
    application()->processEvents();
    EXPECT_DOUBLE_EQ(20.0, maxZoomOutInput->property("realValue").toDouble());
    EXPECT_EQ(20, maxZoomOutInput->property("value").toInt());

    maxZoomOutInput->setProperty("value", 25);
    EXPECT_DOUBLE_EQ(25.0, maxZoomOutInput->property("realValue").toDouble());
    ASSERT_TRUE(pressButton(root, QStringLiteral("Reset")));
    EXPECT_DOUBLE_EQ(10.0, maxZoomOutInput->property("realValue").toDouble());
    EXPECT_EQ(10, maxZoomOutInput->property("value").toInt());
    ASSERT_TRUE(pressButton(root, QStringLiteral("Save")));
    EXPECT_FALSE(config()->exists(kMaxZoomOutKey));
    EXPECT_DOUBLE_EQ(10.0, config()->getValue(kMaxZoomOutKey, 10.0));

    config()->setValue(kMaxZoomOutKey, 20.0);
    maxZoomOutInput->setProperty("value", 25);
    ASSERT_TRUE(pressButton(root, QStringLiteral("Cancel")));
    EXPECT_DOUBLE_EQ(20.0, maxZoomOutInput->property("realValue").toDouble());
    EXPECT_EQ(20, maxZoomOutInput->property("value").toInt());
    ASSERT_TRUE(pressButton(root, QStringLiteral("Save")));
    EXPECT_DOUBLE_EQ(20.0, config()->getValue(kMaxZoomOutKey, -1.0));

    maxZoomOutInput->setProperty("value", 25);
    ASSERT_TRUE(pressButton(root, QStringLiteral("Save")));
    EXPECT_DOUBLE_EQ(25.0, config()->getValue(kMaxZoomOutKey, -1.0));
    EXPECT_EQ(25, maxZoomOutInput->property("value").toInt());
}

TEST_F(InterfaceQmlTest, LoweringMaxZoomOutReclampsExistingWaveformDisplay) {
    QObject* waveformDisplay = loadWaveformDisplay();
    ASSERT_NE(nullptr, waveformDisplay);
    QObject* zoomControl = waveformDisplay->findChild<QObject*>(
            QStringLiteral("waveformZoomControl"));
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
