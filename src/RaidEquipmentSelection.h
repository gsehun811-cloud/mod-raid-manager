#ifndef RAID_MANAGER_EQUIPMENT_SELECTION_H
#define RAID_MANAGER_EQUIPMENT_SELECTION_H

#include "RaidEquipmentCandidateRules.h"
#include "RaidPreparationProfiles.h"
#include <array>
#include <chrono>
#include <string>

class Player;

namespace RaidManager
{
    struct CandidateIndexValidation
    {
        std::size_t cursor = 0;
        bool complete = false;
        bool compatible = true;
        uint32 mismatchItem = 0;
    };

    struct SearchPhaseMetrics
    {
        uint32 visits = 0;
        uint32 scanned = 0;
        uint32 detailed = 0;
        uint32 scoreCandidates = 0;
        uint32 timeStops = 0;
        uint32 evaluationStops = 0;
        uint32 metadataStops = 0;
        uint64 workUs = 0;
        uint64 wallUs = 0;
    };

    struct EquipmentSearchMetrics
    {
        bool started = false;
        uint8 activePhase = 0;
        uint32 turns = 0;
        uint32 restarts = 0;
        uint64 validationUs = 0;
        uint32 preparedHits = 0;
        uint32 preparedMisses = 0;
        uint32 hintEvaluations = 0;
        std::array<SearchPhaseMetrics, 4> phases{};
        std::chrono::steady_clock::time_point startedAt{};
        std::chrono::steady_clock::time_point phaseEntered{};
        std::chrono::steady_clock::time_point finishedAt{};
    };

    struct EquipmentChoice
    {
        uint32 item = 0;
        CandidateRoute route = CandidateRoute::None;
        bool verifiedSource = false;
        float score = 0;
        std::string reason;
    };

    struct EquipmentSelection
    {
        bool started = false;
        bool poolStarted = false;
        bool indexed = false;
        char const* indexMode = "unused";
        uint8 phase = 0;
        std::size_t tier = 0;
        std::size_t cursor = 0;
        std::size_t end = 0;
        std::vector<uint16> previousKeys;
        EquipmentChoice choice;
        EquipmentSearchMetrics metrics;
    };

    // Incremental, read-only candidate search. No Player/AI pointers survive a work turn.
    bool SelectEquipmentStep(Player* bot, PrepRole role, RaidProfile const& profile, uint8 slot,
        uint32 exactItem, EquipmentSelection& selection, CandidateIndexValidation& validation,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
    bool RevalidateEquipmentChoice(Player* bot, PrepRole role, RaidProfile const& profile, uint8 slot,
        EquipmentChoice const& choice);
    bool IsOptionalOffhand(Player* bot, PrepRole role);
    char const* DescribeCandidateRoute(CandidateRoute route);
    std::string EquipmentSearchSummary(EquipmentSelection const& selection);
}

#endif
