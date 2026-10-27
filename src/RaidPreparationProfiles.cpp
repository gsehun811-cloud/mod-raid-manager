#include "RaidPreparationProfiles.h"
#include "AiFactory.h"
#include "Config.h"
#include "DBCStores.h"
#include "Map.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <vector>
namespace RaidManager
{
namespace
{
    struct Tier { char const* name; uint16 key; };
    // Exact cache keys in the repository's playerbots_bis_gear dataset, not arithmetic item-level bumps.
    std::array<Tier, 12> const Tiers = {{{"Unknown", 0}, {"ClassicEarly", 78}, {"ClassicBWL", 83},
        {"ClassicAQ", 88}, {"ClassicNaxx", 92}, {"TBC4", 125}, {"TBC5", 141}, {"TBC6", 156},
        {"Sunwell", 164}, {"Wotlk7", 224}, {"Wotlk8", 245}, {"Wotlk9", 258}}};
    std::map<uint32, RaidProfile> Profiles;
    std::map<uint32, uint16> ConfiguredBisKeys;
    std::vector<RaidProfile> Overrides;
    std::map<std::pair<uint32, PrepRole>, std::string> SpecNames;
    std::vector<std::vector<uint32>> const& Preset(Player* bot, int spec)
    {
        auto const& levels = sPlayerbotAIConfig.parsedSpecLinkOrder[bot->getClass()][spec];
        for (int level = std::min<uint32>(bot->GetLevel(), MAX_LEVEL - 1); level > 0; --level)
            if (!levels[level].empty())
                return levels[level];
        for (uint32 level = bot->GetLevel() + 1; level < MAX_LEVEL; ++level)
            if (!levels[level].empty())
                return levels[level];
        static std::vector<std::vector<uint32>> const empty;
        return empty;
    }
    bool AllowedTalent(TalentEntry const* talent, uint32 level)
    {
        if (!talent || !sPlayerbotAIConfig.limitTalentsExpansion)
            return talent != nullptr;
        if (level <= 60 && (talent->Row > 6 || (talent->Row == 6 && talent->Col != 1)))
            return false;
        return !(level <= 70 && (talent->Row > 8 || (talent->Row == 8 && talent->Col != 1)));
    }
    uint32 DesiredTab(uint32 cls, PrepRole role)
    {
        if (role == PrepRole::Tank)
            return cls == CLASS_WARRIOR ? WARRIOR_TAB_PROTECTION :
                cls == CLASS_PALADIN ? PALADIN_TAB_PROTECTION : cls == CLASS_DRUID ? DRUID_TAB_FERAL :
                cls == CLASS_DEATH_KNIGHT ? DEATH_KNIGHT_TAB_BLOOD : 99;
        if (role == PrepRole::Healer)
            return cls == CLASS_PALADIN ? PALADIN_TAB_HOLY : cls == CLASS_PRIEST ? PRIEST_TAB_HOLY :
                cls == CLASS_DRUID ? DRUID_TAB_RESTORATION : cls == CLASS_SHAMAN ? SHAMAN_TAB_RESTORATION : 99;
        return 99;
    }
}
DifficultyPolicy GetDifficultyPolicy(uint8 level, uint8 availableHigherTiers)
{
    if (level < 1 || level > 7)
        return {};
    uint8 steps = level == 1 ? 0 : level == 2 ? 1 : 2;
    uint8 missing = steps - std::min(steps, availableHigherTiers);
    return { steps, static_cast<uint8>(missing * 5 + (level >= 4 ? (level - 3) * 5 : 0)) };
}
char const* RoleName(PrepRole role)
{
    switch (role)
    {
        case PrepRole::Tank: return "Tank";
        case PrepRole::Healer: return "Healer";
        case PrepRole::Ranged: return "Ranged";
        case PrepRole::Melee: return "Melee";
    }
    return "Unknown";
}
void LoadPreparationProfiles()
{
    Profiles.clear();
    ConfiguredBisKeys.clear();
    // 249/533 are resolved using actual raid capacity below (classic variants vs Wrath variants).
    std::map<uint32, std::string> const maps = {{249,"Wotlk9"}, {409,"ClassicEarly"}, {469,"ClassicBWL"},
        {309,"ClassicEarly"}, {509,"ClassicEarly"}, {531,"ClassicAQ"}, {532,"TBC4"}, {534,"TBC6"},
        {544,"TBC4"}, {548,"TBC5"}, {550,"TBC5"}, {564,"TBC6"}, {565,"TBC4"}, {568,"TBC6"},
        {580,"Sunwell"}, {533,"Wotlk7"}, {615,"Wotlk7"}, {616,"Wotlk7"}, {624,"Unknown"},
        {603,"Wotlk8"}, {649,"Wotlk9"}, {631,"Wotlk10"}, {724,"RubySanctum"}};
    for (auto const& [map, name] : maps)
    {
        uint16 key = name == "Wotlk10" ? 264 : name == "RubySanctum" ? 290 : 0;
        for (Tier const& tier : Tiers)
            if (name == tier.name)
                key = tier.key;
        int32 configured = sConfigMgr->GetOption<int32>("RaidManager.Prep.BisKey." + std::to_string(map), -1);
        if (configured >= 0 && configured <= 65535)
            ConfiguredBisKeys[map] = static_cast<uint16>(configured);
        Profiles[map] = { name, key, map, 0, 0 };
    }
    SpecNames.clear();
    for (uint32 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
        for (PrepRole role : {PrepRole::Tank, PrepRole::Healer, PrepRole::Ranged, PrepRole::Melee})
        {
            std::string fallback = cls == CLASS_DRUID && role == PrepRole::Tank ? "bear pve" :
                cls == CLASS_DRUID && role == PrepRole::Melee ? "cat pve" : "";
            SpecNames[{cls, role}] = sConfigMgr->GetOption<std::string>("RaidManager.Prep.Spec." +
                std::to_string(cls) + "." + RoleName(role), fallback);
        }
    LOG_INFO("module.raidmanager", "Preparation config evaluated; expected module file {}modules/raid-manager.conf. "
        "File load status is reported by ConfigMgr, not inferred here. Effective RaidManager keys: {}",
        sConfigMgr->GetConfigPath(), sConfigMgr->GetKeysByString("RaidManager.").size());
    Overrides.clear();
    // Explicit custom profile: map difficulty actualCapacity name exactBisCacheKey; entries separated by ';'.
    std::istringstream entries(sConfigMgr->GetOption<std::string>("RaidManager.Prep.Profiles", ""));
    std::string entry;
    while (std::getline(entries, entry, ';'))
    {
        RaidProfile profile;
        uint32 key = 0;
        std::istringstream fields(entry);
        if (fields >> profile.map >> profile.difficulty >> profile.capacity >> profile.name >> key &&
            key <= 65535 && profile.capacity)
        {
            profile.bisKey = static_cast<uint16>(key);
            auto duplicate = std::find_if(Overrides.begin(), Overrides.end(), [&profile](RaidProfile const& existing)
            {
                return existing.map == profile.map && existing.difficulty == profile.difficulty &&
                    existing.capacity == profile.capacity;
            });
            if (duplicate != Overrides.end())
            {
                LOG_WARN("module.raidmanager", "Duplicate preparation profile {}/{}/{}: first entry retained",
                    profile.map, profile.difficulty, profile.capacity);
                continue;
            }
            Overrides.push_back(profile);
        }
        else if (entry.find_first_not_of(" \t\r\n") != std::string::npos)
            LOG_WARN("module.raidmanager", "Invalid preparation profile entry '{}'; ignored", entry);
    }
}
RaidProfile GetRaidProfile(Map* map)
{
    RaidProfile result;
    result.map = map->GetId();
    result.difficulty = map->GetSpawnMode();
    result.capacity = map->ToInstanceMap()->GetMaxPlayers();
    auto found = Profiles.find(result.map);
    if (found != Profiles.end())
    {
        result.name = found->second.name;
        result.bisKey = found->second.bisKey;
    }
    if (result.capacity == 40 && (result.map == 249 || result.map == 533))
    {
        result.name = result.map == 249 ? "ClassicOnyxia" : "ClassicNaxx";
        result.bisKey = result.map == 249 ? 78 : 92;
    }
    // Ambiguous/custom capacities or difficulties need an exact profile rather than a guessed tier.
    bool classic = result.map == 309 || result.map == 409 || result.map == 469 ||
        result.map == 509 || result.map == 531;
    uint32 classicCapacity = result.map == 309 || result.map == 509 ? 20 : 40;
    bool variant = result.map == 249 || result.map == 533;
    if ((classic && (result.capacity != classicCapacity || result.difficulty != 0)) ||
        (variant && result.capacity != 40 && result.capacity != 10 && result.capacity != 25) ||
        (variant && result.capacity == 40 && result.difficulty != 0))
    {
        result.name = "UnknownVariant";
        result.bisKey = 0;
    }
    auto configured = ConfiguredBisKeys.find(result.map);
    if (configured != ConfiguredBisKeys.end())
        result.bisKey = configured->second;
    for (RaidProfile const& profile : Overrides)
        if (profile.map == result.map && profile.difficulty == result.difficulty && profile.capacity == result.capacity)
            return profile;
    return result;
}
PrepRole ClassifyPrepRole(Player* bot)
{
    if (PlayerbotAI::IsTank(bot, true) || (bot->getClass() == CLASS_DRUID &&
        AiFactory::GetPlayerSpecTab(bot) == DRUID_TAB_FERAL && PlayerbotAI::IsTank(bot)))
        return PrepRole::Tank;
    if (PlayerbotAI::IsHeal(bot, true))
        return PrepRole::Healer;
    return PlayerbotAI::IsRangedDps(bot, true) ? PrepRole::Ranged : PrepRole::Melee;
}
int FindRoleSpec(Player* bot, PrepRole role)
{
    auto const& requested = SpecNames[{bot->getClass(), role}];
    uint32 desired = DesiredTab(bot->getClass(), role);
    if (bot->getClass() == CLASS_PRIEST && role == PrepRole::Healer &&
        AiFactory::GetPlayerSpecTab(bot) == PRIEST_TAB_DISCIPLINE)
        desired = PRIEST_TAB_DISCIPLINE;
    if (desired == 99)
        desired = AiFactory::GetPlayerSpecTab(bot);
    for (int spec = 0; spec < MAX_SPECNO; ++spec)
    {
        auto const& name = sPlayerbotAIConfig.premadeSpecName[bot->getClass()][spec];
        if (name.empty() || (!requested.empty() && name != requested))
            continue;
        // Bear/cat share a tab: require explicit configuration rather than guessing a role from the tab.
        if (bot->getClass() == CLASS_DRUID && desired == DRUID_TAB_FERAL && requested.empty())
            continue;
        std::array<uint32, 3> counts{};
        for (auto const& point : Preset(bot, spec))
            if (point.size() == 4 && point[0] < counts.size())
                counts[point[0]] += point[3];
        if (*std::max_element(counts.begin(), counts.end()) &&
            static_cast<uint32>(std::max_element(counts.begin(), counts.end()) - counts.begin()) == desired)
            return spec;
    }
    return -1;
}
bool ApplyRoleTalents(Player* bot, PrepRole role, std::string& error)
{
    int spec = FindRoleSpec(bot, role);
    if (spec < 0)
    {
        error = "no verified role/level talent preset (feral roles require an explicit preset name)";
        return false;
    }
    // Validate the entire plan before resetting; use core LearnTalent one point at a time so no helper
    // can reset inventory, learn professions, or underflow a zero-free-points rank calculation.
    std::vector<std::pair<uint32, uint32>> plan;
    for (auto const& point : Preset(bot, spec))
    {
        if (point.size() != 4)
        {
            error = "malformed talent preset entry";
            return false;
        }
        if (!point[3])
            continue;
        TalentEntry const* found = nullptr;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            auto talent = sTalentStore.LookupEntry(i);
            auto tab = talent ? sTalentTabStore.LookupEntry(talent->TalentTab) : nullptr;
            if (tab && (tab->ClassMask & bot->getClassMask()) && tab->tabpage == point[0] &&
                talent->Row == point[1] && talent->Col == point[2])
                found = talent;
        }
        if (!found)
        {
            error = "talent preset references an unknown class/tree/row/column";
            return false;
        }
        if (!AllowedTalent(found, bot->GetLevel()))
            continue;
        if (point[3] > MAX_TALENT_RANK || !found->RankID[point[3] - 1])
        {
            error = "invalid talent rank in preset";
            return false;
        }
        plan.emplace_back(found->TalentID, point[3]);
    }
    if (plan.empty())
    {
        error = "preset has no talents allowed by current era/level policy";
        return false;
    }
    bot->resetTalents(true);
    for (auto const& [id, ranks] : plan)
    {
        if (!bot->GetFreeTalentPoints())
            break;
        uint32 known = 0;
        auto talent = sTalentStore.LookupEntry(id);
        for (uint32 rank = 0; rank < MAX_TALENT_RANK; ++rank)
            if (talent->RankID[rank] && bot->HasTalent(talent->RankID[rank], bot->GetActiveSpec()))
                known = rank + 1;
        for (uint32 rank = known; rank < ranks && bot->GetFreeTalentPoints(); ++rank)
        {
            bot->LearnTalent(id, rank);
            if (!bot->HasTalent(talent->RankID[rank], bot->GetActiveSpec()))
            {
                error = "preset failed a prerequisite or learning validation";
                return false;
            }
        }
    }
    bot->SendTalentsInfoData(false);
    if (bot->GetFreeTalentPoints() == bot->CalculateTalentsPoints())
    {
        error = "no talent point was applied";
        return false;
    }
    if (ClassifyPrepRole(bot) == role)
        return true;
    // Bear strategy is applied in the AI step; verify its feral tree here, not its temporary form.
    if (role == PrepRole::Tank && bot->getClass() == CLASS_DRUID &&
        AiFactory::GetPlayerSpecTab(bot) == DRUID_TAB_FERAL)
        return true;
    error = "applied talents do not verify the requested role";
    return false;
}
bool PrepareCombatSkill(Player* bot, uint32 skill)
{
    if (!bot->HasSkill(skill))
        return false;
    uint16 maximum = std::max(bot->GetPureMaxSkillValue(skill), bot->GetMaxSkillValueForLevel());
    bot->SetSkill(skill, bot->GetSkillStep(skill), std::max(bot->GetPureSkillValue(skill), maximum), maximum);
    return bot->GetPureSkillValue(skill) >= maximum;
}
uint32 RequiredRoleSpell(Player* bot, PrepRole role)
{
    if (role == PrepRole::Tank)
    {
        switch (bot->getClass())
        {
            case CLASS_WARRIOR: return 355;
            case CLASS_PALADIN: return 25780;
            case CLASS_DRUID: return 6795;
            case CLASS_DEATH_KNIGHT: return 56222;
            default: return 0;
        }
    }
    if (role == PrepRole::Healer)
    {
        switch (bot->getClass())
        {
            case CLASS_PRIEST: return 2050;
            case CLASS_PALADIN: return 635;
            case CLASS_DRUID: return 5185;
            case CLASS_SHAMAN: return 331;
            default: return 0;
        }
    }
    switch (bot->getClass())
    {
        case CLASS_WARRIOR: return 78;
        case CLASS_PALADIN: return 21084;
        case CLASS_DRUID: return role == PrepRole::Melee ? 5221 : 5176;
        case CLASS_DEATH_KNIGHT: return 45477;
        case CLASS_HUNTER: return 75;
        case CLASS_ROGUE: return 1752;
        case CLASS_PRIEST: return 585;
        case CLASS_MAGE: return 133;
        case CLASS_WARLOCK: return 686;
        case CLASS_SHAMAN: return 403;
        default: break;
    }
    return 0;
}
}
