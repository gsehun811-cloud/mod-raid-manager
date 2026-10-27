#ifndef RAID_MANAGER_PREPARATION_QUEUE_H
#define RAID_MANAGER_PREPARATION_QUEUE_H

#include <cstddef>

namespace RaidManager
{
    struct PreparationPick
    {
        bool available = false;
        bool admit = false;
        std::size_t index = 0;
    };

    // Preserve serial role assignment/talent commitment, then rotate independent bot work.
    // Cancellation never admits a new bot; the caller persists/releases one queued bot at a time.
    template<class Queue, class RolePending>
    PreparationPick PickPreparationTask(Queue const& ready, bool unstarted, bool cancelled,
        bool& preferAdmission, RolePending rolePending)
    {
        if (!cancelled)
        {
            for (std::size_t i = 0; i < ready.size(); ++i)
                if (rolePending(ready[i]))
                    return {true, false, i};
            if (unstarted && (ready.empty() || preferAdmission))
            {
                preferAdmission = false;
                return {true, true, 0};
            }
        }
        if (ready.empty())
            return {};
        preferAdmission = true;
        return {true, false, 0};
    }
}

#endif
