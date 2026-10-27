#ifndef RAID_MANAGER_EQUIPMENT_CANDIDATE_RULES_H
#define RAID_MANAGER_EQUIPMENT_CANDIDATE_RULES_H

#include "RaidEquipmentSources.h"
#include <utility>
#include <vector>

namespace RaidManager
{
    enum class CandidateRoute : uint8 { None, CurrentBis, PreviousBis, Dungeon, IlvlFallback, Retained };
    enum class ProtectionRole : uint8 { Tank, Healer, Caster, Physical };

    struct ProtectedStats
    {
        float stamina = 0;
        float armor = 0;
        float defense = 0;
        float dodge = 0;
        float parry = 0;
        float blockValue = 0;
        float blockRating = 0;
        float healing = 0;
        float mana = 0;
        float intellect = 0;
        float spirit = 0;
        float spellPower = 0;
        float hit = 0;
        float expertise = 0;
        float weaponDps = 0;
    };
    struct CandidateCatalogEntry
    {
        uint32 item;
        uint16 ilvl;
        uint8 requiredLevel;
        uint8 inventoryType;
        uint8 itemClass;
        uint8 subclass;
        uint8 quality;
        uint8 observedStage;
        uint8 dungeonStage;
        uint8 observedDifficulty;
        bool observedRaid;
    };

    struct CandidateCeiling
    {
        uint32 map;
        uint8 difficulty;
        uint32 capacity;
        uint8 stage;
        uint16 ilvl;
        uint32 anchorItem;
    };

    enum class CandidateRejection : uint8
    {
        None,
        UnknownPolicy,
        ItemLevel,
        RequiredLevel,
        EraGuard,
        KnownLaterStage,
        KnownHigherDifficulty
    };

    struct CandidateLimits
    {
        uint16 ilvl = 0;
        uint8 stage = 0;
        uint8 maximumLevel = 0;
        uint32 anchorItem = 0;
        uint8 raidDifficultyMask = 0;
    };

    CandidateLimits GetCandidateLimits(uint32 map, uint32 difficulty, uint32 capacity);
    CandidateCatalogEntry const* GetCandidateMetadata(uint32 item);
    CandidateCatalogEntry const* GetPoolCandidate(std::size_t index);
    CandidateCatalogEntry const* GetIndexedPoolCandidate(std::size_t index);
    std::pair<std::size_t, std::size_t> GetIndexedCandidatePool(uint8 stage, uint8 playerClass,
        uint8 slot, uint16 ilvl);
    std::size_t GetCandidateCatalogSize();
    CandidateCatalogEntry const* GetCatalogCandidate(std::size_t index);
    bool MatchesCandidateIndexSnapshot(std::size_t index, uint16 classMask, bool unsupported);
    uint32 GetCandidateProficiencySkill(std::size_t index);
    bool IndexAllowsProficiency(std::size_t index, uint8 playerClass);
    std::size_t GetCandidateIndexMemoryBytes();
    std::pair<std::size_t, std::size_t> GetCandidatePool(uint8 slot, uint16 ilvl);
    CandidateRejection CheckCandidateLimits(CandidateCatalogEntry const& item, CandidateLimits const& limits,
        uint8 playerLevel);
    bool IsImprovement(float oldScore, float newScore);
    bool PreservesRoleFloors(ProtectionRole role, ProtectedStats const& oldStats, ProtectedStats const& newStats);
    bool PreferCandidate(CandidateRoute route, float score, uint32 item,
        CandidateRoute oldRoute, float oldScore, uint32 oldItem);
    std::vector<uint16> GetPreviousBisKeys(uint8 stage);
}

#endif
