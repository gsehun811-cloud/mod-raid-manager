#include "RaidPromotionPolicy.h"

#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "World.h"

// Disabled/missing progression modules must not turn into permission to promote.
#if __has_include("IndividualProgression.h")
#include "IndividualProgression.h"
#define RAID_MANAGER_HAS_PROGRESSION_API
#endif

namespace RaidManager
{
PromotionAssessment AssessPromotion(Player* leader, Player* bot, bool allowEqualLevel)
{
    PromotionAssessment result;
    result.serverLimit = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    result.playerbotsLimit = sPlayerbotAIConfig.randomBotMaxLevel;
    if (!leader || !bot || !result.serverLimit || !result.playerbotsLimit ||
        (allowEqualLevel ? bot->GetLevel() > leader->GetLevel() : bot->GetLevel() >= leader->GetLevel()) ||
        leader->GetLevel() > result.serverLimit ||
        leader->GetLevel() > result.playerbotsLimit)
    {
        result.block = PromotionBlock::LevelLimit;
        return result;
    }

#ifdef RAID_MANAGER_HAS_PROGRESSION_API
    // Use the module's live state, rather than assuming that config defaults describe the running realm.
    if (!sIndividualProgression->enabled || sIndividualProgression->BotAccountsMaxLevel <= 0)
        return result;

    result.progressionLimit = static_cast<uint32>(sIndividualProgression->BotAccountsMaxLevel);
    // Use the progression module's actual account-name classifier. RandomPlayerbotMgr ownership
    // alone does not prove that this module treats the account as a bot account. A failed lookup
    // also returns false and must not authorize promotion. This API reads the login DB at runtime.
    result.botAccountVerified = sIndividualProgression->isBotAccount(bot);
    if (!result.botAccountVerified)
    {
        result.block = PromotionBlock::BotAccountUnverified;
        return result;
    }
    if (leader->GetLevel() > result.progressionLimit)
    {
        result.block = PromotionBlock::LevelLimit;
        return result;
    }

    // The progression module applies BotAccountsMaxLevel to these accounts, rather than the normal
    // player's 60/70 personal progression gates. Do not grant or copy any progression quests.
    // Combat preparation is deferred to raidprep. Normal level-up maintenance (including item
    // replacement, talent/spell changes and shaman totem cleanup) is explicitly permitted.
    result.block = PromotionBlock::Allowed;
#endif
    return result;
}

char const* DescribePromotionBlock(PromotionBlock block)
{
    switch (block)
    {
        case PromotionBlock::Allowed:
            return "level limits and bot account classification verified";
        case PromotionBlock::LevelLimit:
            return "target level is outside the verified server, Playerbots or progression level limits";
        case PromotionBlock::ProgressionUnavailable:
            return "the active progression limits could not be verified; promotion is disabled";
        case PromotionBlock::BotAccountUnverified:
            return "the progression module could not confirm this as a bot account";
    }
    return "promotion is disabled";
}
}
