#include "preferences/settingsmanager.h"

#include <QDir>
#include <QTemporaryDir>

#include "config.h"
#include "engine/enginebuffer.h"
#include "preferences/usersettings.h"
#include "test/mixxxtest.h"

namespace {

const ConfigKey kChannel1KeylockEngineKey(
        QStringLiteral("[Channel1]"),
        QStringLiteral("keylock_engine"));
const ConfigKey kChannel2KeylockEngineKey(
        QStringLiteral("[Channel2]"),
        QStringLiteral("keylock_engine"));

EngineBuffer::KeylockEngine expectedStableEngine() {
#ifdef __RUBBERBAND__
    return EngineBuffer::KeylockEngine::RubberBandFaster;
#else
    return EngineBuffer::KeylockEngine::SoundTouch;
#endif
}

} // namespace

class SettingsManagerTest : public MixxxTest {};

TEST_F(SettingsManagerTest, SeedsStableKeylockEngineForFreshSettingsDirectory) {
    QTemporaryDir profileParent;
    ASSERT_TRUE(profileParent.isValid());

    const QString settingsPath = QDir(profileParent.path()).filePath("fresh-profile");
    ASSERT_FALSE(QDir(settingsPath).exists());

    SettingsManager manager(settingsPath);

    EXPECT_TRUE(QDir(settingsPath).exists());
    ASSERT_TRUE(manager.settings()->exists(kChannel1KeylockEngineKey));
    EXPECT_EQ(static_cast<int>(expectedStableEngine()),
            manager.settings()->getValue(kChannel1KeylockEngineKey, -1));
    ASSERT_TRUE(manager.settings()->exists(kChannel2KeylockEngineKey));
    EXPECT_EQ(static_cast<int>(expectedStableEngine()),
            manager.settings()->getValue(kChannel2KeylockEngineKey, -1));
}

TEST_F(SettingsManagerTest, SeedsMissingDeckKeylockEngineForExistingSettingsDirectory) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());
    ASSERT_TRUE(QDir(settingsDir.path()).exists());

    SettingsManager manager(settingsDir.path());

    EXPECT_EQ(static_cast<int>(expectedStableEngine()),
            manager.settings()->getValue(kChannel1KeylockEngineKey, -1));
}

TEST_F(SettingsManagerTest, PreservesExplicitGlobalEngineDuringDeckMigration) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());

    UserSettings existingSettings(QDir(settingsDir.path()).filePath(MIXXX_SETTINGS_FILE));
    const ConfigKey globalKeylockEngineKey(
            QStringLiteral("[App]"),
            QStringLiteral("keylock_engine"));
    existingSettings.setValue(
            globalKeylockEngineKey,
            EngineBuffer::KeylockEngine::SoundTouch);
    ASSERT_TRUE(existingSettings.save());

    SettingsManager manager(settingsDir.path());

    ASSERT_TRUE(manager.settings()->exists(kChannel1KeylockEngineKey));
    EXPECT_EQ(static_cast<int>(EngineBuffer::KeylockEngine::SoundTouch),
            manager.settings()->getValue(kChannel1KeylockEngineKey, -1));
}

TEST_F(SettingsManagerTest, PreservesExplicitDeckEngine) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());

    UserSettings existingSettings(QDir(settingsDir.path()).filePath(MIXXX_SETTINGS_FILE));
    existingSettings.setValue(
            kChannel1KeylockEngineKey,
            EngineBuffer::KeylockEngine::SoundTouch);
    ASSERT_TRUE(existingSettings.save());

    SettingsManager manager(settingsDir.path());

    EXPECT_EQ(static_cast<int>(EngineBuffer::KeylockEngine::SoundTouch),
            manager.settings()->getValue(kChannel1KeylockEngineKey, -1));
    EXPECT_EQ(static_cast<int>(expectedStableEngine()),
            manager.settings()->getValue(kChannel2KeylockEngineKey, -1));
}
