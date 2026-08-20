#include "library/memorycuepromotionconfig.h"

namespace mixxx::library {

const ConfigKey kMainCuePromotionConfigKey = ConfigKey{
        QStringLiteral("[Library]"),
        QStringLiteral("PromoteFirstMemoryCueToMainCue")};

MainCuePromotionPolicy MainCuePromotionPolicy::currentBehavior() {
    return MainCuePromotionPolicy{};
}

MainCuePromotionPolicy MainCuePromotionPolicy::fromConfigValue(bool value) {
    return MainCuePromotionPolicy{value};
}

bool MainCuePromotionPolicy::shouldPromote(
        bool mainCueAlreadyFound, bool candidateIsLoop) const {
    return promoteFirstMemoryCue && !mainCueAlreadyFound && !candidateIsLoop;
}

MainCuePromotionPolicy loadMainCuePromotionPolicy(
        const UserSettingsPointer& pConfig) {
    if (!pConfig) {
        return MainCuePromotionPolicy::currentBehavior();
    }
    return MainCuePromotionPolicy::fromConfigValue(
            pConfig->getValue(kMainCuePromotionConfigKey, true));
}

} // namespace mixxx::library
