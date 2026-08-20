#pragma once

#include "library/memorycuepromotionpolicy.h"
#include "preferences/configobject.h"
#include "preferences/usersettings.h"

namespace mixxx::library {

extern const ConfigKey kMainCuePromotionConfigKey;

/// Read the policy on a non-real-time thread before starting an import.
MainCuePromotionPolicy loadMainCuePromotionPolicy(
        const UserSettingsPointer& pConfig);

} // namespace mixxx::library
