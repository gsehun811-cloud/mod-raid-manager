#include "RaidEquipmentCandidateRules.h"
#include "RaidPreparationSets.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>

namespace RaidManager
{
namespace
{
    #include "RaidEquipmentCandidateData.inc"
    #include "RaidEquipmentCandidateIndexes.inc"
    #include "RaidPreparationSetData.inc"

    int ClassIndex(uint8 playerClass)
    {
        return playerClass >= 1 && playerClass <= 9 ? playerClass - 1 : playerClass == 11 ? 9 : -1;
    }
}

PreparedSetRecord const* GetPreparedSet(uint32 map, uint8 difficulty, uint16 capacity,
    uint8 playerClass, uint8 tab, uint8 role, uint8 faction, uint16 bisKey)
{
    auto key = std::make_tuple(map, difficulty, capacity, playerClass, tab, role, faction);
    auto found = std::lower_bound(std::begin(PreparedSetRecords), std::end(PreparedSetRecords), key,
        [](PreparedSetRecord const& r, auto const& k)
        {
            return std::tie(r.map, r.difficulty, r.capacity, r.playerClass, r.tab, r.role, r.faction) < k;
        });
    return found != std::end(PreparedSetRecords) && found->bisKey == bisKey &&
        std::tie(found->map, found->difficulty, found->capacity, found->playerClass,
            found->tab, found->role, found->faction) == key ? found : nullptr;
}

PreparedBisRecord const* GetPreparedBis(uint16 key, uint8 playerClass, uint8 tab, uint8 faction)
{
    auto target = std::make_tuple(key, playerClass, tab, faction);
    auto found = std::lower_bound(std::begin(PreparedBisRecords), std::end(PreparedBisRecords), target,
        [](PreparedBisRecord const& r, auto const& k)
        {
            return std::tie(r.key, r.playerClass, r.tab, r.faction) < k;
        });
    return found != std::end(PreparedBisRecords) &&
        std::tie(found->key, found->playerClass, found->tab, found->faction) == target ? found : nullptr;
}

uint32 PreparedBisItem(PreparedBisRecord const* record, uint8 slot)
{
    if (!record || slot >= 19 || !(record->present & (1u << slot)))
        return 0;
    auto item = GetCatalogCandidate(record->indices[slot]);
    return item ? item->item : 0;
}

bool PreparedBisMatches(PreparedBisRecord const* record, std::map<uint8, uint32> const& live)
{
    for (auto const& [slot, item] : live)
        if (PreparedBisItem(record, slot) != item)
            return false;
    for (uint8 slot = 0; slot < 19; ++slot)
        if (uint32 item = PreparedBisItem(record, slot); item && !live.count(slot))
            return false;
    return true;
}

bool PreparedHintContains(PreparedSetRecord const* record, uint8 slot, uint32 item)
{
    if (!record || slot >= 19)
        return false;
    for (uint8 i = 0; i < record->counts[slot]; ++i)
        if (auto entry = GetCatalogCandidate(record->hints[slot][i]); entry && entry->item == item)
            return true;
    return false;
}

std::size_t GetPreparedSetCount()
{
    return std::size(PreparedSetRecords);
}

std::size_t GetPreparedSetMemoryBytes()
{
    return sizeof(PreparedSetRecords) + sizeof(PreparedBisRecords);
}

uint16 GetSnapshotClassMask(std::size_t index)
{
    return index < std::size(CandidateSnapshotClassMasks) ? CandidateSnapshotClassMasks[index] : 0;
}

bool GetSnapshotUnsupported(std::size_t index)
{
    return index >= std::size(CandidateSnapshotUnsupported) || CandidateSnapshotUnsupported[index];
}

CandidateLimits GetCandidateLimits(uint32 map, uint32 difficulty, uint32 capacity)
{
    for (auto const& entry : CandidateCeilings)
        if (entry.map == map && entry.difficulty == difficulty && entry.capacity == capacity)
        {
            uint8 level = entry.stage < static_cast<uint8>(UnlockStage::TbcDungeons) ? 60 :
                entry.stage < static_cast<uint8>(UnlockStage::WrathDungeons) ? 70 : 80;
            uint8 modes = difficulty == 2 ? 5 : static_cast<uint8>((1u << (difficulty + 1)) - 1);
            return {entry.ilvl, entry.stage, level, entry.anchorItem, modes};
        }
    return {};
}

CandidateCatalogEntry const* GetCandidateMetadata(uint32 item)
{
    auto found = std::lower_bound(std::begin(CandidateCatalog), std::end(CandidateCatalog), item,
        [](CandidateCatalogEntry const& entry, uint32 id) { return entry.item < id; });
    return found != std::end(CandidateCatalog) && found->item == item ? found : nullptr;
}

CandidateCatalogEntry const* GetPoolCandidate(std::size_t index)
{
    return index < std::size(CandidatePoolIndices) ? &CandidateCatalog[CandidatePoolIndices[index]] : nullptr;
}

CandidateCatalogEntry const* GetIndexedPoolCandidate(std::size_t index)
{
    return index < std::size(StageClassPoolIndices) ? &CandidateCatalog[StageClassPoolIndices[index]] : nullptr;
}

std::pair<std::size_t, std::size_t> GetIndexedCandidatePool(uint8 stage, uint8 playerClass,
    uint8 slot, uint16 ilvl)
{
    int cls = ClassIndex(playerClass);
    if (!stage || stage > 18 || cls < 0 || slot >= 19)
        return {};
    std::size_t bucket = ((static_cast<std::size_t>(stage) - 1) * 10 + cls) * 19 + slot;
    auto first = std::begin(StageClassPoolIndices) + StageClassPoolOffsets[bucket];
    auto last = std::begin(StageClassPoolIndices) + StageClassPoolOffsets[bucket + 1];
    auto start = std::lower_bound(first, last, ilvl,
        [](uint16 index, uint16 maximum) { return CandidateCatalog[index].ilvl > maximum; });
    return {static_cast<std::size_t>(start - std::begin(StageClassPoolIndices)),
        static_cast<std::size_t>(last - std::begin(StageClassPoolIndices))};
}

std::size_t GetCandidateCatalogSize()
{
    return std::size(CandidateCatalog);
}

CandidateCatalogEntry const* GetCatalogCandidate(std::size_t index)
{
    return index < std::size(CandidateCatalog) ? &CandidateCatalog[index] : nullptr;
}

bool MatchesCandidateIndexSnapshot(std::size_t index, uint16 classMask, bool unsupported)
{
    return index < std::size(CandidateCatalog) && CandidateSnapshotClassMasks[index] == classMask &&
        static_cast<bool>(CandidateSnapshotUnsupported[index]) == unsupported;
}

uint32 GetCandidateProficiencySkill(std::size_t index)
{
    return index < std::size(CandidateProficiencySkills) ? CandidateProficiencySkills[index] : 0;
}

bool IndexAllowsProficiency(std::size_t index, uint8 playerClass)
{
    return ClassIndex(playerClass) >= 0 && index < std::size(CandidateProficiencyClassMasks) &&
        (CandidateProficiencyClassMasks[index] & (1u << (playerClass - 1)));
}

std::size_t GetCandidateIndexMemoryBytes()
{
    return sizeof(CandidateCatalog) + sizeof(CandidateCeilings) + sizeof(CandidatePoolIndices) +
        sizeof(CandidatePoolOffsets) + sizeof(StageClassPoolIndices) + sizeof(StageClassPoolOffsets) +
        sizeof(CandidateSnapshotClassMasks) + sizeof(CandidateSnapshotUnsupported) +
        sizeof(CandidateProficiencySkills) + sizeof(CandidateProficiencyClassMasks);
}

std::pair<std::size_t, std::size_t> GetCandidatePool(uint8 slot, uint16 ilvl)
{
    if (slot >= std::size(CandidatePoolOffsets) - 1)
        return {};
    auto first = std::begin(CandidatePoolIndices) + CandidatePoolOffsets[slot];
    auto last = std::begin(CandidatePoolIndices) + CandidatePoolOffsets[slot + 1];
    auto start = std::lower_bound(first, last, ilvl,
        [](uint32 index, uint16 maximum) { return CandidateCatalog[index].ilvl > maximum; });
    return {static_cast<std::size_t>(start - std::begin(CandidatePoolIndices)),
        static_cast<std::size_t>(last - std::begin(CandidatePoolIndices))};
}

CandidateRejection CheckCandidateLimits(CandidateCatalogEntry const& item, CandidateLimits const& limits,
    uint8 playerLevel)
{
    if (!limits.ilvl || !limits.stage)
        return CandidateRejection::UnknownPolicy;
    if (!item.ilvl || item.ilvl > limits.ilvl)
        return CandidateRejection::ItemLevel;
    if (item.requiredLevel > std::min(playerLevel, limits.maximumLevel))
        return CandidateRejection::RequiredLevel;
    // Conservative legacy guards used for rejection only. They do not prove introduction or acquisition.
    if ((limits.maximumLevel <= 60 && item.item >= 23728) ||
        (limits.maximumLevel <= 70 && item.item >= 35570))
        return CandidateRejection::EraGuard;
    if (item.observedStage > limits.stage)
        return CandidateRejection::KnownLaterStage;
    if (item.observedRaid && item.observedStage == limits.stage &&
        (item.observedDifficulty >= 4 || !(limits.raidDifficultyMask & (1u << item.observedDifficulty))))
        return CandidateRejection::KnownHigherDifficulty;
    return CandidateRejection::None;
}

bool IsImprovement(float oldScore, float newScore)
{
    return std::isfinite(oldScore) && std::isfinite(newScore) && newScore > 0.0f &&
        newScore > oldScore * 1.05f + 0.01f;
}

bool PreservesRoleFloors(ProtectionRole role, ProtectedStats const& oldStats, ProtectedStats const& newStats)
{
    auto preserves = [](float before, float after)
    {
        return std::isfinite(before) && std::isfinite(after) && after + 0.001f >= before;
    };
    if (!preserves(oldStats.weaponDps, newStats.weaponDps))
        return false;
    if (role == ProtectionRole::Tank)
        return preserves(oldStats.stamina, newStats.stamina) && preserves(oldStats.armor, newStats.armor) &&
            preserves(oldStats.defense, newStats.defense) && preserves(oldStats.dodge, newStats.dodge) &&
            preserves(oldStats.parry, newStats.parry) && preserves(oldStats.blockValue, newStats.blockValue) &&
            preserves(oldStats.blockRating, newStats.blockRating);
    if (role == ProtectionRole::Healer)
        return preserves(oldStats.healing, newStats.healing) && preserves(oldStats.mana, newStats.mana) &&
            preserves(oldStats.intellect, newStats.intellect) && preserves(oldStats.spirit, newStats.spirit);
    if (role == ProtectionRole::Caster)
        return preserves(oldStats.spellPower, newStats.spellPower) && preserves(oldStats.hit, newStats.hit);
    return preserves(oldStats.hit, newStats.hit) && preserves(oldStats.expertise, newStats.expertise);
}

bool PreferCandidate(CandidateRoute route, float score, uint32 item,
    CandidateRoute oldRoute, float oldScore, uint32 oldItem)
{
    if (!item || !std::isfinite(score) || score <= 0.0f)
        return false;
    if (!oldItem)
        return true;
    if (route != oldRoute)
        return static_cast<uint8>(route) < static_cast<uint8>(oldRoute);
    return score > oldScore || (score == oldScore && item < oldItem);
}

std::vector<uint16> GetPreviousBisKeys(uint8 stage)
{
    // Release-stage order, not arithmetic subtraction from the current cache key.
    struct Tier { UnlockStage stage; uint16 key; };
    Tier const tiers[] =
    {
        {UnlockStage::WrathRuby, 290}, {UnlockStage::WrathIcc, 264}, {UnlockStage::WrathToc, 258},
        {UnlockStage::WrathUlduar, 245}, {UnlockStage::WrathT7, 224}, {UnlockStage::TbcSunwell, 164},
        {UnlockStage::TbcZA, 156}, {UnlockStage::TbcT6, 156}, {UnlockStage::TbcT5, 141},
        {UnlockStage::TbcT4, 125}, {UnlockStage::ClassicNaxx, 92}, {UnlockStage::ClassicAQ, 88},
        {UnlockStage::ClassicZG, 78}, {UnlockStage::ClassicBWL, 83}, {UnlockStage::ClassicMC, 78}
    };
    std::vector<uint16> result;
    for (auto const& [tier, key] : tiers)
        if (static_cast<uint8>(tier) < stage && std::find(result.begin(), result.end(), key) == result.end())
            result.push_back(key);
    return result;
}
}
