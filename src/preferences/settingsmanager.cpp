#include "preferences/settingsmanager.h"

#include <QDir>
#include <array>

#include "control/control.h"
#include "engine/enginebuffer.h"
#include "preferences/upgrade.h"
#include "util/assert.h"

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

EngineBuffer::KeylockEngine defaultStableKeylockEngine() {
#ifdef __RUBBERBAND__
    return EngineBuffer::KeylockEngine::RubberBandFaster;
#else
    return EngineBuffer::KeylockEngine::SoundTouch;
#endif
}

bool isStableKeylockEngine(EngineBuffer::KeylockEngine engine) {
    switch (engine) {
    case EngineBuffer::KeylockEngine::SoundTouch:
        return true;
#ifdef __RUBBERBAND__
    case EngineBuffer::KeylockEngine::RubberBandFaster:
    case EngineBuffer::KeylockEngine::RubberBandFiner:
        return true;
#endif
    default:
        return false;
    }
}

EngineBuffer::KeylockEngine defaultKeylockEngineForMigration(
        const UserSettingsPointer& pSettings) {
    if (pSettings->exists(kGlobalKeylockEngineKey)) {
        const auto globalKeylockEngine =
                pSettings->getValue<EngineBuffer::KeylockEngine>(
                        kGlobalKeylockEngineKey,
                        defaultStableKeylockEngine());
        if (isStableKeylockEngine(globalKeylockEngine)) {
            return globalKeylockEngine;
        }
    }
    return defaultStableKeylockEngine();
}

void initializePerDeckKeylockEngines(const UserSettingsPointer& pSettings) {
    const auto keylockEngine = defaultKeylockEngineForMigration(pSettings);
    for (const char* group : kDeckGroups) {
        const ConfigKey keylockEngineKey(
                QString::fromLatin1(group),
                QStringLiteral("keylock_engine"));
        if (!pSettings->exists(keylockEngineKey)) {
            pSettings->setValue(keylockEngineKey, keylockEngine);
        }
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

    initializePerDeckKeylockEngines(m_pSettings);

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
