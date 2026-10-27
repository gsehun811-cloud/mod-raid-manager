#include "RaidEquipmentSources.h"
#include "RaidPreparationProfiles.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include <algorithm>
#include <iterator>

namespace RaidManager
{
namespace
{
    // Generated offline from repository SQL. No SQL loader or live DB dependency.
    #include "RaidEquipmentSourcesData.inc"

    bool SlotMatches(uint8 slot, uint32 inventoryType)
    {
        switch (slot)
        {
            case EQUIPMENT_SLOT_HEAD: return inventoryType == INVTYPE_HEAD;
            case EQUIPMENT_SLOT_NECK: return inventoryType == INVTYPE_NECK;
            case EQUIPMENT_SLOT_SHOULDERS: return inventoryType == INVTYPE_SHOULDERS;
            case EQUIPMENT_SLOT_CHEST: return inventoryType == INVTYPE_CHEST || inventoryType == INVTYPE_ROBE;
            case EQUIPMENT_SLOT_WAIST: return inventoryType == INVTYPE_WAIST;
            case EQUIPMENT_SLOT_LEGS: return inventoryType == INVTYPE_LEGS;
            case EQUIPMENT_SLOT_FEET: return inventoryType == INVTYPE_FEET;
            case EQUIPMENT_SLOT_WRISTS: return inventoryType == INVTYPE_WRISTS;
            case EQUIPMENT_SLOT_HANDS: return inventoryType == INVTYPE_HANDS;
            case EQUIPMENT_SLOT_FINGER1:
            case EQUIPMENT_SLOT_FINGER2: return inventoryType == INVTYPE_FINGER;
            case EQUIPMENT_SLOT_TRINKET1:
            case EQUIPMENT_SLOT_TRINKET2: return inventoryType == INVTYPE_TRINKET;
            case EQUIPMENT_SLOT_BACK: return inventoryType == INVTYPE_CLOAK;
            case EQUIPMENT_SLOT_MAINHAND:
                return inventoryType == INVTYPE_WEAPON || inventoryType == INVTYPE_WEAPONMAINHAND ||
                    inventoryType == INVTYPE_2HWEAPON;
            case EQUIPMENT_SLOT_OFFHAND:
                // Two-handed offhands need a separate talent/core check, not an invalid-slot diagnosis.
                return inventoryType == INVTYPE_WEAPON || inventoryType == INVTYPE_WEAPONOFFHAND ||
                    inventoryType == INVTYPE_SHIELD || inventoryType == INVTYPE_HOLDABLE ||
                    inventoryType == INVTYPE_2HWEAPON;
            case EQUIPMENT_SLOT_RANGED:
                return inventoryType == INVTYPE_RANGED || inventoryType == INVTYPE_RANGEDRIGHT ||
                    inventoryType == INVTYPE_THROWN || inventoryType == INVTYPE_RELIC;
            default: return false;
        }
    }
}

std::vector<EquipmentSource> GetEquipmentSources(uint32 item)
{
    auto first = std::lower_bound(std::begin(SourceRecords), std::end(SourceRecords), item,
        [](EquipmentSource const& source, uint32 id) { return source.item < id; });
    auto last = first;
    while (last != std::end(SourceRecords) && last->item == item)
        ++last;
    return {first, last};
}

SourcePolicy GetEquipmentSourcePolicy(RaidProfile const& profile)
{
    // Match actual map/difficulty/capacity; explicit BiS overrides do not authorize source tiers.
    for (auto const& map : SourceMaps)
        if (map.map == profile.map && map.raid && profile.difficulty < 4 &&
            (map.difficulties & (1u << profile.difficulty)) &&
            map.capacities[profile.difficulty] == profile.capacity)
        {
            uint8 modes = static_cast<uint8>((1u << (profile.difficulty + 1)) - 1);
            // A 10-player heroic raid does not authorize 25-player normal sources.
            if (profile.difficulty == 2)
                modes = 5;
            return {map.stage, modes, 3};
        }
    // Classic 40-player 249/533 cannot be proved by the Wrath SQL snapshot.
    return {};
}

SourceAssessment AssessEquipmentSource(uint32 item, SourcePolicy const& policy)
{
    if (policy.maximumStage == UnlockStage::Unknown)
        return {SourceVerdict::UnknownPolicy, 0};
    SourceAssessment result;
    for (auto const& source : GetEquipmentSources(item))
    {
        if (source.proof != SourceProof::SqlSnapshotPath || source.stage == UnlockStage::Unknown)
            continue;
        if (static_cast<uint8>(source.stage) > static_cast<uint8>(policy.maximumStage))
        {
            if (result.verdict == SourceVerdict::Unverified)
                result = {SourceVerdict::LaterStage, source.evidence};
            continue;
        }
        auto mask = source.raid ? policy.raidDifficultyMask : policy.dungeonDifficultyMask;
        if (!(mask & (1u << source.difficulty)))
        {
            result = {SourceVerdict::DifficultyBlocked, source.evidence};
            continue;
        }
        return {SourceVerdict::Allowed, source.evidence};
    }
    return result;
}

SourceDiagnostics DiagnoseEquipmentCandidate(Player const* bot, uint8 slot, uint32 item,
    SourcePolicy const& policy)
{
    SourceDiagnostics result;
    result.source = AssessEquipmentSource(item, policy);
    auto proto = sObjectMgr->GetItemTemplate(item);
    result.missingTemplate = !proto;
    if (!proto)
        return result;
    result.wrongSlot = !SlotMatches(slot, proto->InventoryType);
    if (bot)
        result.wrongFaction =
            (proto->HasFlag2(ITEM_FLAG2_FACTION_HORDE) && bot->GetTeamId() != TEAM_HORDE) ||
            (proto->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE) && bot->GetTeamId() != TEAM_ALLIANCE) ||
            !(proto->AllowableRace & bot->getRaceMask());
    return result;
}

char const* DescribeSourceVerdict(SourceVerdict verdict)
{
    switch (verdict)
    {
        case SourceVerdict::Unverified: return "unverified SQL acquisition path";
        case SourceVerdict::UnknownPolicy: return "unknown map/difficulty/capacity policy";
        case SourceVerdict::LaterStage: return "source unlocks after this raid stage";
        case SourceVerdict::DifficultyBlocked: return "source difficulty is not authorized";
        case SourceVerdict::Allowed: return "SQL snapshot source permitted by map policy";
    }
    return "unverified";
}

void LogEquipmentSourceDiagnostic(Player const* bot, uint8 slot, uint32 item, RaidProfile const& profile)
{
    auto result = DiagnoseEquipmentCandidate(bot, slot, item, GetEquipmentSourcePolicy(profile));
    LOG_INFO("module.raidmanager", "Source assessment: bot {} slot {} item {} map {} difficulty {}: {}; "
        "evidence {} (data/equipment-source-evidence.json), missingTemplate {}, wrongSlot {}, wrongFaction {}. "
        "SQL snapshot proof is not live acquisition validation; unverified fallback is checked separately.",
        bot->GetGUID().ToString(), slot, item, profile.map, profile.difficulty,
        DescribeSourceVerdict(result.source.verdict), result.source.evidence,
        result.missingTemplate, result.wrongSlot, result.wrongFaction);
}
}
