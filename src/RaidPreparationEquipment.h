#ifndef RAID_MANAGER_PREPARATION_EQUIPMENT_H
#define RAID_MANAGER_PREPARATION_EQUIPMENT_H
#include "Define.h"
#include <string>
class Player;
namespace RaidManager
{
    bool ApplyPreparedItem(Player* bot, uint8 slot, uint32 item, std::string& reason);
}
#endif
