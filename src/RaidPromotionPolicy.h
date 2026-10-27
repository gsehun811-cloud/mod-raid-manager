#ifndef RAID_MANAGER_PROMOTION_POLICY_H
#define RAID_MANAGER_PROMOTION_POLICY_H

#include "Define.h"

class Player;

namespace RaidManager
{
    enum class PromotionBlock : uint8
    {
        Allowed,
        LevelLimit,
        ProgressionUnavailable,
        BotAccountUnverified
    };

    struct PromotionAssessment
    {
        PromotionBlock block = PromotionBlock::ProgressionUnavailable;
        uint32 serverLimit = 0;
        uint32 playerbotsLimit = 0;
        uint32 progressionLimit = 0;
        bool botAccountVerified = false;
    };

    // Public API checks only; mutation and post-promotion eligibility checks belong to recruitment.
    PromotionAssessment AssessPromotion(Player* leader, Player* bot, bool allowEqualLevel = false);
    char const* DescribePromotionBlock(PromotionBlock block);
}

#endif
