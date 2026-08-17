#include "preferences/settingsmanager.h"

#include <array>

#include <QDir>
#include <QTemporaryDir>

#include "config.h"
#include "engine/enginebuffer.h"
#include "preferences/usersettings.h"
#include "test/mixxxtest.h"

namespace {

const ConfigKey kKeylockEngineKey(
        QStringLiteral("[App]"),
        QStringLiteral("keylock_engine"));

constexpr std::array<const char*, 4> kDeckGroups = {
        "[Channel1]",
        "[Channel2]",
        "[Channel3]",
        "[Channel4]",
};

ConfigKey deckKeylockEngineKey(const char* group) {
    return ConfigKey(
            QString::fromLatin1(group),
            QStringLiteral("keylock_engine"));
}

EngineBuffer::KeylockEngine defaultStableKeylockEngine() {
#ifdef __RUBBERBAND__
    return EngineBuffer::KeylockEngine::RubberBandFaster;
#else
    return EngineBuffer::KeylockEngine::SoundTouch;
#endif
}

std::array<EngineBuffer::KeylockEngine, 4> explicitPerDeckKeylockEngines() {
#ifdef __RUBBERBAND__
    return {
            EngineBuffer::KeylockEngine::SoundTouch,
            EngineBuffer::KeylockEngine::RubberBandFaster,
            EngineBuffer::KeylockEngine::RubberBandFiner,
            EngineBuffer::KeylockEngine::SoundTouch,
    };
#else
    return {
            EngineBuffer::KeylockEngine::SoundTouch,
            EngineBuffer::KeylockEngine::SoundTouch,
            EngineBuffer::KeylockEngine::SoundTouch,
            EngineBuffer::KeylockEngine::SoundTouch,
    };
#endif
}

} // namespace

class SettingsManagerTest : public MixxxTest {};

TEST_F(SettingsManagerTest, SeedsPerDeckKeylockEnginesForFreshSettingsDirectory) {
    QTemporaryDir profileParent;
    ASSERT_TRUE(profileParent.isValid());

    const QString settingsPath = QDir(profileParent.path()).filePath("fresh-profile");
    ASSERT_FALSE(QDir(settingsPath).exists());

    SettingsManager manager(settingsPath);

    EXPECT_TRUE(QDir(settingsPath).exists());
    for (const char* group : kDeckGroups) {
        const ConfigKey key = deckKeylockEngineKey(group);
        ASSERT_TRUE(manager.settings()->exists(key));
        EXPECT_EQ(static_cast<int>(defaultStableKeylockEngine()),
                manager.settings()->getValue(key, -1))
                << group;
    }
}

TEST_F(SettingsManagerTest, SeedsPerDeckKeylockEnginesForExistingSettingsDirectory) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    ASSERT_TRUE(QDir(settingsDir.path()).exists());

    SettingsManager manager(settingsDir.path());

    EXPECT_FALSE(manager.settings()->exists(kKeylockEngineKey));
    for (const char* group : kDeckGroups) {
        const ConfigKey key = deckKeylockEngineKey(group);
        ASSERT_TRUE(manager.settings()->exists(key));
        EXPECT_EQ(static_cast<int>(defaultStableKeylockEngine()),
                manager.settings()->getValue(key, -1))
                << group;
    }
}

TEST_F(SettingsManagerTest, PreservesExplicitPerDeckKeylockEnginesInExistingSettingsDirectory) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());

    UserSettings existingSettings(QDir(settingsDir.path()).filePath(MIXXX_SETTINGS_FILE));
    const auto perDeckKeylockEngines = explicitPerDeckKeylockEngines();
    for (size_t i = 0; i < kDeckGroups.size(); ++i) {
        existingSettings.setValue(
                deckKeylockEngineKey(kDeckGroups[i]),
                perDeckKeylockEngines[i]);
    }
    ASSERT_TRUE(existingSettings.save());

    SettingsManager manager(settingsDir.path());

    for (size_t i = 0; i < kDeckGroups.size(); ++i) {
        const ConfigKey key = deckKeylockEngineKey(kDeckGroups[i]);
        ASSERT_TRUE(manager.settings()->exists(key));
        EXPECT_EQ(static_cast<int>(perDeckKeylockEngines[i]),
                manager.settings()->getValue(key, -1))
                << kDeckGroups[i];
    }
}

#ifdef __BUNGEE__
TEST_F(SettingsManagerTest, PreservesBungeePerDeckKeylockEngineDuringMigration) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());

    UserSettings existingSettings(QDir(settingsDir.path()).filePath(MIXXX_SETTINGS_FILE));
    const ConfigKey key = deckKeylockEngineKey("[Channel1]");
    existingSettings.setValue(key, EngineBuffer::KeylockEngine::Bungee);
    ASSERT_TRUE(existingSettings.save());

    SettingsManager manager(settingsDir.path());

    ASSERT_TRUE(manager.settings()->exists(key));
    EXPECT_EQ(static_cast<int>(EngineBuffer::KeylockEngine::Bungee),
            manager.settings()->getValue(key, -1));
}
#endif

#ifdef __SIGNALSMITH__
TEST_F(SettingsManagerTest, PreservesSignalSmithPerDeckKeylockEngineDuringMigration) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());

    UserSettings existingSettings(QDir(settingsDir.path()).filePath(MIXXX_SETTINGS_FILE));
    const ConfigKey key = deckKeylockEngineKey("[Channel2]");
    existingSettings.setValue(key, EngineBuffer::KeylockEngine::SignalSmith);
    ASSERT_TRUE(existingSettings.save());

    SettingsManager manager(settingsDir.path());

    ASSERT_TRUE(manager.settings()->exists(key));
    EXPECT_EQ(static_cast<int>(EngineBuffer::KeylockEngine::SignalSmith),
            manager.settings()->getValue(key, -1));
}
#endif
