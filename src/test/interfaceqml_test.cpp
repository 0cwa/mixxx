#include <gtest/gtest.h>

#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <memory>

#include "qml/qmlconfigproxy.h"
#include "test/mixxxtest.h"

namespace {
const ConfigKey kMaxZoomOutKey(QStringLiteral("[Waveform]"),
        QStringLiteral("MaxZoomOut"));

class InterfaceQmlTest : public MixxxTest {
  protected:
    void SetUp() override {
        mixxx::qml::QmlConfigProxy::registerUserSettings(config());
        m_engine.addImportPath(QStringLiteral(RESOURCE_FOLDER "/qml"));
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

    static QObject* findMaxZoomOutInput(QObject* root) {
        for (QObject* child : root->findChildren<QObject*>()) {
            if (child->property("suffix").toString() == QStringLiteral("x") &&
                    child->property("min").toDouble() == 10.0 &&
                    child->property("max").toDouble() == 100.0) {
                return child;
            }
        }
        return nullptr;
    }

    static QObject* findButton(QObject* root, const QString& text) {
        for (QObject* child : root->findChildren<QObject*>()) {
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
} // namespace
