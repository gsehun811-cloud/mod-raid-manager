#include "RaidWork.h"
#include <array>
#include <chrono>
#include <set>
namespace RaidManager
{
namespace
{
    // Accessed only on the world thread; no Player/AI/Group pointers survive a work step.
    std::array<bool, 2> Pending{};
    std::chrono::steady_clock::time_point Last{};
    std::set<ObjectGuid> Bots;
    std::set<ObjectGuid> Groups;
}
void SetWorkPending(WorkKind kind, bool pending)
{
    Pending[static_cast<std::size_t>(kind)] = pending;
}
bool TakeWorkTurn(WorkKind kind)
{
    if (kind != WorkKind::Recruitment)
        return false; // Preparation has its own globally bounded admission budget.
    auto index = static_cast<std::size_t>(kind);
    auto now = std::chrono::steady_clock::now();
    if (!Pending[index] || now - Last < std::chrono::milliseconds(200))
        return false;
    Last = now;
    return true;
}
bool RecruitmentNeedsPriority()
{
    auto elapsed = std::chrono::steady_clock::now() - Last;
    return elapsed < std::chrono::milliseconds(2) ||
        (Pending[static_cast<std::size_t>(WorkKind::Recruitment)] && elapsed >= std::chrono::milliseconds(200));
}
bool IsBotReserved(ObjectGuid bot)
{
    return Bots.count(bot) != 0;
}
bool TryReserveBot(ObjectGuid bot)
{
    return Bots.insert(bot).second;
}
void ReleaseBot(ObjectGuid bot)
{
    Bots.erase(bot);
}
bool IsGroupReserved(ObjectGuid group)
{
    return Groups.count(group) != 0;
}
bool TryReserveGroup(ObjectGuid group)
{
    return Groups.insert(group).second;
}
void ReleaseGroup(ObjectGuid group)
{
    Groups.erase(group);
}
}
