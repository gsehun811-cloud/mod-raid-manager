#include "RaidGroupRecruitment.h"
#include "RaidPromotionPolicy.h"
#include "RaidWork.h"

#include "AcceptInvitationAction.h"
#include "Chat.h"
#include "Config.h"
#include "Event.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GroupScript.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "Map.h"
#include "InviteToGroupAction.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PlayerScript.h"
#include "RandomPlayerbotMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldScript.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace RaidManager
{
namespace
{
    enum class Role : uint8
    {
        Tank,
        Healer,
        Ranged,
        Melee,
        Count
    };

    constexpr std::size_t RoleCount = static_cast<std::size_t>(Role::Count);
    constexpr std::size_t ClassCount = MAX_CLASSES;

    Role GetRole(Player* player)
    {
        if (PlayerbotAI::IsTank(player, true))
            return Role::Tank;
        if (PlayerbotAI::IsHeal(player, true))
            return Role::Healer;
        return PlayerbotAI::IsRangedDps(player, true) ? Role::Ranged : Role::Melee;
    }

    struct Composition
    {
        std::array<uint32, RoleCount> roles{};
        std::array<uint32, ClassCount> classes{};

        void Add(Player* player)
        {
            ++roles[static_cast<std::size_t>(GetRole(player))];
            ++classes[player->getClass()];
        }

        uint32 Count(Role role) const { return roles[static_cast<std::size_t>(role)]; }
    };

    Composition GetComposition(Player* leader)
    {
        Composition result;
        if (Group* group = leader->GetGroup())
        {
            for (auto const& slot : group->GetMemberSlots())
                if (Player* member = ObjectAccessor::FindPlayer(slot.guid))
                    result.Add(member);
        }
        else
            result.Add(leader);
        return result;
    }

    Group* EnsureRaidGroup(Player* leader)
    {
        Group* group = leader->GetGroup();
        if (!group)
        {
            // Match Playerbots' GroupInviteOperation creation API; joining still uses invite/accept.
            auto created = std::make_unique<Group>();
            if (!created->Create(leader))
                return nullptr;
            group = created.release();
            sGroupMgr->AddGroup(group);
        }
        if (!group->isRaidGroup())
            GroupConvertToRaidOperation(leader->GetGUID()).Execute();
        return leader->GetGroup();
    }

    bool IsAvailable(Player* player)
    {
        return player && player->IsInWorld() && player->GetSession() &&
            !player->GetSession()->IsLoggingOut() && player->IsAlive() && !player->IsInCombat() &&
            !player->IsBeingTeleported() && !player->IsInFlight() && !player->IsSpectator() &&
            !player->InBattleground() && !player->InBattlegroundQueue();
    }

    bool HasInstanceBindings(Player* bot)
    {
        // The core maintains these maps for logged-in players. This is a read-only lookup:
        // reject both permanent and temporary bindings, without removing or changing either.
        for (uint8 difficulty = 0; difficulty < MAX_DIFFICULTY; ++difficulty)
            if (!sInstanceSaveMgr->PlayerGetBoundInstances(bot->GetGUID(), Difficulty(difficulty)).empty())
                return true;
        return false;
    }

    bool IsEligible(Player* leader, Player* bot, Group* expectedInvite = nullptr, bool lowerLevel = false)
    {
        if (!IsAvailable(bot) || bot == leader || bot->GetGroup() || bot->GetOriginalGroup() ||
            (bot->GetGroupInvite() && bot->GetGroupInvite() != expectedInvite) ||
            bot->GetTeamId() != leader->GetTeamId() ||
            (lowerLevel ? bot->GetLevel() >= leader->GetLevel() : bot->GetLevel() != leader->GetLevel()) ||
            !bot->GetMap() || bot->GetMap()->IsDungeon() || HasInstanceBindings(bot))
            return false;

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        return ai && !ai->IsRealPlayer() && sRandomPlayerbotMgr.IsRandomBot(bot) &&
            (!ai->GetMaster() || ai->GetMaster() == leader);
    }

    // Both providers apply the same ownership, online-state and instance-binding protections.
    std::vector<ObjectGuid> CollectOnlineCandidates(Player* leader, bool lowerLevel = false)
    {
        std::vector<ObjectGuid> candidates;
        for (auto itr = sRandomPlayerbotMgr.GetPlayerBotsBegin();
            itr != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++itr)
            if (IsEligible(leader, itr->second, nullptr, lowerLevel))
                candidates.push_back(itr->first);
        return candidates;
    }

    uint32 ScoreCandidate(Player* bot, Composition const& composition, uint32 tankGoal, uint32 healerGoal,
        uint32 remainingSlots)
    {
        Role role = GetRole(bot);
        uint32 tier = 0;
        if (role == Role::Melee && !composition.Count(role) && remainingSlots == 1)
            tier = 5;
        else if (role == Role::Tank && composition.Count(role) < tankGoal)
            tier = 4;
        else if (role == Role::Healer && composition.Count(role) < healerGoal)
            tier = 3;
        else if (role == Role::Melee && !composition.Count(role))
            tier = 2;
        else if (role == Role::Ranged || role == Role::Melee)
            tier = 1;

        // Role deficits dominate diversity; within a tier, prefer less represented classes.
        // Ranged DPS wins equal-class-count ties, while melee gets a reserved first slot above.
        return tier * 10000 + (40 - composition.classes[bot->getClass()]) * 100 +
            (role == Role::Ranged ? 1 : 0);
    }

    uint32 BalancedPriority(Player* bot, Player* leader, Composition const& composition, uint32 size,
        uint32 remaining, std::array<uint32, 5>& rejected)
    {
        uint32 tanks = size == 20 ? 2 : 4;
        uint32 healers = size == 20 ? 5 : 10;
        Role role = GetRole(bot);
        // Leave holes rather than filling them with surplus tanks, healers or repeated melee classes.
        if (composition.classes[bot->getClass()] >= (size + 4) / 5)
        {
            ++rejected[0];
            return 0;
        }
        uint32 deficits = (tanks > composition.Count(Role::Tank) ? tanks - composition.Count(Role::Tank) : 0) +
            (healers > composition.Count(Role::Healer) ? healers - composition.Count(Role::Healer) : 0);
        if ((role == Role::Tank && composition.Count(role) >= tanks) ||
            (role == Role::Healer && composition.Count(role) >= healers))
        {
            ++rejected[1];
            return 0;
        }
        if (role == Role::Ranged || role == Role::Melee)
        {
            if (remaining <= deficits)
            {
                ++rejected[2];
                return 0;
            }
            if (role == Role::Melee &&
                composition.Count(role) >= std::max<uint32>(1, (size - tanks - healers) / 3))
            {
                ++rejected[3];
                return 0;
            }
        }
        uint32 rolePriority = role == Role::Tank ? 5 : role == Role::Healer ? 4 :
            role == Role::Melee && !composition.Count(role) ? 3 : role == Role::Ranged ? 2 : 1;
        return rolePriority * 100000 + (size - composition.classes[bot->getClass()]) * 1000 +
            (bot->GetLevel() == leader->GetLevel() ? 100 : 0) + bot->GetLevel();
    }

    using Clock = std::chrono::steady_clock;
    constexpr auto RecruitmentTimeout = std::chrono::minutes(5);

    struct RecruitmentJob
    {
        ObjectGuid leader;
        ObjectGuid group;
        uint64 logoutEpoch;
        uint32 size;
        std::vector<ObjectGuid> candidates;
        Clock::time_point started;
        std::string reason;
        bool fill = false;
        uint32 targetLevel = 0;
        uint32 promotionCandidatesExamined = 0;
        uint32 promotionAttemptLimit = 0;
        std::array<uint32, 5> fillRejected{};
        uint32 promotionAttempts = 0;
        uint32 promoted = 0;
        uint32 promotedNotJoined = 0;
        uint32 promotionDenied = 0;
        uint32 joinFailures = 0;
        uint32 promotionFailures = 0;
        uint32 partialLevelChanges = 0;
        uint32 saveFailures = 0;
        ObjectGuid pendingBot;
        uint64 pendingBotEpoch = 0;
        uint32 pendingOriginalLevel = 0;
        bool pendingOutcomeRecorded = false;
        bool pendingPromoted = false;
        bool pendingSaveAttempted = false;
        Clock::time_point pendingDeadline{};
        PromotionAssessment lastAssessment;
    };

    struct LifecycleEvent
    {
        ObjectGuid leader;
        ObjectGuid group;
    };

    class InvitationCleanup
    {
    public:
        InvitationCleanup(ObjectGuid bot, ObjectGuid group) : _bot(bot), _group(group) { }
        ~InvitationCleanup()
        {
            if (Player* bot = ObjectAccessor::FindPlayer(_bot))
                if (Group* pending = bot->GetGroupInvite(); pending && pending->GetGUID() == _group)
                    pending->RemoveInvite(bot);
        }

    private:
        ObjectGuid _bot;
        ObjectGuid _group;
    };

    class RecruitmentManager
    {
    public:
        static RecruitmentManager& Instance()
        {
            static RecruitmentManager manager;
            return manager;
        }

        uint64 LogoutEpoch(ObjectGuid leader)
        {
            std::lock_guard<std::mutex> lock(_eventMutex);
            return _logoutEpochs[leader];
        }

        void LoadConfig()
        {
            int32 limit = sConfigMgr->GetOption<int32>("RaidManager.MaxPromotionsPerRun", 0);
            _maxPromotionsPerRun = limit > 0 ? static_cast<uint32>(limit) : 0;
        }

        void NotifyLogout(ObjectGuid player)
        {
            std::lock_guard<std::mutex> lock(_eventMutex);
            ++_logoutEpochs[player];
            _events.push_back({ player, ObjectGuid::Empty });
        }

        void NotifyGroupChange(ObjectGuid group, ObjectGuid member = ObjectGuid::Empty)
        {
            std::lock_guard<std::mutex> lock(_eventMutex);
            _events.push_back({ member, group });
        }

        void Start(ObjectGuid guid, uint64 epoch, uint32 size, bool fill)
        {
            DrainEvents();
            RemoveFinished();
            Player* leader = ObjectAccessor::FindPlayer(guid);
            if (!leader || !leader->GetSession() || LogoutEpoch(guid) != epoch)
                return;

            ChatHandler handler(leader->GetSession());
            if (std::any_of(_jobs.begin(), _jobs.end(), [guid](RecruitmentJob const& job)
                { return job.leader == guid; }))
            {
                handler.SendSysMessage("Raid Manager: recruitment is already active. Use .raidgroup cancel first.");
                return;
            }

            Group* group = leader->GetGroup();
            if (!IsAvailable(leader) || leader->GetGroupInvite() || leader->GetOriginalGroup() ||
                (group && (!group->IsLeader(guid) || group->isLFGGroup() || group->isBGGroup() ||
                    group->isBFGroup() || group->GetInviteeCount() || group->GetMembersCount() > size)))
            {
                handler.SendSysMessage("Raid Manager: recruitment requires an available group leader, no pending "
                    "invitations, and a normal group no larger than the target size.");
                return;
            }

            if (group && IsGroupReserved(group->GetGUID()))
            {
                handler.SendSysMessage("Raid Manager: recruitment/preparation already active for this group.");
                return;
            }
            if (leader->GetMap()->IsRaid() && leader->GetMap()->ToInstanceMap() &&
                size > leader->GetMap()->ToInstanceMap()->GetMaxPlayers())
            {
                handler.SendSysMessage("Raid Manager: requested size exceeds actual raid instance capacity.");
                return;
            }
            auto candidates = CollectOnlineCandidates(leader);
            if (fill)
            {
                auto lower = CollectOnlineCandidates(leader, true);
                candidates.insert(candidates.end(), lower.begin(), lower.end());
            }
            if (!group && candidates.empty() && (!fill || CollectOnlineCandidates(leader, true).empty()))
            {
                handler.PSendSysMessage("Raid Manager: 1/{} members, {} missing. No eligible online random bots "
                    "without instance bindings were found.", size, size - 1);
                if (fill)
                    handler.SendSysMessage("Raid Manager fill: promoted 0, promoted but not joined 0. "
                        "No eligible lower-level candidates were found.");
                return;
            }
            group = EnsureRaidGroup(leader);
            if (!group)
            {
                handler.SendSysMessage("Raid Manager: could not create the raid group.");
                return;
            }

            if (!TryReserveGroup(group->GetGUID()))
                return;
            _jobs.push_back({ guid, group->GetGUID(), epoch, size, std::move(candidates), Clock::now(), {} });
            _jobs.back().fill = fill;
            _jobs.back().targetLevel = leader->GetLevel();
            // Snapshot the limit: reloads never replenish an active command's attempt budget.
            _jobs.back().promotionAttemptLimit = _maxPromotionsPerRun;
            handler.SendSysMessage("Raid Manager: recruitment started. Use .raidgroup cancel to stop. "
                "Playerbots summon and repair settings remain in effect.");
            if (fill)
            {
                handler.SendSysMessage("Raid Manager fill: role balance and class diversity run first; "
                    "equal-level wins ties. "
                    "Eligible lower-level random bots may be permanently promoted. Normal Playerbots level-up "
                    "maintenance is enabled. Promoted bots still require raid preparation.");
                handler.PSendSysMessage("Raid Manager fill: promotion attempt limit {} (0 = unlimited). "
                    "Failed attempts still consume the limit.", _jobs.back().promotionAttemptLimit);
            }
        }

        void Cancel(ObjectGuid leader, uint64 epoch)
        {
            if (LogoutEpoch(leader) != epoch)
                return;
            bool found = false;
            for (RecruitmentJob& job : _jobs)
                if (job.leader == leader)
                {
                    job.reason = "cancelled; members already recruited remain in the group";
                    found = true;
                }
            RemoveFinished();
            if (!found)
                if (Player* player = ObjectAccessor::FindPlayer(leader); player && player->GetSession())
                    ChatHandler(player->GetSession()).SendSysMessage("Raid Manager: no recruitment is active.");
        }

        bool CanAccept(Player* bot, Group* group)
        {
            if (bot->GetGUID() != _accepting.bot || group->GetGUID() != _accepting.group)
                return true;
            // The core calls this after removing the pending invite and before AddMember/ResetInstances.
            DrainEvents();
            bool active = std::any_of(_jobs.begin(), _jobs.end(), [this](RecruitmentJob const& job)
            {
                return job.leader == _accepting.leader && job.group == _accepting.group && job.reason.empty();
            });
            Player* leader = ObjectAccessor::FindPlayer(_accepting.leader);
            bool eligible = active && LogoutEpoch(_accepting.leader) == _accepting.logoutEpoch && IsAvailable(leader) &&
                leader->GetGroup() == group && group->IsLeader(_accepting.leader) &&
                group->isRaidGroup() && !group->isLFGGroup() && !group->isBGGroup() && !group->isBFGroup() &&
                !leader->GetOriginalGroup() && !leader->GetGroupInvite() &&
                group->GetMembersCount() < _accepting.size && !group->GetInviteeCount() &&
                IsEligible(leader, bot, group);
            return eligible && (!_accepting.promoted ||
                AssessPromotion(leader, bot, true).block == PromotionBlock::Allowed);
        }

        void Update()
        {
            DrainEvents();
            for (RecruitmentJob& job : _jobs)
            {
                Player* leader = ObjectAccessor::FindPlayer(job.leader);
                Group* group = leader ? leader->GetGroup() : nullptr;
                if (!IsAvailable(leader) || LogoutEpoch(job.leader) != job.logoutEpoch)
                    job.reason = "leader unavailable or logged out";
                else if (leader->GetLevel() != job.targetLevel)
                    job.reason = "leader level changed; start a new recruitment for the new level";
                else if (!group || group->GetGUID() != job.group || !group->IsLeader(job.leader) ||
                    !group->isRaidGroup() || group->isLFGGroup() || group->isBGGroup() || group->isBFGroup() ||
                    leader->GetOriginalGroup())
                    job.reason = "group disbanded, replaced, or leader changed";
                else if (group->GetMembersCount() >= job.size)
                    job.reason = "target reached or group already exceeds target";
                else if (group->GetInviteeCount() || leader->GetGroupInvite())
                    job.reason = "other invitations are pending; recruitment stopped without cancelling them";
                else if (Clock::now() - job.started >= RecruitmentTimeout)
                    job.reason = "recruitment timed out";
            }
            RemoveFinished();

            SetWorkPending(WorkKind::Recruitment, !_jobs.empty());
            if (!TakeWorkTurn(WorkKind::Recruitment))
                return;

            // Shared global pacing also covers preparation, without catch-up bursts.
            try
            {
                Step(_jobs.front());
            }
            catch (std::exception const& error)
            {
                RecordPromotionOutcome(_jobs.front());
                SavePendingMutation(_jobs.front());
                LOG_ERROR("module.raidmanager", "Recruitment step failed: {}", error.what());
                _jobs.front().reason = "promotion or invitation processing failed; changes were not rolled back";
            }
            catch (...)
            {
                RecordPromotionOutcome(_jobs.front());
                SavePendingMutation(_jobs.front());
                LOG_ERROR("module.raidmanager", "Recruitment step failed with an unknown exception");
                _jobs.front().reason = "promotion or invitation processing failed; changes were not rolled back";
            }
            _accepting = {};
            DrainEvents();
            RemoveFinished();
            if (_jobs.size() > 1)
                std::rotate(_jobs.begin(), _jobs.begin() + 1, _jobs.end());
        }

    private:
        void RecordPromotionOutcome(RecruitmentJob& job)
        {
            if (job.pendingBot.IsEmpty() || job.pendingOutcomeRecorded)
                return;
            job.pendingOutcomeRecorded = true;
            Player* bot = ObjectAccessor::FindConnectedPlayer(job.pendingBot);
            if (bot && LogoutEpoch(job.pendingBot) == job.pendingBotEpoch && bot->GetLevel() == job.targetLevel)
            {
                job.pendingPromoted = true;
                ++job.promoted;
                // Count immediately, so cancellation, logout or group loss before invitation is reported.
                ++job.promotedNotJoined;
            }
            else
            {
                ++job.promotionFailures;
                if (bot && LogoutEpoch(job.pendingBot) == job.pendingBotEpoch &&
                    bot->GetLevel() != job.pendingOriginalLevel)
                    ++job.partialLevelChanges;
            }
        }

        void ReleasePending(RecruitmentJob& job)
        {
            if (job.pendingBot.IsEmpty())
                return;
            if (job.pendingPromoted)
                if (Player* bot = ObjectAccessor::FindConnectedPlayer(job.pendingBot);
                    bot && LogoutEpoch(job.pendingBot) == job.pendingBotEpoch)
                    if (Group* group = bot->GetGroup(); group && group->GetGUID() == job.group)
                        --job.promotedNotJoined;
            ReleaseBot(job.pendingBot);
            job.pendingBot.Clear();
            job.pendingOutcomeRecorded = false;
            job.pendingPromoted = false;
        }

        bool SavePendingMutation(RecruitmentJob& job)
        {
            if (job.pendingBot.IsEmpty() || job.pendingSaveAttempted)
                return true;
            Player* bot = ObjectAccessor::FindConnectedPlayer(job.pendingBot);
            if (!bot || LogoutEpoch(job.pendingBot) != job.pendingBotEpoch ||
                bot->GetLevel() == job.pendingOriginalLevel)
                return true;
            job.pendingSaveAttempted = true;
            try
            {
                // Use normal asynchronous persistence, including its far-teleport save deferral.
                bot->SaveToDB(false, false);
                return true;
            }
            catch (...)
            {
                ++job.saveFailures;
                LOG_ERROR("module.raidmanager", "Could not request a promoted bot's character save");
                return false;
            }
        }

        void DrainEvents()
        {
            std::vector<LifecycleEvent> events;
            {
                std::lock_guard<std::mutex> lock(_eventMutex);
                events.swap(_events);
            }
            for (LifecycleEvent const& event : events)
                for (RecruitmentJob& job : _jobs)
                    if ((event.group.IsEmpty() && event.leader == job.leader) ||
                        (event.group == job.group && (event.leader.IsEmpty() || event.leader == job.leader)))
                        job.reason = "leader logged out, left the group, or group leadership/lifetime changed";
        }

        void RemoveFinished()
        {
            for (auto itr = _jobs.begin(); itr != _jobs.end();)
            {
                if (itr->reason.empty())
                {
                    ++itr;
                    continue;
                }
                ReleasePending(*itr);
                Report(*itr);
                ReleaseGroup(itr->group);
                itr = _jobs.erase(itr);
            }
        }

        static void Report(RecruitmentJob const& job)
        {
            Player* leader = ObjectAccessor::FindPlayer(job.leader);
            if (!leader || !leader->GetSession() || leader->GetSession()->IsLoggingOut())
                return;
            ChatHandler handler(leader->GetSession());
            handler.PSendSysMessage("Raid Manager: recruitment ended: {}.", job.reason);
            if (job.fill)
            {
                handler.PSendSysMessage("Raid Manager fill: last scan exclusions: class quota {}, surplus role {}, "
                    "reserved tank/healer vacancies {}, melee quota {}, promotion budget {}.",
                    job.fillRejected[0], job.fillRejected[1], job.fillRejected[2], job.fillRejected[3],
                    job.fillRejected[4]);
                handler.PSendSysMessage("Raid Manager fill: promoted {}, promoted but not joined {}, "
                    "promotion denied {}, promotion failures {}, join failures {}, candidates examined {}.",
                    job.promoted, job.promotedNotJoined, job.promotionDenied, job.promotionFailures, job.joinFailures,
                    job.promotionCandidatesExamined);
                handler.PSendSysMessage("Raid Manager fill: raid preparation incomplete for {} promoted bots. "
                    "Partial level changes {}, save request failures {}. Existing changes are not rolled back.",
                    job.promoted, job.partialLevelChanges, job.saveFailures);
                handler.PSendSysMessage("Raid Manager fill: promotion attempts {}/{}. "
                    "Automatic maintenance completion is unverified.",
                    job.promotionAttempts, job.promotionAttemptLimit);
                if (job.promotionCandidatesExamined)
                    handler.PSendSysMessage("Raid Manager fill: last checked limits: server {}, Playerbots {}, "
                        "progression {} (0 = unverified), bot account confirmed {}. {}.",
                        job.lastAssessment.serverLimit,
                        job.lastAssessment.playerbotsLimit, job.lastAssessment.progressionLimit,
                        job.lastAssessment.botAccountVerified ? "yes" : "no",
                        DescribePromotionBlock(job.lastAssessment.block));
            }
            Group* group = leader->GetGroup();
            if (!group || group->GetGUID() != job.group)
                return;
            Composition composition = GetComposition(leader);
            uint32 count = group->GetMembersCount();
            handler.PSendSysMessage("Raid Manager: {}/{} members (including you), {} missing. "
                "Tanks {}/{}, healers {}/{}, ranged DPS {}, melee DPS {}.",
                count, job.size, count < job.size ? job.size - count : 0,
                composition.Count(Role::Tank), job.size == 20 ? 2 : 4,
                composition.Count(Role::Healer), job.size == 20 ? 5 : 10,
                composition.Count(Role::Ranged), composition.Count(Role::Melee));
        }

        void PromoteFillCandidate(RecruitmentJob& job, Player* leader, Group* group, ObjectGuid selected)
        {
            if (job.promotionAttemptLimit && job.promotionAttempts >= job.promotionAttemptLimit)
            {
                job.reason = "promotion attempt limit reached for this command";
                return;
            }
            ObjectGuid best = selected;
            job.candidates.erase(std::remove(job.candidates.begin(), job.candidates.end(), best), job.candidates.end());
            Player* bot = ObjectAccessor::FindPlayer(best);
            if (!IsEligible(leader, bot, nullptr, true))
                return;
            job.lastAssessment = AssessPromotion(leader, bot);
            ++job.promotionCandidatesExamined;
            if (job.lastAssessment.block != PromotionBlock::Allowed)
            {
                ++job.promotionDenied;
                if (job.lastAssessment.block != PromotionBlock::BotAccountUnverified || job.candidates.empty())
                    job.reason = DescribePromotionBlock(job.lastAssessment.block);
                return;
            }

            // Classification reads can take time; recheck the live objects and bindings immediately
            // before mutation. A reservation prevents another Raid Manager job inviting this bot.
            DrainEvents();
            if (!job.reason.empty() || !IsAvailable(leader) || leader->GetLevel() != job.targetLevel ||
                leader->GetGroup() != group || !group->IsLeader(job.leader) ||
                group->GetMembersCount() >= job.size || group->GetInviteeCount() ||
                !IsEligible(leader, bot, nullptr, true) || !TryReserveBot(best))
                return;

            job.pendingBot = best;
            job.pendingBotEpoch = LogoutEpoch(best);
            job.pendingOriginalLevel = bot->GetLevel();
            job.pendingOutcomeRecorded = false;
            job.pendingPromoted = false;
            job.pendingSaveAttempted = false;
            job.pendingDeadline = Clock::now() + std::chrono::seconds(15);

            // Deliberately use only the normal core level-up API. Its packet triggers the permitted
            // Playerbots maintenance on later AI updates. No factory/reset/unbind APIs are invoked.
            // Consume immediately before calling, including vetoes, partial changes and exceptions.
            ++job.promotionAttempts;
            bot->GiveLevel(static_cast<uint8>(job.targetLevel));
            RecordPromotionOutcome(job);
            if (!SavePendingMutation(job))
            {
                job.reason = "promotion changed the bot, but the save request failed; no rollback performed";
                return;
            }

            ChatHandler handler(leader->GetSession());
            if (!job.pendingPromoted)
            {
                handler.PSendSysMessage("Raid Manager fill: promotion failed for {} (level {} -> {}, target {}). "
                    "Any changes are retained.", bot->GetName(), job.pendingOriginalLevel, bot->GetLevel(),
                    job.targetLevel);
                ReleasePending(job);
                return;
            }
            handler.PSendSysMessage("Raid Manager fill: {} promoted {} -> {}; save requested. "
                "Raid preparation incomplete; automatic maintenance completion is unverified. "
                "Group invitation will be attempted on a later update.",
                bot->GetName(), job.pendingOriginalLevel, bot->GetLevel());
        }

        bool InviteCandidate(RecruitmentJob& job, Player* leader, Player* bot, bool promoted)
        {
            {
                _accepting = { job.leader, job.group, bot->GetGUID(), job.logoutEpoch, job.size, promoted };
                InvitationCleanup cleanup(bot->GetGUID(), job.group);
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                InviteToGroupAction invite(ai);
                invite.Invite(leader, bot);

                Group* group = leader->GetGroup();
                Group* invitation = bot->GetGroupInvite();
                if (group && group->GetGUID() == job.group && group->IsLeader(job.leader) &&
                    group->GetMembersCount() < job.size && group->GetInviteeCount() == 1 &&
                    invitation == group && IsEligible(leader, bot, group))
                {
                    WorldPacket packet;
                    packet << uint8(1) << leader->GetName();
                    AcceptInvitationAction accept(ai);
                    accept.Execute(Event("group invite", packet, leader));
                }
            }
            bool joined = bot->GetGroup() && bot->GetGroup()->GetGUID() == job.group;
            if (!joined)
                ++job.joinFailures;
            return joined;
        }

        void JoinPromotedBot(RecruitmentJob& job, Player* leader)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(job.pendingBot);
            if (bot && LogoutEpoch(job.pendingBot) == job.pendingBotEpoch && bot->GetSession() &&
                !bot->GetSession()->IsLoggingOut() && bot->IsBeingTeleported() && Clock::now() < job.pendingDeadline)
                return;

            bool joined = bot && LogoutEpoch(job.pendingBot) == job.pendingBotEpoch &&
                bot->GetGroup() && bot->GetGroup()->GetGUID() == job.group;
            if (!joined)
            {
                if (!bot || LogoutEpoch(job.pendingBot) != job.pendingBotEpoch || !IsEligible(leader, bot))
                    ++job.joinFailures;
                else
                {
                    job.lastAssessment = AssessPromotion(leader, bot, true);
                    if (job.lastAssessment.block == PromotionBlock::Allowed)
                        joined = InviteCandidate(job, leader, bot, true);
                    else
                    {
                        ++job.promotionDenied;
                        ++job.joinFailures;
                    }
                }
            }

            ChatHandler(leader->GetSession()).PSendSysMessage("Raid Manager fill: promoted bot {}. "
                "Raid preparation incomplete; automatic maintenance completion is unverified. "
                "Previous level and maintenance changes are retained.",
                joined ? "joined the group" : "could not join the group");
            ReleasePending(job);
            Group* group = leader->GetGroup();
            if (!group || group->GetGUID() != job.group || !group->IsLeader(job.leader))
                job.reason = "group disbanded or leader changed during invitation";
            else if (group->GetMembersCount() >= job.size)
                job.reason = "target reached";
            else if (job.candidates.empty())
                job.reason = "balanced candidate pool exhausted";
        }

        void Step(RecruitmentJob& job)
        {
            Player* leader = ObjectAccessor::FindPlayer(job.leader);
            Group* group = leader ? leader->GetGroup() : nullptr;
            if (!IsAvailable(leader) || !group || group->GetGUID() != job.group ||
                !group->IsLeader(job.leader) || group->GetMembersCount() >= job.size || group->GetInviteeCount() ||
                (leader->GetMap()->IsRaid() && leader->GetMap()->ToInstanceMap() &&
                    job.size > leader->GetMap()->ToInstanceMap()->GetMaxPlayers()))
            {
                job.reason = "group or leader state changed";
                return;
            }

            if (!job.pendingBot.IsEmpty())
            {
                JoinPromotedBot(job, leader);
                return;
            }
            Composition composition = GetComposition(leader);
            ObjectGuid best;
            uint32 bestPriority = 0;
            // Recheck every candidate: bindings, login, level, combat, group and master may have changed.
            job.candidates.erase(std::remove_if(job.candidates.begin(), job.candidates.end(),
                [this, leader, &job](ObjectGuid guid)
                {
                    auto bot = ObjectAccessor::FindPlayer(guid);
                    return IsBotReserved(guid) || !IsEligible(leader, bot, nullptr,
                        job.fill && bot && bot->GetLevel() < leader->GetLevel());
                }),
                job.candidates.end());
            job.fillRejected = {};
            for (ObjectGuid guid : job.candidates)
            {
                Player* bot = ObjectAccessor::FindPlayer(guid);
                uint32 priority = job.fill ? BalancedPriority(bot, leader, composition, job.size,
                    job.size - group->GetMembersCount(), job.fillRejected) :
                    ScoreCandidate(bot, composition, job.size == 20 ? 2 : 4,
                        job.size == 20 ? 5 : 10, job.size - group->GetMembersCount());
                if (!priority)
                    continue;
                if (bot->GetLevel() < leader->GetLevel() && job.promotionAttemptLimit &&
                    job.promotionAttempts >= job.promotionAttemptLimit)
                {
                    ++job.fillRejected[4];
                    continue;
                }
                if (best.IsEmpty() || priority > bestPriority)
                {
                    best = guid;
                    bestPriority = priority;
                }
            }
            if (best.IsEmpty())
            {
                job.reason = job.fill ?
                    "no balanced eligible candidates: role/class quotas, promotion budget or candidate state" :
                    "candidate pool exhausted";
                return;
            }
            if (job.fill && ObjectAccessor::FindPlayer(best)->GetLevel() < leader->GetLevel())
            {
                PromoteFillCandidate(job, leader, group, best);
                return;
            }
            job.candidates.erase(std::remove(job.candidates.begin(), job.candidates.end(), best), job.candidates.end());
            Player* bot = ObjectAccessor::FindPlayer(best);
            if (!IsEligible(leader, bot))
                return;

            InviteCandidate(job, leader, bot, false);

            group = leader->GetGroup();
            if (!group || group->GetGUID() != job.group || !group->IsLeader(job.leader))
                job.reason = "group disbanded or leader changed during invitation";
            else if (group->GetMembersCount() >= job.size)
                job.reason = "target reached";
            else if (job.candidates.empty())
                job.reason = "candidate pool exhausted";
        }

        // Only the world thread accesses jobs. Hooks can post invalidation events without erasing
        // an active Step's job. Logout epochs also invalidate commands queued by an old login session.
        std::deque<RecruitmentJob> _jobs;
        uint32 _maxPromotionsPerRun = 0;
        std::mutex _eventMutex;
        std::vector<LifecycleEvent> _events;
        std::map<ObjectGuid, uint64> _logoutEpochs;
        struct AcceptanceContext
        {
            ObjectGuid leader;
            ObjectGuid group;
            ObjectGuid bot;
            uint64 logoutEpoch;
            uint32 size;
            bool promoted = false;
            // No pointers to players, sessions, groups or AI objects survive the current step.
        } _accepting{};
    };

    class RecruitmentControlOperation final : public PlayerbotOperation
    {
    public:
        RecruitmentControlOperation(ObjectGuid leader, uint32 size, uint64 epoch, bool fill)
            : _leader(leader), _size(size), _epoch(epoch), _fill(fill) { }

        uint32 GetPriority() const override { return 50; }
        std::string GetName() const override { return "RaidManagerRecruitmentControl"; }
        bool Execute() override
        {
            if (_size)
                RecruitmentManager::Instance().Start(_leader, _epoch, _size, _fill);
            else
                RecruitmentManager::Instance().Cancel(_leader, _epoch);
            return true;
        }

    private:
        ObjectGuid _leader;
        uint32 _size;
        uint64 _epoch;
        bool _fill;
    };

    class RecruitmentWorldScript final : public WorldScript
    {
    public:
        RecruitmentWorldScript() : WorldScript("raid_manager_recruitment_world",
            { WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }
        void OnAfterConfigLoad(bool /*reload*/) override { RecruitmentManager::Instance().LoadConfig(); }
        void OnUpdate(uint32 /*diff*/) override { RecruitmentManager::Instance().Update(); }
    };

    class RecruitmentPlayerScript final : public PlayerScript
    {
    public:
        RecruitmentPlayerScript() : PlayerScript("raid_manager_recruitment_player",
            { PLAYERHOOK_ON_BEFORE_LOGOUT, PLAYERHOOK_CAN_GROUP_ACCEPT }) { }
        void OnPlayerBeforeLogout(Player* player) override
        {
            RecruitmentManager::Instance().NotifyLogout(player->GetGUID());
        }
        bool OnPlayerCanGroupAccept(Player* player, Group* group) override
        {
            return RecruitmentManager::Instance().CanAccept(player, group);
        }
    };

    class RecruitmentGroupScript final : public GroupScript
    {
    public:
        RecruitmentGroupScript() : GroupScript("raid_manager_recruitment_group",
            { GROUPHOOK_ON_DISBAND, GROUPHOOK_ON_CHANGE_LEADER, GROUPHOOK_ON_REMOVE_MEMBER }) { }
        void OnDisband(Group* group) override
        {
            RecruitmentManager::Instance().NotifyGroupChange(group->GetGUID());
        }
        void OnChangeLeader(Group* group, ObjectGuid /*newLeader*/, ObjectGuid /*oldLeader*/) override
        {
            RecruitmentManager::Instance().NotifyGroupChange(group->GetGUID());
        }
        void OnRemoveMember(Group* group, ObjectGuid member, RemoveMethod /*method*/, ObjectGuid /*kicker*/,
            char const* /*reason*/) override
        {
            RecruitmentManager::Instance().NotifyGroupChange(group->GetGUID(), member);
        }
    };

    void QueueControl(ChatHandler* handler, uint32 size, bool fill = false)
    {
        Player* leader = handler->GetPlayer();
        if (!leader)
            return;
        uint64 epoch = RecruitmentManager::Instance().LogoutEpoch(leader->GetGUID());
        if (PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<RecruitmentControlOperation>(leader->GetGUID(), size, epoch, fill)))
            handler->SendSysMessage("Raid Manager: recruitment request queued.");
        else
            handler->SendSysMessage("Raid Manager: request queue is full. Try again later.");
    }
}

void QueueRecruitment(ChatHandler* handler, uint32 targetSize, bool fill)
{
    if (targetSize == 20 || targetSize == 40)
        QueueControl(handler, targetSize, fill);
}

void QueueCancellation(ChatHandler* handler)
{
    QueueControl(handler, 0);
}

void RegisterRecruitmentScripts()
{
    new RecruitmentWorldScript();
    new RecruitmentPlayerScript();
    new RecruitmentGroupScript();
}
}
