#ifndef RAID_MANAGER_GROUP_RECRUITMENT_H
#define RAID_MANAGER_GROUP_RECRUITMENT_H

#include "Define.h"

class ChatHandler;

namespace RaidManager
{
    void QueueRecruitment(ChatHandler* handler, uint32 targetSize, bool fill = false);
    void QueueCancellation(ChatHandler* handler);
    void RegisterRecruitmentScripts();
}

#endif
