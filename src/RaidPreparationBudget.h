#ifndef RAID_MANAGER_PREPARATION_BUDGET_H
#define RAID_MANAGER_PREPARATION_BUDGET_H

#include "Define.h"
#include <algorithm>

namespace RaidManager
{
    // Pure monotonic-microsecond admission policy, shared by production and offline tests.
    // Heavy calls cannot be preempted: their complete elapsed cost becomes credit debt.
    class PreparationBudget
    {
    public:
        void Observe(uint64 now, uint32 worldDiff)
        {
            if (!_started)
            {
                _started = true;
                _refillAt = now;
            }
            uint64 elapsed = now - _refillAt;
            _refillAt = now;
            _fraction += std::min<uint64>(elapsed, 10000000) * 30;
            _credit = std::min<int64>(3000, _credit + static_cast<int64>(_fraction / 1000));
            _fraction %= 1000;
            if (worldDiff >= 100)
                _slowUntil = now + 2000000;
            if (worldDiff >= 250)
                _severeUntil = now + 2000000;
        }

        uint32 Begin(uint64 now)
        {
            uint64 interval = now < _severeUntil ? 250000 : now < _slowUntil ? 100000 : 50000;
            uint32 maximum = now < _severeUntil ? 500 : now < _slowUntil ? 1500 : 3000;
            if ((_admitted && now - _last < interval) || _credit < 500)
                return 0;
            _admitted = true;
            _last = now;
            ++slices;
            if (maximum < 3000)
                ++throttled;
            return static_cast<uint32>(std::min<int64>(_credit, maximum));
        }

        void Charge(uint64 elapsed)
        {
            _credit -= static_cast<int64>(elapsed);
            workUs += elapsed;
            maximumSliceUs = std::max(maximumSliceUs, elapsed);
        }

        uint64 slices = 0;
        uint64 throttled = 0;
        uint64 workUs = 0;
        uint64 maximumSliceUs = 0;

    private:
        bool _started = false;
        bool _admitted = false;
        int64 _credit = 3000;
        uint64 _refillAt = 0;
        uint64 _fraction = 0;
        uint64 _last = 0;
        uint64 _slowUntil = 0;
        uint64 _severeUntil = 0;
    };
}

#endif
