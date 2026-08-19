#include "preferences/settingsmanager.h"

#include <QDir>

#include "control/control.h"
#include "engine/enginebuffer.h"
#include "preferences/upgrade.h"
#include "util/assert.h"

namespace {

const ConfigKey kGlobalKeylockEngineKey(
        QStringLiteral("[App]"),
        QStringLiteral("keylock_engine"));

EngineBuffer::KeylockEngine defaultStableKeylockEngine() {
#ifdef __BUNGEE__
    return EngineBuffer::KeylockEngine::Bungee;
#elif defined(__RUBBERBAND__)
    return EngineBuffer::KeylockEngine::RubberBandFaster;
#else
    return EngineBuffer::KeylockEngine::SoundTouch;
#endif
}

EngineBuffer::KeylockEngine defaultKeylockEngineForMigration(
        const UserSettingsPointer& pSettings) {
    if (pSettings->exists(kGlobalKeylockEngineKey)) {
        return pSettings->getValue<EngineBuffer::KeylockEngine>(
                kGlobalKeylockEngineKey,
                defaultStableKeylockEngine());
    }
    return defaultStableKeylockEngine();
}

void initializeGlobalKeylockEngine(const UserSettingsPointer& pSettings) {
    const auto keylockEngine = defaultKeylockEngineForMigration(pSettings);
    if (!pSettings->exists(kGlobalKeylockEngineKey)) {
        pSettings->setValue(kGlobalKeylockEngineKey, keylockEngine);
    }
}

} // namespace

SettingsManager::SettingsManager(const QString& settingsPath)
        : m_bShouldRescanLibrary(false) {
    // First make sure the settings path exists. If we don't then other parts of
    // Mixxx (such as the library) will produce confusing errors.
    const bool settingsDirectoryExistedBeforeStartup = QDir(settingsPath).exists();
    if (!settingsDirectoryExistedBeforeStartup) {
        QDir().mkpath(settingsPath);
    }

    // Check to see if this is the first time this version of Mixxx is run
    // after an upgrade and make any needed changes.
    Upgrade upgrader;
    m_pSettings = upgrader.versionUpgrade(settingsPath);
    VERIFY_OR_DEBUG_ASSERT(!m_pSettings.isNull()) {
        m_pSettings = UserSettingsPointer(new UserSettings(""));
    }

    initializeGlobalKeylockEngine(m_pSettings);

    m_bShouldRescanLibrary = upgrader.rescanLibrary();

    ControlDoublePrivate::setUserConfig(m_pSettings);

#ifdef __BROADCAST__
    m_pBroadcastSettings = BroadcastSettingsPointer(
                               new BroadcastSettings(m_pSettings));
#endif
}

SettingsManager::~SettingsManager() {
    ControlDoublePrivate::setUserConfig(UserSettingsPointer());
}
