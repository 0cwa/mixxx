#include "preferences/settingsmanager.h"

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

EngineBuffer::KeylockEngine defaultKeylockEngine() {
#ifdef __BUNGEE__
    return EngineBuffer::KeylockEngine::Bungee;
#elif defined(__RUBBERBAND__)
    return EngineBuffer::KeylockEngine::RubberBandFaster;
#else
    return EngineBuffer::KeylockEngine::SoundTouch;
#endif
}

} // namespace

class SettingsManagerTest : public MixxxTest {};

TEST_F(SettingsManagerTest, SeedsGlobalKeylockEngineForFreshSettingsDirectory) {
    QTemporaryDir profileParent;
    ASSERT_TRUE(profileParent.isValid());

    const QString settingsPath = QDir(profileParent.path()).filePath("fresh-profile");
    ASSERT_FALSE(QDir(settingsPath).exists());

    SettingsManager manager(settingsPath);

    ASSERT_TRUE(manager.settings()->exists(kKeylockEngineKey));
    EXPECT_EQ(static_cast<int>(defaultKeylockEngine()),
            manager.settings()->getValue(kKeylockEngineKey, -1));
}

TEST_F(SettingsManagerTest, SeedsGlobalKeylockEngineForExistingSettingsDirectory) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());

    SettingsManager manager(settingsDir.path());

    ASSERT_TRUE(manager.settings()->exists(kKeylockEngineKey));
    EXPECT_EQ(static_cast<int>(defaultKeylockEngine()),
            manager.settings()->getValue(kKeylockEngineKey, -1));
}

TEST_F(SettingsManagerTest, PreservesExplicitGlobalKeylockEngine) {
    QTemporaryDir settingsDir;
    ASSERT_TRUE(settingsDir.isValid());

    UserSettings existingSettings(QDir(settingsDir.path()).filePath(MIXXX_SETTINGS_FILE));
    existingSettings.setValue(kKeylockEngineKey, EngineBuffer::KeylockEngine::SoundTouch);
    ASSERT_TRUE(existingSettings.save());

    SettingsManager manager(settingsDir.path());

    ASSERT_TRUE(manager.settings()->exists(kKeylockEngineKey));
    EXPECT_EQ(static_cast<int>(EngineBuffer::KeylockEngine::SoundTouch),
            manager.settings()->getValue(kKeylockEngineKey, -1));
}
