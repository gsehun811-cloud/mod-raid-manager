#ifndef RAID_MANAGER_PREPARATION_PROFILES_H
#define RAID_MANAGER_PREPARATION_PROFILES_H
#include "Define.h"
#include <string>
class Player;
class Map;
namespace RaidManager
{
    enum class PrepRole : uint8 { Tank, Healer, Ranged, Melee };
    struct RaidProfile
    {
        std::string name;
        uint16 bisKey = 0;
        uint32 map = 0;
        uint32 difficulty = 0;
        uint32 capacity = 0;
    };
    struct DifficultyPolicy
    {
        uint8 requestedTierSteps = 0;
        uint8 bonusPercent = 0;
    };
    DifficultyPolicy GetDifficultyPolicy(uint8 level, uint8 availableHigherTiers);
    void LoadPreparationProfiles();
    RaidProfile GetRaidProfile(Map* map);
    PrepRole ClassifyPrepRole(Player* bot);
    char const* RoleName(PrepRole role);
    int FindRoleSpec(Player* bot, PrepRole role);
    bool ApplyRoleTalents(Player* bot, PrepRole role, std::string& error);
    bool PrepareCombatSkill(Player* bot, uint32 skill);
    uint32 RequiredRoleSpell(Player* bot, PrepRole role);
}
#endif
