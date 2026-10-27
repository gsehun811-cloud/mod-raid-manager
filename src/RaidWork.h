#ifndef RAID_MANAGER_WORK_H
#define RAID_MANAGER_WORK_H
#include "ObjectGuid.h"
namespace RaidManager
{
    enum class WorkKind
    {
        Recruitment,
        Preparation
    };
    void SetWorkPending(WorkKind kind, bool pending);
    bool TakeWorkTurn(WorkKind kind);
    bool RecruitmentNeedsPriority();
    bool IsBotReserved(ObjectGuid bot);
    bool TryReserveBot(ObjectGuid bot);
    void ReleaseBot(ObjectGuid bot);
    bool IsGroupReserved(ObjectGuid group);
    bool TryReserveGroup(ObjectGuid group);
    void ReleaseGroup(ObjectGuid group);
}
#endif
