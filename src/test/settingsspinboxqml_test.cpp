#include <gtest/gtest.h>

#include <QDoubleValidator>
#include <QJSValue>
#include <QLocale>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>
#include <memory>

namespace {

class SettingsSpinBoxQmlTest : public ::testing::Test {
  protected:
    void SetUp() override {
        QLocale::setDefault(QLocale::c());
        m_pEngine = std::make_unique<QQmlEngine>();
    }

    void TearDown() override {
        m_pEngine.reset();
        QLocale::setDefault(m_previousLocale);
    }

    std::unique_ptr<QObject> createSpinBox(double min, double max, int precision) {
        QQmlComponent component(m_pEngine.get(),
                QUrl::fromLocalFile(QStringLiteral(
                        RESOURCE_FOLDER "/qml/Settings/SpinBox.qml")));
        EXPECT_FALSE(component.isError()) << qPrintable(component.errorString());
        auto pRoot = std::unique_ptr<QObject>(component.createWithInitialProperties({
                {QStringLiteral("min"), min},
                {QStringLiteral("max"), max},
                {QStringLiteral("precision"), precision},
                {QStringLiteral("realValue"), min},
                {QStringLiteral("locale"), QVariant::fromValue(QLocale::c())},
        }));
        EXPECT_TRUE(pRoot) << qPrintable(component.errorString());
        return pRoot;
    }

    QDoubleValidator* validator(QObject* pRoot) {
        return qobject_cast<QDoubleValidator*>(
                pRoot->property("validator").value<QObject*>());
    }

    QValidator::State validate(QDoubleValidator* pValidator, const char* text) {
        QString input = QString::fromLatin1(text);
        int position = input.size();
        return pValidator->validate(input, position);
    }

    QJSValue convert(QObject* pRoot, const char* functionName, QJSValue value) {
        const auto object = m_pEngine->newQObject(pRoot);
        const auto function = object.property(QString::fromLatin1(functionName));
        EXPECT_TRUE(function.isCallable());
        const auto result = function.callWithInstance(object,
                {value, object.property(QStringLiteral("locale"))});
        EXPECT_FALSE(result.isError()) << qPrintable(result.toString());
        return result;
    }

  private:
    const QLocale m_previousLocale;
    std::unique_ptr<QQmlEngine> m_pEngine;
};

TEST_F(SettingsSpinBoxQmlTest, DecimalValidatorUsesDisplayedUnits) {
    auto pRoot = createSpinBox(0.01, 10, 2);
    ASSERT_TRUE(pRoot);
    auto* pValidator = validator(pRoot.get());
    ASSERT_TRUE(pValidator);
    EXPECT_DOUBLE_EQ(pValidator->bottom(), 0.01);
    EXPECT_DOUBLE_EQ(pValidator->top(), 10);
    EXPECT_EQ(pValidator->decimals(), 2);

    for (const char* text : {"0.01", "0.05", "0.5", "10", "10.00"}) {
        SCOPED_TRACE(text);
        EXPECT_EQ(validate(pValidator, text), QValidator::Acceptable);
    }
    for (const char* text : {"0",
                 "0.00",
                 "-0.01",
                 "10.01",
                 "11",
                 "100",
                 "0.001",
                 "0.005",
                 "0.051",
                 "1.234",
                 "1e0"}) {
        SCOPED_TRACE(text);
        EXPECT_NE(validate(pValidator, text), QValidator::Acceptable);
    }
}

TEST_F(SettingsSpinBoxQmlTest, DecimalTextConversionsAndArrowClampsAgree) {
    auto pRoot = createSpinBox(0.01, 10, 2);
    ASSERT_TRUE(pRoot);
    auto* pValidator = validator(pRoot.get());
    ASSERT_TRUE(pValidator);
    struct Entry {
        const char* input;
        int value;
        const char* display;
    };
    for (const auto& entry : {Entry{"0.01", 1, "0.01"},
                 Entry{"0.05", 5, "0.05"},
                 Entry{"0.5", 50, "0.50"},
                 Entry{"10", 1000, "10.00"}}) {
        SCOPED_TRACE(entry.input);
        ASSERT_EQ(validate(pValidator, entry.input), QValidator::Acceptable);
        const auto parsed = convert(pRoot.get(),
                "valueFromText",
                QJSValue(QString::fromLatin1(entry.input)));
        EXPECT_EQ(parsed.toInt(), entry.value);
        ASSERT_TRUE(pRoot->setProperty("value", parsed.toInt()));
        EXPECT_DOUBLE_EQ(pRoot->property("realValue").toDouble(), entry.value / 100.0);
        EXPECT_EQ(convert(pRoot.get(), "textFromValue", QJSValue(entry.value)).toString(),
                QString::fromLatin1(entry.display));
        EXPECT_EQ(pRoot->property("displayText").toString(), QString::fromLatin1(entry.display));
        ASSERT_TRUE(QMetaObject::invokeMethod(pRoot.get(), "increase"));
        EXPECT_EQ(pRoot->property("value").toInt(), entry.value == 1000 ? 1000 : entry.value + 1);
        ASSERT_TRUE(QMetaObject::invokeMethod(pRoot.get(), "decrease"));
        EXPECT_EQ(pRoot->property("value").toInt(), entry.value == 1000 ? 999 : entry.value);
    }
    ASSERT_TRUE(pRoot->setProperty("value", 1));
    ASSERT_TRUE(QMetaObject::invokeMethod(pRoot.get(), "decrease"));
    EXPECT_EQ(pRoot->property("value").toInt(), 1);
    EXPECT_DOUBLE_EQ(pRoot->property("realValue").toDouble(), 0.01);
}

TEST_F(SettingsSpinBoxQmlTest, PrecisionZeroKeepsIntegerValidationAndSteps) {
    auto pRoot = createSpinBox(0, 10, 0);
    ASSERT_TRUE(pRoot);
    auto* pValidator = validator(pRoot.get());
    ASSERT_TRUE(pValidator);
    EXPECT_DOUBLE_EQ(pValidator->bottom(), 0);
    EXPECT_DOUBLE_EQ(pValidator->top(), 10);
    EXPECT_EQ(pValidator->decimals(), 0);
    for (const char* text : {"0", "1", "10"}) {
        EXPECT_EQ(validate(pValidator, text), QValidator::Acceptable);
    }
    for (const char* text : {"-1", "11", "0.5", "1.0"}) {
        EXPECT_NE(validate(pValidator, text), QValidator::Acceptable);
    }
    ASSERT_TRUE(pRoot->setProperty("value", 1));
    EXPECT_EQ(convert(pRoot.get(), "textFromValue", QJSValue(1)).toString(), QStringLiteral("1"));
    ASSERT_TRUE(QMetaObject::invokeMethod(pRoot.get(), "increase"));
    EXPECT_DOUBLE_EQ(pRoot->property("realValue").toDouble(), 2);
    ASSERT_TRUE(QMetaObject::invokeMethod(pRoot.get(), "decrease"));
    EXPECT_DOUBLE_EQ(pRoot->property("realValue").toDouble(), 1);
}

TEST_F(SettingsSpinBoxQmlTest, ReversedBoundsAreSortedInDisplayedUnits) {
    auto pRoot = createSpinBox(10, 0.01, 2);
    ASSERT_TRUE(pRoot);
    auto* pValidator = validator(pRoot.get());
    ASSERT_TRUE(pValidator);
    EXPECT_DOUBLE_EQ(pValidator->bottom(), 0.01);
    EXPECT_DOUBLE_EQ(pValidator->top(), 10);
    EXPECT_EQ(validate(pValidator, "0.01"), QValidator::Acceptable);
    EXPECT_EQ(validate(pValidator, "10"), QValidator::Acceptable);
    EXPECT_NE(validate(pValidator, "10.01"), QValidator::Acceptable);
}

TEST_F(SettingsSpinBoxQmlTest, PrecisionThreeUsesItsOwnDecimalFactor) {
    auto pRoot = createSpinBox(0.001, 1, 3);
    ASSERT_TRUE(pRoot);
    auto* pValidator = validator(pRoot.get());
    ASSERT_TRUE(pValidator);
    EXPECT_DOUBLE_EQ(pValidator->bottom(), 0.001);
    EXPECT_DOUBLE_EQ(pValidator->top(), 1);
    EXPECT_EQ(pValidator->decimals(), 3);
    EXPECT_EQ(validate(pValidator, "0.001"), QValidator::Acceptable);
    EXPECT_EQ(validate(pValidator, "1.000"), QValidator::Acceptable);
    EXPECT_NE(validate(pValidator, "0.0001"), QValidator::Acceptable);
    EXPECT_NE(validate(pValidator, "1.001"), QValidator::Acceptable);
}

TEST_F(SettingsSpinBoxQmlTest, SignedBoundsRetainTheirDisplayedEndpoints) {
    auto pRoot = createSpinBox(-0.5, 1.5, 2);
    ASSERT_TRUE(pRoot);
    auto* pValidator = validator(pRoot.get());
    ASSERT_TRUE(pValidator);
    EXPECT_DOUBLE_EQ(pValidator->bottom(), -0.5);
    EXPECT_DOUBLE_EQ(pValidator->top(), 1.5);
    for (const char* text : {"-0.50", "0", "1.50"}) {
        EXPECT_EQ(validate(pValidator, text), QValidator::Acceptable);
    }
    for (const char* text : {"-0.51", "1.51"}) {
        EXPECT_NE(validate(pValidator, text), QValidator::Acceptable);
    }
}

} // namespace
