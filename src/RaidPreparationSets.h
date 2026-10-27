#ifndef RAID_MANAGER_PREPARATION_SETS_H
#define RAID_MANAGER_PREPARATION_SETS_H

#include "RaidEquipmentCandidateRules.h"
#include <array>
#include <map>

namespace RaidManager
{
    struct PreparedBisRecord
    {
        uint16 key;
        uint8 playerClass;
        uint8 tab;
        uint8 faction;
        uint32 present;
        std::array<uint16, 19> indices;
    };

    struct PreparedSetRecord
    {
        uint32 map;
        uint16 capacity;
        uint8 difficulty;
        uint8 playerClass;
        uint8 tab;
        uint8 role;
        uint8 faction;
        uint16 bisKey;
        std::array<std::array<uint16, 4>, 19> hints;
        std::array<uint8, 19> counts;
    };

    PreparedSetRecord const* GetPreparedSet(uint32 map, uint8 difficulty, uint16 capacity,
        uint8 playerClass, uint8 tab, uint8 role, uint8 faction, uint16 bisKey);
    PreparedBisRecord const* GetPreparedBis(uint16 key, uint8 playerClass, uint8 tab, uint8 faction);
    uint32 PreparedBisItem(PreparedBisRecord const* record, uint8 slot);
    bool PreparedBisMatches(PreparedBisRecord const* record, std::map<uint8, uint32> const& live);
    bool PreparedHintContains(PreparedSetRecord const* record, uint8 slot, uint32 item);
    std::size_t GetPreparedSetCount();
    std::size_t GetPreparedSetMemoryBytes();
    uint16 GetSnapshotClassMask(std::size_t index);
    bool GetSnapshotUnsupported(std::size_t index);
}

#endif
