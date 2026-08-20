#include "library/memorycuepromotionpolicy.h"

#include <gtest/gtest.h>

namespace mixxx::library {

TEST(MainCuePromotionPolicyTest, DefaultsToCurrentBehavior) {
    const auto policy = MainCuePromotionPolicy::currentBehavior();

    EXPECT_TRUE(policy.shouldPromote(false, false));
    EXPECT_FALSE(policy.shouldPromote(true, false));
    EXPECT_FALSE(policy.shouldPromote(false, true));
}

TEST(MainCuePromotionPolicyTest, CanDisablePromotion) {
    const auto policy = MainCuePromotionPolicy::fromConfigValue(false);

    EXPECT_FALSE(policy.shouldPromote(false, false));
    EXPECT_FALSE(policy.shouldPromote(true, false));
    EXPECT_FALSE(policy.shouldPromote(false, true));
}

TEST(MainCuePromotionPolicyTest, ConfigValueControlsPromotion) {
    EXPECT_TRUE(MainCuePromotionPolicy::fromConfigValue(true)
                    .shouldPromote(false, false));
    EXPECT_FALSE(MainCuePromotionPolicy::fromConfigValue(false)
                    .shouldPromote(false, false));
}

} // namespace mixxx::library
