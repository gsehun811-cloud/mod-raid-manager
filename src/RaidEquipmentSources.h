#ifndef RAID_MANAGER_EQUIPMENT_SOURCES_H
#define RAID_MANAGER_EQUIPMENT_SOURCES_H

#include "Define.h"
#include <cstddef>
#include <vector>

class Player;

namespace RaidManager
{
    struct RaidProfile;

    // Explicit chronological policy, independent of item level and BiS cache keys.
    enum class UnlockStage : uint8
    {
        Unknown,
        ClassicDungeons,
        ClassicMC,
        ClassicBWL,
        ClassicZG,
        ClassicAQ,
        ClassicNaxx,
        TbcDungeons,
        TbcT4,
        TbcT5,
        TbcT6,
        TbcZA,
        TbcSunwell,
        WrathDungeons,
        WrathT7,
        WrathUlduar,
        WrathToc,
        WrathIcc,
        WrathRuby
    };

    enum class SourceKind : uint8 { Creature, Chest };
    enum class SourceProof : uint8 { Unverified, SqlSnapshotPath };
    enum class SourceVerdict : uint8 { Unverified, UnknownPolicy, LaterStage, DifficultyBlocked, Allowed };

    struct EquipmentSource
    {
        uint32 item;
        uint32 map;
        uint8 difficulty;
        UnlockStage stage;
        SourceKind kind;
        SourceProof proof;
        bool raid;
        uint32 evidence;
    };

    struct SourcePolicy
    {
        UnlockStage maximumStage = UnlockStage::Unknown;
        uint8 raidDifficultyMask = 0;
        uint8 dungeonDifficultyMask = 0;
    };

    struct SourceAssessment
    {
        SourceVerdict verdict = SourceVerdict::Unverified;
        uint32 evidence = 0;
        bool EligibleForAutomaticSelection() const { return verdict == SourceVerdict::Allowed; }
    };

    struct SourceDiagnostics
    {
        SourceAssessment source;
        bool missingTemplate = false;
        bool wrongSlot = false;
        bool wrongFaction = false;
        bool EligibleForAutomaticSelection() const
        {
            return source.EligibleForAutomaticSelection() && !missingTemplate && !wrongSlot && !wrongFaction;
        }
    };

    // Evidence IDs resolve to data/equipment-source-evidence.json; never query a database here.
    std::vector<EquipmentSource> GetEquipmentSources(uint32 item);
    SourcePolicy GetEquipmentSourcePolicy(RaidProfile const& profile);
    SourceAssessment AssessEquipmentSource(uint32 item, SourcePolicy const& policy);
    SourceDiagnostics DiagnoseEquipmentCandidate(Player const* bot, uint8 slot, uint32 item,
        SourcePolicy const& policy);
    void LogEquipmentSourceDiagnostic(Player const* bot, uint8 slot, uint32 item, RaidProfile const& profile);
    char const* DescribeSourceVerdict(SourceVerdict verdict);
}

#endif
