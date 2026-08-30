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

    std::unique_ptr<QObject> loadInterface() {
        QQmlComponent component(
                &m_engine,
                QUrl::fromLocalFile(QStringLiteral(
                        RESOURCE_FOLDER "/qml/Settings/Interface.qml")));
        std::unique_ptr<QObject> root(component.create());
        EXPECT_FALSE(component.isError()) << qPrintable(component.errorString());
        EXPECT_TRUE(root) << qPrintable(component.errorString());
        return root;
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

    QQmlEngine m_engine;
};

TEST_F(InterfaceQmlTest, ResetAndCancelSaveTheDisplayedMaxZoomOutValue) {
    config()->setValue(kMaxZoomOutKey, 20.0);
    auto root = loadInterface();
    ASSERT_NE(nullptr, root);
    QObject* maxZoomOutInput = findMaxZoomOutInput(root.get());
    ASSERT_NE(nullptr, maxZoomOutInput);

    maxZoomOutInput->setProperty("value", 25);
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "resetWaveform"));
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "saveWaveform"));
    EXPECT_DOUBLE_EQ(10.0, config()->getValue(kMaxZoomOutKey, -1.0));
    EXPECT_EQ(10, maxZoomOutInput->property("value").toInt());

    config()->setValue(kMaxZoomOutKey, 20.0);
    maxZoomOutInput->setProperty("value", 25);
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "loadWaveform"));
    ASSERT_TRUE(QMetaObject::invokeMethod(root.get(), "saveWaveform"));
    EXPECT_DOUBLE_EQ(20.0, config()->getValue(kMaxZoomOutKey, -1.0));
    EXPECT_EQ(20, maxZoomOutInput->property("value").toInt());
}
} // namespace
