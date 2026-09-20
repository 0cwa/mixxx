#include "preferences/settingsmanager.h"

#include <QDir>
#include <QTemporaryDir>
#include <array>

#include "config.h"
#include "engine/enginebuffer.h"
#include "preferences/usersettings.h"
#include "test/mixxxtest.h"

namespace {

const ConfigKey kGlobalKeylockEngineKey(
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
    std::array<EngineBuffer::KeylockEngine, 4> engines = {
            EngineBuffer::KeylockEngine::SoundTouch,
            EngineBuffer::KeylockEngine::SoundTouch,
            EngineBuffer::KeylockEngine::SoundTouch,
            EngineBuffer::KeylockEngine::SoundTouch,
    };
#ifdef __RUBBERBAND__
    engines[1] = EngineBuffer::KeylockEngine::RubberBandFaster;
    engines[2] = EngineBuffer::KeylockEngine::RubberBandFiner;
#endif
#ifdef __BUNGEE__
    engines[0] = EngineBuffer::KeylockEngine::Bungee;
#endif
#ifdef __SIGNALSMITH__
    engines[3] = EngineBuffer::KeylockEngine::SignalSmith;
#endif
    return engines;
}

void expectGlobalKeylockEngineMigration(
        EngineBuffer::KeylockEngine globalEngine,
        EngineBuffer::KeylockEngine expectedEngine) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());

    UserSettings existingSettings(QDir(settingsDir.path()).filePath(MIXXX_SETTINGS_FILE));
    existingSettings.setValue(kGlobalKeylockEngineKey, globalEngine);
    ASSERT_TRUE(existingSettings.save());

    SettingsManager manager(settingsDir.path());

    for (const char* group : kDeckGroups) {
        const ConfigKey key = deckKeylockEngineKey(group);
        ASSERT_TRUE(manager.settings()->exists(key));
        EXPECT_EQ(static_cast<int>(expectedEngine),
                manager.settings()->getValue(key, -1))
                << group;
    }
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

    EXPECT_FALSE(manager.settings()->exists(kGlobalKeylockEngineKey));
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
    existingSettings.setValue(
            kGlobalKeylockEngineKey,
            EngineBuffer::KeylockEngine::SoundTouch);
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

TEST_F(SettingsManagerTest, MigratesValidGlobalKeylockEngineToPerDeckSettings) {
    expectGlobalKeylockEngineMigration(
            EngineBuffer::KeylockEngine::SoundTouch,
            EngineBuffer::KeylockEngine::SoundTouch);
}

TEST_F(SettingsManagerTest, RejectsInvalidGlobalKeylockEngineDuringMigration) {
    expectGlobalKeylockEngineMigration(
            static_cast<EngineBuffer::KeylockEngine>(-1),
            defaultStableKeylockEngine());
}

TEST_F(SettingsManagerTest, MigratesAvailableOptionalGlobalKeylockEngines) {
    bool testedEngine = false;
#ifdef __RUBBERBAND__
    if (EngineBuffer::isKeylockEngineAvailable(
                EngineBuffer::KeylockEngine::RubberBandR3ShortWindow)) {
        expectGlobalKeylockEngineMigration(
                EngineBuffer::KeylockEngine::RubberBandR3ShortWindow,
                EngineBuffer::KeylockEngine::RubberBandR3ShortWindow);
        testedEngine = true;
    }
#endif
#ifdef __BUNGEE__
    if (EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::Bungee)) {
        expectGlobalKeylockEngineMigration(
                EngineBuffer::KeylockEngine::Bungee,
                EngineBuffer::KeylockEngine::Bungee);
        testedEngine = true;
    }
#endif
#ifdef __SIGNALSMITH__
    if (EngineBuffer::isKeylockEngineAvailable(EngineBuffer::KeylockEngine::SignalSmith)) {
        expectGlobalKeylockEngineMigration(
                EngineBuffer::KeylockEngine::SignalSmith,
                EngineBuffer::KeylockEngine::SignalSmith);
        testedEngine = true;
    }
#endif
    if (!testedEngine) {
        GTEST_SKIP() << "No optional keylock engine is available";
    }
}

#ifdef __RUBBERBAND__
TEST_F(SettingsManagerTest, RejectsUnavailableRubberBandR3GlobalKeylockEngine) {
    const auto engine = EngineBuffer::KeylockEngine::RubberBandR3ShortWindow;
    if (EngineBuffer::isKeylockEngineAvailable(engine)) {
        GTEST_SKIP() << "RubberBand R3 is available";
    }
    expectGlobalKeylockEngineMigration(engine, defaultStableKeylockEngine());
}
#endif

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
