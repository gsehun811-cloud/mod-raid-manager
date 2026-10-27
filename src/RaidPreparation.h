#ifndef RAID_MANAGER_PREPARATION_H
#define RAID_MANAGER_PREPARATION_H
#include "Define.h"
class ChatHandler;
namespace RaidManager
{
    void QueuePreparation(ChatHandler* handler, bool cancel);
    void QueueDifficultyRequest(ChatHandler* handler, uint32 level);
    void RegisterPreparationScripts();
}
#endif
