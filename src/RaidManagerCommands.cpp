#include "Chat.h"
#include "CommandScript.h"
#include "RBAC.h"
#include "Player.h"
#include "Map.h"
#include "RaidGroupRecruitment.h"
#include "RaidPreparation.h"

#include <string>

using namespace Acore::ChatCommands;

// Recruitment is isolated from raid preparation and difficulty changes.
class raid_manager_commandscript final : public CommandScript
{
public:
    raid_manager_commandscript() : CommandScript("raid_manager_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "raidgroup", HandleRaidGroup, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "raidprep", HandleRaidPrep, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "raiddiff", HandleRaidDiff, rbac::RBAC_PERM_COMMAND_GM, Console::No }
        };
        return commandTable;
    }

private:
    // Only raid-specific preparation and difficulty require the actual raid map.
    // A group formed outside does not tell us which raid tier to use.
    static bool RequireRaidInstance(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player || !player->GetMap() || !player->GetMap()->IsRaid())
        {
            handler->SendSysMessage("Raid Manager: enter a raid instance before using raidprep or raiddiff. "
                "Outside, use .raidgroup 20 or .raidgroup 40 to form a group.");
            return false;
        }
        return true;
    }

    // Group assembly can also be requested outside an instance.
    static bool HandleRaidGroup(ChatHandler* handler, std::string const& argument, Optional<std::string> option)
    {
        if ((option && *option != "fill") || (argument == "cancel" && option))
        {
            handler->SendSysMessage("Raid Manager: usage .raidgroup 20 [fill], .raidgroup 40 [fill], "
                "or .raidgroup cancel");
            return true;
        }
        if (argument == "cancel")
        {
            RaidManager::QueueCancellation(handler);
            return true;
        }

        if (argument != "20" && argument != "40")
        {
            handler->SendSysMessage("Raid Manager: usage .raidgroup 20 [fill], .raidgroup 40 [fill], "
                "or .raidgroup cancel");
            return true;
        }

        if (!handler->GetPlayer())
        {
            handler->SendSysMessage("Raid Manager: this command requires an in-game player.");
            return true;
        }

        RaidManager::QueueRecruitment(handler, argument == "20" ? 20 : 40, option.has_value());
        return true;
    }

    static bool HandleRaidPrep(ChatHandler* handler, Optional<std::string> argument)
    {
        if (argument && *argument != "cancel")
        {
            handler->SendSysMessage("Raid Manager: usage .raidprep or .raidprep cancel");
            return true;
        }
        RaidManager::QueuePreparation(handler, argument.has_value());
        return true;
    }

    static bool HandleRaidDiff(ChatHandler* handler, uint32 level)
    {
        if (level < 1 || level > 7)
        {
            handler->SendSysMessage("Raid Manager prototype: usage .raiddiff 1 through .raiddiff 7");
            return true;
        }

        if (!RequireRaidInstance(handler))
            return true;

        RaidManager::QueueDifficultyRequest(handler, level);
        return true;
    }
};

void AddRaidManagerCommandScripts()
{
    new raid_manager_commandscript();
    RaidManager::RegisterRecruitmentScripts();
    RaidManager::RegisterPreparationScripts();
}
