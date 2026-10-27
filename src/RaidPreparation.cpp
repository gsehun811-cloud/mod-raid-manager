#include "RaidPreparation.h"
#include "RaidPreparationBudget.h"
#include "RaidPreparationQueue.h"
#include "RaidEquipmentSources.h"
#include "RaidEquipmentSelection.h"
#include "RaidPreparationEquipment.h"
#include "RaidPreparationProfiles.h"
#include "RaidPromotionPolicy.h"
#include "RaidWork.h"
#include "AiFactory.h"
#include "BisListMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Event.h"
#include "Engine.h"
#include "Group.h"
#include "GroupScript.h"
#include "InstanceSaveMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PlayerScript.h"
#include "RandomPlayerbotMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
namespace RaidManager
{
namespace
{
    enum class Stage
    {
        Basics,
        Talents,
        Catalog,
        Spells,
        Skills,
        GearPlan,
        GearSlot,
        Strategies,
        Verify,
        Save
    };
    enum Diagnostic : std::size_t { Talents, Spells, Skills, Bis, Gear, RoleAI, RaidAI, DiagnosticCount };
    enum Result : std::size_t { Unchecked, Success, Partial, Failed, Unsupported, ResultCount };
    std::array<char const*, DiagnosticCount> const DiagnosticNames =
        {"talents", "spells", "skills", "BiS", "gear", "role AI", "raid AI"};
    struct PreparationMetrics
    {
        bool started = false;
        bool turnOpen = false;
        Stage activeStage = Stage::Basics;
        std::array<uint32, 10> turns{};
        std::array<uint64, 10> workUs{};
        std::array<uint64, 10> wallUs{};
        std::chrono::steady_clock::time_point startedAt{};
        std::chrono::steady_clock::time_point stageEntered{};
        std::chrono::steady_clock::time_point turnEntered{};
    };
    struct BotTask
    {
        ObjectGuid guid;
        uint64 epoch = 0;
        PrepRole role = PrepRole::Melee;
        Stage stage = Stage::Basics;
        bool dirty = false;
        bool talents = false;
        bool ai = false;
        bool invalidated = false;
        bool nonAiIssue = false;
        std::array<Result, DiagnosticCount> results{};
        uint32 gearApplied = 0;
        uint32 gearSkipped = 0;
        uint32 gearFailed = 0;
        std::vector<std::string> issues;
        std::map<uint8, uint32> equipment;
        std::map<uint8, uint32> bisEquipment;
        EquipmentSelection selection;
        bool choiceReady = false;
        uint32 equipAttempts = 0;
        uint32 retained = 0;
        std::chrono::steady_clock::time_point admittedAt{};
        bool gearEnabled = false;
        uint8 slot = 0;
        std::size_t trainer = 0;
        std::size_t spell = 0;
        std::size_t skill = 0;
        uint32 steps = 0;
        PreparationMetrics timing;
    };

    uint64 PreparationElapsedUs(std::chrono::steady_clock::time_point start,
        std::chrono::steady_clock::time_point end)
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }

    uint64 PreparationNowUs()
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void EndPreparationTurn(BotTask& task)
    {
        if (task.timing.turnOpen)
        {
            task.timing.workUs[static_cast<std::size_t>(task.timing.activeStage)] +=
                PreparationElapsedUs(task.timing.turnEntered, std::chrono::steady_clock::now());
            task.timing.turnOpen = false;
        }
    }

    class PreparationStageTurn
    {
    public:
        explicit PreparationStageTurn(BotTask& task) : _task(task), _guid(task.guid)
        {
            auto now = std::chrono::steady_clock::now();
            auto& timing = task.timing;
            if (!timing.started)
            {
                timing.started = true;
                timing.startedAt = timing.stageEntered = now;
                timing.activeStage = task.stage;
            }
            if (timing.activeStage != task.stage)
            {
                timing.wallUs[static_cast<std::size_t>(timing.activeStage)] +=
                    PreparationElapsedUs(timing.stageEntered, now);
                timing.activeStage = task.stage;
                timing.stageEntered = now;
            }
            ++timing.turns[static_cast<std::size_t>(task.stage)];
            timing.turnEntered = now;
            timing.turnOpen = true;
        }

        ~PreparationStageTurn()
        {
            if (_task.guid == _guid)
                EndPreparationTurn(_task);
        }

    private:
        BotTask& _task;
        ObjectGuid _guid;
    };

    std::string PreparationTimingSummary(BotTask const& task)
    {
        auto const& timing = task.timing;
        if (!timing.started)
            return "stages-not-started";
        auto now = std::chrono::steady_clock::now();
        std::string result = Acore::StringFormat("wall_ms={}", PreparationElapsedUs(timing.startedAt, now) / 1000);
        char const* names[] =
            {"basics", "talents", "catalog", "spells", "skills", "plan", "gear", "AI", "verify", "save"};
        for (std::size_t stage = 0; stage < timing.turns.size(); ++stage)
        {
            uint64 wall = timing.wallUs[stage] + (stage == static_cast<std::size_t>(timing.activeStage) ?
                PreparationElapsedUs(timing.stageEntered, now) : 0);
            result += Acore::StringFormat(" {}[turns={},wall_ms={},work_us={}]",
                names[stage], timing.turns[stage], wall / 1000, timing.workUs[stage]);
        }
        return result;
    }

    struct PrepJob
    {
        ObjectGuid leader;
        uint64 epoch;
        ObjectGuid group;
        uint32 instance;
        RaidProfile profile;
        std::vector<ObjectGuid> bots;
        std::size_t cursor = 0;
        BotTask current;
        uint32 complete = 0;
        uint32 partial = 0;
        uint32 failed = 0;
        uint32 skipped = 0;
        std::string stopped;
        std::array<std::array<uint32, ResultCount>, DiagnosticCount> results{};
        uint32 gearApplied = 0;
        uint32 gearSkipped = 0;
        uint32 gearFailed = 0;
        uint32 aiOnlyPartial = 0;
        CandidateIndexValidation indexValidation;
        std::deque<BotTask> ready;
        bool preferAdmission = true;
        std::chrono::steady_clock::time_point requestedAt = std::chrono::steady_clock::now();
        uint64 microsteps = 0;
        uint64 workUs = 0;
        uint64 maximumStepUs = 0;
        std::array<uint64, 10> worldDiffBins{};
        uint32 worldMaximumDiff = 0;
    };
    std::array<uint32, 10> const WorldDiffBounds = {5, 10, 20, 30, 50, 75, 100, 150, 250, 0xffffffff};

    uint32 WorldDiffPercentile(PrepJob const& job, uint32 percent)
    {
        uint64 frames = 0;
        for (auto count : job.worldDiffBins)
            frames += count;
        uint64 cumulative = 0;
        for (std::size_t i = 0; i < job.worldDiffBins.size(); ++i)
        {
            cumulative += job.worldDiffBins[i];
            if (cumulative * 100 >= frames * percent)
                return std::min(WorldDiffBounds[i], job.worldMaximumDiff);
        }
        return 0;
    }
    bool Contains(std::vector<std::string> const& list, std::string const& value)
    {
        return std::find(list.begin(), list.end(), value) != list.end();
    }
    void Issue(BotTask& task, std::string const& reason, Diagnostic category = Spells)
    {
        if (task.results[category] != Failed)
            task.results[category] = Partial;
        if (category != RoleAI && category != RaidAI)
            task.nonAiIssue = true;
        if (!Contains(task.issues, reason))
            task.issues.push_back(reason);
    }
    char const* InstanceStrategy(uint32 map)
    {
        switch (map)
        {
            case 249: return "onyxia";
            case 409: return "moltencore";
            case 469: return "bwl";
            case 509: return "aq20";
            case 532: return "karazhan";
            case 533: return "naxx";
            case 534: return "hyjal";
            case 544: return "magtheridon";
            case 548: return "ssc";
            case 550: return "tempestkeep";
            case 564: return "blacktemple";
            case 565: return "gruulslair";
            case 568: return "zulaman";
            case 603: return "ulduar";
            case 615: return "wotlk-os";
            case 616: return "wotlk-eoe";
            case 624: return "voa";
            case 631: return "icc";
            case 724: return "rs";
            default: return "";
        }
    }
    std::vector<uint32> TemplateIds;
    std::vector<uint32> ClassTrainers;
    std::size_t CatalogCursor = 0;
    bool CatalogStarted = false;
    std::set<Trainer::Trainer const*> SeenTrainers;
    bool DiscoverTrainers()
    {
        if (!CatalogStarted)
        {
            for (auto const& entry : *sObjectMgr->GetCreatureTemplates())
                TemplateIds.push_back(entry.first);
            CatalogStarted = true;
        }
        std::size_t end = std::min(CatalogCursor + 512, TemplateIds.size());
        for (; CatalogCursor < end; ++CatalogCursor)
            if (auto trainer = sObjectMgr->GetTrainer(TemplateIds[CatalogCursor]);
                trainer && trainer->GetTrainerType() == Trainer::Type::Class && SeenTrainers.insert(trainer).second)
                ClassTrainers.push_back(TemplateIds[CatalogCursor]);
        return CatalogCursor == TemplateIds.size();
    }
    bool LearnClassSpell(Player* bot, uint32 id, std::string& reason)
    {
        auto info = sSpellMgr->GetSpellInfo(id);
        if (!info || !info->IsSpellValid() || info->SpellLevel > bot->GetLevel())
        {
            reason = "invalid or above-level class trainer spell";
            return false;
        }
        for (auto const& effect : info->Effects)
            if (effect.Effect == SPELL_EFFECT_SKILL_STEP || effect.Effect == SPELL_EFFECT_TRADE_SKILL ||
                effect.ApplyAuraName == SPELL_AURA_MOUNTED)
            {
                reason = "profession/riding trainer effect excluded";
                return false;
            }
        auto bounds = sSpellMgr->GetSkillLineAbilityMapBounds(id);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (auto skill = sSkillLineStore.LookupEntry(itr->second->SkillLine); skill &&
                (skill->id == SKILL_RIDING || skill->categoryId == SKILL_CATEGORY_PROFESSION ||
                    skill->categoryId == SKILL_CATEGORY_SECONDARY))
            {
                reason = "profession/riding skill-line spell excluded";
                return false;
            }
        if (!bot->HasSpell(id))
            bot->learnSpell(id, false);
        if (!bot->HasSpell(id))
        {
            reason = "class spell learning verification failed";
            return false;
        }
        return true;
    }
    class PreparationManager
    {
    public:
        static PreparationManager& Instance()
        {
            static PreparationManager manager;
            return manager;
        }
        uint64 Epoch(ObjectGuid guid)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _epochs[guid];
        }
        void Logout(Player* player)
        {
            ObjectGuid guid = player->GetGUID();
            std::lock_guard<std::mutex> lock(_mutex);
            ++_epochs[guid];
        }
        void MemberChanged(ObjectGuid guid)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _changedMembers.insert(guid);
        }
        void GroupLost(ObjectGuid group)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _lostGroups.insert(group);
        }
        void Request(ObjectGuid leaderGuid, uint64 epoch, bool cancel, uint32 difficulty)
        {
            if (Epoch(leaderGuid) != epoch)
                return;
            auto leader = ObjectAccessor::FindPlayer(leaderGuid);
            if (!leader || !leader->GetSession())
                return;
            ChatHandler handler(leader->GetSession());
            if (cancel)
            {
                bool found = false;
                for (auto& job : _jobs)
                    if (job.leader == leaderGuid)
                    {
                        job.stopped = "cancelled; completed character changes retained";
                        found = true;
                    }
                if (!found)
                    handler.SendSysMessage("Raid Manager: no preparation is active.");
                return;
            }
            auto group = leader->GetGroup();
            auto map = leader->GetMap();
            if (!map || !map->IsRaid() || !map->GetInstanceId() || !map->ToInstanceMap() || !group ||
                !group->isRaidGroup() || !group->IsLeader(leaderGuid) || group->isLFGGroup() ||
                group->isBGGroup() || group->isBFGroup() || leader->IsInCombat() || leader->IsBeingTeleported())
            {
                handler.SendSysMessage("Raid Manager: enter a real raid instance as raid leader, out of combat.");
                return;
            }
            if (difficulty)
            {
                if (difficulty == 1 && _baseline.count({group->GetGUID(), map->GetInstanceId()}))
                    handler.SendSysMessage("Raid Manager: policy already level 1, no extra buffs. "
                        "This does not imply that every bot completed preparation.");
                else
                    handler.SendSysMessage("Raid Manager: manual difficulty changes are not implemented. "
                        "No gear or buffs changed. Use .raidprep for baseline level 1.");
                return;
            }
            auto profile = GetRaidProfile(map);
            if (!profile.capacity || group->GetMembersCount() > profile.capacity)
            {
                handler.SendSysMessage("Raid Manager: group exceeds actual capacity, or capacity is unknown.");
                return;
            }
            if (!TryReserveGroup(group->GetGUID()))
            {
                handler.SendSysMessage("Raid Manager: recruitment or preparation already active for this group.");
                return;
            }
            PrepJob job{leaderGuid, epoch, group->GetGUID(), map->GetInstanceId(), profile, {}};
            for (auto const& slot : group->GetMemberSlots())
                if (slot.guid != leaderGuid)
                    job.bots.push_back(slot.guid);
            _jobs.push_back(std::move(job));
            _baseline.insert({group->GetGUID(), map->GetInstanceId()});
            LOG_INFO("module.raidmanager", "Prep leader {} group {} map {} instance {} difficulty {} "
                "capacity {} profile {} BiS key {} (live cache checked separately per bot)",
                leaderGuid.ToString(), group->GetGUID().ToString(), profile.map, map->GetInstanceId(),
                profile.difficulty, profile.capacity, profile.name, profile.bisKey);
            handler.PSendSysMessage("Raid Manager: baseline 1, no extra buffs. Map {}, instance {}, "
                "difficulty {}, capacity {}, profile {}, BiS key {}. Queued {} group members.",
                profile.map, map->GetInstanceId(), profile.difficulty, profile.capacity,
                profile.name.empty() ? "unknown (gear skipped)" : profile.name,
                profile.bisKey, _jobs.back().bots.size());
        }
        void Update(uint32 worldDiff)
        {
            _budget.Observe(PreparationNowUs(), worldDiff);
            std::set<ObjectGuid> lost;
            std::set<ObjectGuid> changed;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                lost.swap(_lostGroups);
                changed.swap(_changedMembers);
            }
            for (auto itr = _baseline.begin(); itr != _baseline.end();)
                if (lost.count(itr->first))
                    itr = _baseline.erase(itr);
                else
                    ++itr;
            for (auto& job : _jobs)
            {
                auto bin = std::lower_bound(WorldDiffBounds.begin(), WorldDiffBounds.end(), worldDiff);
                ++job.worldDiffBins[static_cast<std::size_t>(bin - WorldDiffBounds.begin())];
                job.worldMaximumDiff = std::max(job.worldMaximumDiff, worldDiff);
                auto leader = ObjectAccessor::FindPlayer(job.leader);
                auto group = leader ? leader->GetGroup() : nullptr;
                if (changed.count(job.current.guid))
                    job.current.invalidated = true;
                for (auto& task : job.ready)
                    if (changed.count(task.guid))
                        task.invalidated = true;
                if (lost.count(job.group) || changed.count(job.leader) || !leader || Epoch(job.leader) != job.epoch ||
                    !leader->GetSession() || !group || group->GetGUID() != job.group || !group->IsLeader(job.leader) ||
                    leader->GetMapId() != job.profile.map || leader->GetInstanceId() != job.instance ||
                    leader->GetMap()->GetSpawnMode() != job.profile.difficulty || leader->GetSession()->IsLoggingOut())
                    job.stopped = "leader/group/login/raid instance changed";
            }
            for (auto itr = _jobs.begin(); itr != _jobs.end();)
            {
                if (itr->stopped.empty() && (itr->cursor < itr->bots.size() || !itr->current.guid.IsEmpty() ||
                    !itr->ready.empty()))
                {
                    ++itr;
                    continue;
                }
                if (!itr->current.guid.IsEmpty() || !itr->ready.empty())
                {
                    // Interrupted character persistence is also paced, not a burst of saves during cancellation.
                    ++itr;
                    continue;
                }
                Report(*itr);
                ReleaseGroup(itr->group);
                itr = _jobs.erase(itr);
            }
            SetWorkPending(WorkKind::Preparation, !_jobs.empty());
            if (_jobs.empty() || RecruitmentNeedsPriority())
                return;
            auto sliceStart = std::chrono::steady_clock::now();
            uint32 allowance = _budget.Begin(PreparationNowUs());
            if (!allowance)
                return;
            _sliceDeadline = sliceStart + std::chrono::microseconds(allowance);
            _heavyPerformed = false;
            _learnedThisSlice = 0;
            uint32 steps = 0;
            while (steps++ < 32 && !_heavyPerformed && std::chrono::steady_clock::now() < _sliceDeadline)
            {
                auto& job = _jobs.front();
                auto pick = PickPreparationTask(job.ready, job.cursor < job.bots.size(), !job.stopped.empty(),
                    job.preferAdmission, [](BotTask const& task) { return task.stage <= Stage::Talents; });
                if (!pick.available)
                    break;
                if (!pick.admit)
                {
                    auto task = job.ready.begin() + pick.index;
                    job.current = std::move(*task);
                    job.ready.erase(task);
                }
                auto start = std::chrono::steady_clock::now();
                try
                {
                    if (!job.stopped.empty())
                    {
                        _heavyPerformed = true;
                        FinishBot(job, false, "preparation interrupted");
                    }
                    else
                        Step(job);
                }
                catch (std::exception const& error)
                {
                    LOG_ERROR("module.raidmanager", "Preparation failed: {}", error.what());
                    _heavyPerformed = true;
                    FinishBot(job, false, "exception; completed changes retained");
                }
                catch (...)
                {
                    _heavyPerformed = true;
                    FinishBot(job, false, "unknown error; completed changes retained");
                }
                uint64 work = PreparationElapsedUs(start, std::chrono::steady_clock::now());
                ++job.microsteps;
                job.workUs += work;
                job.maximumStepUs = std::max(job.maximumStepUs, work);
                if (!job.current.guid.IsEmpty())
                {
                    job.ready.push_back(std::move(job.current));
                    job.current = {};
                }
                if (_jobs.size() > 1)
                    std::rotate(_jobs.begin(), _jobs.begin() + 1, _jobs.end());
            }
            _budget.Charge(PreparationElapsedUs(sliceStart, std::chrono::steady_clock::now()));
        }
    private:
        std::string Check(PrepJob const& job, Player* bot)
        {
            if (!bot || !bot->IsInWorld() || !bot->GetSession() || bot->GetSession()->IsLoggingOut())
                return "offline or logging out";
            auto ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            if (!ai || ai->IsRealPlayer() || !sRandomPlayerbotMgr.IsRandomBot(bot))
                return "human/non-random member is read-only";
            auto leader = ObjectAccessor::FindPlayer(job.leader);
            if (!leader || ai->GetMaster() != leader)
                return "bot is not controlled by this leader";
            if (!bot->GetGroup() || bot->GetGroup()->GetGUID() != job.group)
                return "bot left the group";
            if (bot->GetMapId() != job.profile.map || bot->GetInstanceId() != job.instance)
                return "outside this raid instance";
            if (bot->IsInCombat() || !bot->IsAlive() || bot->IsBeingTeleported())
                return "combat, dead or teleporting";
            auto bind = sInstanceSaveMgr->PlayerGetBoundInstance(bot->GetGUID(), job.profile.map,
                Difficulty(job.profile.difficulty));
            if (bind && bind->save && bind->save->GetInstanceId() != job.instance)
                return "incompatible instance binding";
            return {};
        }
        std::array<uint32, 4> Counts(PrepJob const& job)
        {
            std::array<uint32, 4> counts{};
            auto leader = ObjectAccessor::FindPlayer(job.leader);
            if (!leader || !leader->GetGroup())
                return counts;
            for (auto const& slot : leader->GetGroup()->GetMemberSlots())
                if (auto member = ObjectAccessor::FindPlayer(slot.guid); member &&
                    member->GetMapId() == job.profile.map && member->GetInstanceId() == job.instance)
                    ++counts[static_cast<std::size_t>(ClassifyPrepRole(member))];
            return counts;
        }
        static std::pair<uint32, uint32> Goals(uint32 capacity)
        {
            return {capacity > 25 ? 4 : 2, capacity > 25 ? 10 : capacity <= 10 ? 3 : 5};
        }
        void Step(PrepJob& job)
        {
            if (job.current.guid.IsEmpty())
            {
                ObjectGuid guid = job.bots[job.cursor++];
                auto bot = ObjectAccessor::FindPlayer(guid);
                std::string issue = Check(job, bot);
                if (!issue.empty() || !TryReserveBot(guid))
                {
                    ++job.skipped;
                    Tell(job, "member skipped: " + (issue.empty() ? "reserved by another job" : issue));
                    return;
                }
                job.current = {};
                job.current.guid = guid;
                job.current.admittedAt = std::chrono::steady_clock::now();
                job.current.epoch = Epoch(guid);
                job.current.role = ClassifyPrepRole(bot);
                auto counts = Counts(job);
                auto [tanks, healers] = Goals(job.profile.capacity);
                // Preserve existing tanks/healers and the sole melee DPS.
                if (job.current.role == PrepRole::Ranged ||
                    (job.current.role == PrepRole::Melee && counts[3] > 1))
                {
                    if (counts[0] < tanks && FindRoleSpec(bot, PrepRole::Tank) >= 0)
                        job.current.role = PrepRole::Tank;
                    else if (counts[1] < healers && FindRoleSpec(bot, PrepRole::Healer) >= 0)
                        job.current.role = PrepRole::Healer;
                }
                return;
            }
            auto bot = ObjectAccessor::FindPlayer(job.current.guid);
            std::string invalid = Check(job, bot);
            if (job.current.invalidated || !invalid.empty() || Epoch(job.current.guid) != job.current.epoch)
            {
                _heavyPerformed = true;
                FinishBot(job, false, invalid.empty() ? "bot login changed" : invalid);
                return;
            }
            auto leader = ObjectAccessor::FindPlayer(job.leader);
            if (!leader || leader->IsInCombat())
            {
                job.stopped = "raid leader unavailable or in combat";
                return;
            }
            auto& task = job.current;
            auto ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            if (++task.steps % 25 == 0)
                Tell(job, "preparing " + bot->GetName() + "; completed members " +
                    std::to_string(job.complete + job.partial + job.failed + job.skipped) + "/" +
                    std::to_string(job.bots.size()));
            PreparationStageTurn stageTurn(task);
            switch (task.stage)
            {
                case Stage::Basics:
                {
                    _heavyPerformed = true;
                    auto limits = AssessPromotion(bot, bot, true);
                    if (limits.block != PromotionBlock::Allowed)
                    {
                        FinishBot(job, false, DescribePromotionBlock(limits.block));
                        return;
                    }
                    task.dirty = true;
                    if (bot->getClass() != CLASS_DEATH_KNIGHT)
                        PlayerbotFactory(bot, bot->GetLevel()).InitClassSpells();
                    else
                        Issue(task, "broad DK class helper skipped (contains mount/starting-area spells)");
                    task.stage = Stage::Talents;
                    break;
                }
                case Stage::Talents:
                {
                    _heavyPerformed = true;
                    std::string reason;
                    task.talents = ApplyRoleTalents(bot, task.role, reason);
                    task.results[Talents] = task.talents ? Success : Failed;
                    if (!task.talents)
                    {
                        Issue(task, reason, Talents);
                        task.results[Talents] = Failed;
                    }
                    task.stage = Stage::Catalog;
                    break;
                }
                case Stage::Catalog:
                    if (DiscoverTrainers())
                        task.stage = Stage::Spells;
                    break;
                case Stage::Spells:
                    LearnStep(task, bot, _sliceDeadline);
                    break;
                case Stage::Skills:
                {
                    static std::array<uint32, 17> const skills = {SKILL_DEFENSE, SKILL_UNARMED, SKILL_SWORDS,
                        SKILL_AXES, SKILL_MACES, SKILL_2H_SWORDS, SKILL_2H_AXES, SKILL_2H_MACES, SKILL_DAGGERS,
                        SKILL_STAVES, SKILL_POLEARMS, SKILL_FIST_WEAPONS, SKILL_BOWS, SKILL_GUNS,
                        SKILL_CROSSBOWS, SKILL_THROWN, SKILL_WANDS};
                    uint32 checked = 0;
                    while (task.skill < skills.size() && checked++ < 8 &&
                        std::chrono::steady_clock::now() < _sliceDeadline)
                    {
                        uint32 skill = skills[task.skill++];
                        if (bot->HasSkill(skill) && !PrepareCombatSkill(bot, skill))
                            Issue(task, "combat skill verification failed", Skills);
                    }
                    if (task.skill >= skills.size())
                    {
                        if (!bot->HasSkill(SKILL_DEFENSE))
                            Issue(task, "required defense skill missing", Skills);
                        if (task.results[Skills] == Unchecked)
                            task.results[Skills] = Success;
                        task.stage = Stage::GearPlan;
                    }
                    break;
                }
                case Stage::GearPlan:
                {
                    auto limits = AssessPromotion(bot, bot, true);
                    if (limits.block != PromotionBlock::Allowed)
                    {
                        FinishBot(job, false, DescribePromotionBlock(limits.block));
                        return;
                    }
                    if (!task.talents)
                        Issue(task, "gear skipped: role talents not verified", Bis);
                    else if (!job.profile.bisKey)
                        Issue(task, "BiS key 0: gear preparation disabled or profile unknown", Bis);
                    else
                    {
                        // Raid-specific exact tier lookup is independent of random-bot AutoGearScoreLimit.
                        uint8 tab = task.role == PrepRole::Tank && bot->getClass() == CLASS_DRUID ?
                            10 : AiFactory::GetPlayerSpecTab(bot);
                        task.gearEnabled = true;
                        task.bisEquipment = sBisListMgr->GetBisFor(job.profile.bisKey, bot->getClass(), tab,
                            bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2);
                        task.results[Bis] = task.bisEquipment.empty() ? Partial : Success;
                        LOG_INFO("module.raidmanager", "Prep bot {} level {} class {} tab {} faction {} "
                            "BiS key {}: {} cached slots", bot->GetGUID().ToString(), bot->GetLevel(),
                            bot->getClass(), tab, static_cast<uint32>(bot->GetTeamId()),
                            job.profile.bisKey, task.bisEquipment.size());
                        if (task.bisEquipment.empty())
                        {
                            Issue(task, "no exact BiS class/spec/faction data; bounded fallback search enabled", Bis);
                        }
                    }
                    task.stage = Stage::GearSlot;
                    break;
                }
                case Stage::GearSlot:
                    if (!task.gearEnabled)
                    {
                        task.results[Gear] = Unsupported;
                        task.stage = Stage::Strategies;
                    }
                    else if (task.slot < EQUIPMENT_SLOT_END)
                    {
                        uint8 slot = task.slot;
                        bool optional = slot == EQUIPMENT_SLOT_OFFHAND && IsOptionalOffhand(bot, task.role);
                        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD || optional)
                        {
                            ++task.slot;
                            break;
                        }
                        auto bis = task.bisEquipment.find(slot);
                        uint32 exact = bis == task.bisEquipment.end() ? 0 : bis->second;
                        if (!task.choiceReady && !SelectEquipmentStep(bot, task.role, job.profile, slot, exact,
                            task.selection, job.indexValidation, _sliceDeadline))
                            break;
                        task.choiceReady = true;
                        auto const& choice = task.selection.choice;
                        if (choice.route == CandidateRoute::Retained)
                        {
                            task.equipment[slot] = choice.item;
                            ++task.gearApplied;
                            ++task.retained;
                        }
                        else if (choice.item)
                        {
                            _heavyPerformed = true;
                            ++task.equipAttempts;
                            LogEquipmentSourceDiagnostic(bot, slot, choice.item, job.profile);
                            std::string reason;
                            bool valid = RevalidateEquipmentChoice(bot, task.role, job.profile, slot, choice);
                            bool applied = valid && ApplyPreparedItem(bot, slot, choice.item, reason);
                            if (!valid)
                                reason = "candidate requirements or improvement changed; existing equipment preserved";
                            if (applied)
                            {
                                ++task.gearApplied;
                                task.equipment[slot] = choice.item;
                            }
                            else
                            {
                                if (reason.find("failed") != std::string::npos)
                                    ++task.gearFailed;
                                else
                                    ++task.gearSkipped;
                                Issue(task, "gear slot " + std::to_string(slot) + ": " + reason, Gear);
                            }
                            LOG_INFO("module.raidmanager", "Prep bot {} slot {} candidate {}: {} {}",
                                task.guid.ToString(), slot, choice.item,
                                applied ? "equipped/retained" : "not equipped", reason);
                        }
                        else
                        {
                            ++task.gearSkipped;
                            Issue(task, "no suitable slot improvement; existing items preserved", Gear);
                        }
                        auto ceiling = GetCandidateLimits(job.profile.map, job.profile.difficulty,
                            job.profile.capacity);
                        LOG_INFO("module.raidmanager", "Candidate bot {} slot {} item {} route '{}' "
                            "sourceVerified {} ilvlCeiling {} ceilingAnchor {} score {} reason '{}' search='{}'",
                            task.guid.ToString(), slot, choice.item, DescribeCandidateRoute(choice.route),
                            choice.verifiedSource, ceiling.ilvl, ceiling.anchorItem, choice.score, choice.reason,
                            EquipmentSearchSummary(task.selection));
                        task.selection = {};
                        task.choiceReady = false;
                        ++task.slot;
                    }
                    else
                    {
                        if (task.results[Gear] == Unchecked)
                            task.results[Gear] = task.gearApplied ? Success : Unsupported;
                        task.stage = Stage::Strategies;
                    }
                    break;
                case Stage::Strategies:
                    if (task.talents)
                        ApplyStrategies(job, bot, ai);
                    else
                        Issue(task, "role AI skipped because talents were not verified", RoleAI);
                    task.stage = Stage::Verify;
                    break;
                case Stage::Verify:
                {
                    for (auto const& [slot, item] : task.equipment)
                    {
                        auto equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                        if (!equipped || equipped->GetEntry() != item ||
                            bot->CanUseItem(equipped) != EQUIP_ERR_OK)
                            Issue(task, "final equipment differs from the selected plan", Gear);
                    }
                    uint32 spell = RequiredRoleSpell(bot, task.role);
                    if (!spell || !bot->HasSpell(spell))
                    {
                        Issue(task, "required role spell missing or no readiness rule", Spells);
                        task.results[Spells] = Failed;
                    }
                    else if (task.results[Spells] == Unchecked)
                        task.results[Spells] = Success;
                    if (task.role == PrepRole::Tank || task.role == PrepRole::Melee)
                        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND) &&
                            bot->getClass() != CLASS_DRUID)
                            Issue(task, "required main-hand weapon missing", Gear);
                    if (!task.ai)
                        Issue(task, "role AI not verified", RoleAI);
                    task.stage = Stage::Save;
                    break;
                }
                case Stage::Save:
                    _heavyPerformed = true;
                    bot->SaveToDB(false, false);
                    task.dirty = false;
                    FinishBot(job, true, {});
                    break;
            }
        }
        void LearnStep(BotTask& task, Player* bot, std::chrono::steady_clock::time_point deadline)
        {
            // Bounded class-only learning, not broad maintenance or InitAvailableSpells/InitSkills.
            uint32 budget = 32;
            while (budget-- && _learnedThisSlice < 8 && task.trainer < ClassTrainers.size() &&
                std::chrono::steady_clock::now() < deadline)
            {
                auto trainer = sObjectMgr->GetTrainer(ClassTrainers[task.trainer]);
                if (!trainer || !trainer->IsTrainerValidForPlayer(bot) || task.spell >= trainer->GetSpells().size())
                {
                    ++task.trainer;
                    task.spell = 0;
                    continue;
                }
                auto const& spell = trainer->GetSpells()[task.spell++];
                auto info = sSpellMgr->GetSpellInfo(spell.SpellId);
                if (!info)
                {
                    Issue(task, "class trainer references unknown spell");
                    continue;
                }
                // CanTeachSpell uses AssertSpellInfo internally; reject invalid rows before calling it.
                if (!trainer->CanTeachSpell(bot, &spell))
                    continue;
                std::vector<uint32> learn;
                if (spell.IsCastable())
                {
                    bool simple = true;
                    for (auto const& effect : info->Effects)
                        if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                            learn.push_back(effect.TriggerSpell);
                        else if (effect.IsEffect())
                            simple = false;
                    if (!simple || learn.empty())
                    {
                        Issue(task, "complex trainer effect skipped; no arbitrary casting");
                        continue;
                    }
                }
                else
                    learn.push_back(spell.SpellId);
                for (uint32 id : learn)
                {
                    bool known = bot->HasSpell(id);
                    std::string reason;
                    if (!LearnClassSpell(bot, id, reason))
                        Issue(task, reason);
                    if (!known && bot->HasSpell(id))
                        ++_learnedThisSlice;
                }
            }
            if (task.trainer >= ClassTrainers.size())
                task.stage = Stage::Skills;
        }
        void ApplyStrategies(PrepJob& job, Player* bot, PlayerbotAI* ai)
        {
            // Inspection only: never remove or overwrite Commander/custom strategy selections.
            auto& task = job.current;
            auto combat = ai->GetStrategies(BOT_STATE_COMBAT);
            auto nonCombat = ai->GetStrategies(BOT_STATE_NON_COMBAT);
            auto roleType = task.role == PrepRole::Tank ? STRATEGY_TYPE_TANK :
                task.role == PrepRole::Healer ? STRATEGY_TYPE_HEAL : STRATEGY_TYPE_DPS;
            bool combatRole = false;
            for (auto const& name : combat)
                if (auto strategy = ai->GetStrategy(name, BOT_STATE_COMBAT);
                    strategy && (strategy->GetType() & roleType))
                    combatRole = true;
            task.ai = ClassifyPrepRole(bot) == task.role && combatRole;
            if (bot->getClass() == CLASS_DRUID && AiFactory::GetPlayerSpecTab(bot) == DRUID_TAB_FERAL)
                task.ai = task.ai && ai->HasStrategy(task.role == PrepRole::Tank ? "bear" : "cat", BOT_STATE_COMBAT);
            task.results[RoleAI] = task.ai ? Success : Failed;
            if (!task.ai)
            {
                Issue(task, "role strategy/spec mismatch; existing strategies preserved", RoleAI);
                task.results[RoleAI] = Failed;
            }
            std::string instance = InstanceStrategy(job.profile.map);
            if (instance.empty())
                task.results[RaidAI] = Unsupported;
            else if (ai->HasStrategy(instance, BOT_STATE_COMBAT) || ai->HasStrategy(instance, BOT_STATE_NON_COMBAT))
                task.results[RaidAI] = Success;
            else
                Issue(task, "supported instance strategy is not active; no temporary injection performed", RaidAI);
            LOG_INFO("module.raidmanager", "Prep bot {} role {} roleVerified {} instance strategy '{}' result {}",
                task.guid.ToString(), RoleName(task.role), task.ai, instance,
                static_cast<uint32>(task.results[RaidAI]));
            std::string activeCombat;
            std::string activeNonCombat;
            for (auto const& name : combat)
                activeCombat += name + ";";
            for (auto const& name : nonCombat)
                activeNonCombat += name + ";";
            LOG_INFO("module.raidmanager", "Prep bot {} active combat [{}] noncombat [{}]",
                task.guid.ToString(), activeCombat, activeNonCombat);
        }
        void FinishBot(PrepJob& job, bool processed, std::string const& failure)
        {
            auto& task = job.current;
            if (task.guid.IsEmpty())
                return;
            _heavyPerformed = true;
            if (!failure.empty())
                Issue(task, failure);
            auto bot = ObjectAccessor::FindConnectedPlayer(task.guid);
            if (task.dirty && bot && Epoch(task.guid) == task.epoch)
            {
                try
                {
                    bot->SaveToDB(false, false);
                }
                catch (...)
                {
                    Issue(task, "save request failed");
                }
            }
            if (!processed)
                ++job.failed;
            else if (!task.issues.empty())
            {
                ++job.partial;
                if (!task.nonAiIssue)
                    ++job.aiOnlyPartial;
            }
            else
                ++job.complete;
            Tell(job, "bot " + task.guid.ToString() + " " + RoleName(task.role) + ": " +
                (processed ? task.issues.empty() ? "complete" : "partial" : "failed"));
            for (std::size_t category = 0; category < DiagnosticCount; ++category)
            {
                ++job.results[category][task.results[category]];
                LOG_INFO("module.raidmanager", "Prep bot {} {} result {} (0 unchecked, 1 OK, 2 partial, "
                    "3 failed, 4 unsupported)", task.guid.ToString(), DiagnosticNames[category],
                    static_cast<uint32>(task.results[category]));
            }
            job.gearApplied += task.gearApplied;
            job.gearSkipped += task.gearSkipped;
            job.gearFailed += task.gearFailed;
            for (auto const& reason : task.issues)
                LOG_WARN("module.raidmanager", "Prep bot {}: {}", task.guid.ToString(), reason);
            EndPreparationTurn(task);
            LOG_INFO("module.raidmanager", "Prep timing bot {} {}", task.guid.ToString(),
                PreparationTimingSummary(task));
            LOG_INFO("module.raidmanager", "Prep equipment bot {} applyAttempts {} retained {} "
                "admissionWall_ms {} requestWall_ms {}",
                task.guid.ToString(), task.equipAttempts, task.retained,
                PreparationElapsedUs(task.admittedAt, std::chrono::steady_clock::now()) / 1000,
                PreparationElapsedUs(job.requestedAt, std::chrono::steady_clock::now()) / 1000);
            ReleaseBot(task.guid);
            job.current = {};
        }
        static void Tell(PrepJob const& job, std::string const& message)
        {
            if (auto leader = ObjectAccessor::FindPlayer(job.leader); leader && leader->GetSession())
                ChatHandler(leader->GetSession()).PSendSysMessage("Raid Manager prep: {}", message);
        }
        void Report(PrepJob const& job)
        {
            LOG_INFO("module.raidmanager", "Prep job timing group {} bots {} complete {} partial {} failed {} "
                "skipped {} wall_ms {} microsteps {} work_us {} maxStep_us {} budgetSlices {} throttled {} "
                "budgetMaxSlice_us {} mismatchItem {} worldP95Bound_ms {} worldP99Bound_ms {} worldMaxDiff_ms {}",
                job.group.ToString(), job.bots.size(), job.complete, job.partial, job.failed, job.skipped,
                PreparationElapsedUs(job.requestedAt, std::chrono::steady_clock::now()) / 1000,
                job.microsteps, job.workUs, job.maximumStepUs, _budget.slices, _budget.throttled,
                _budget.maximumSliceUs, job.indexValidation.mismatchItem, WorldDiffPercentile(job, 95),
                WorldDiffPercentile(job, 99), job.worldMaximumDiff);
            auto leader = ObjectAccessor::FindPlayer(job.leader);
            if (!leader || !leader->GetSession())
                return;
            for (std::size_t category = 0; category < DiagnosticCount; ++category)
            {
                auto const& counts = job.results[category];
                ChatHandler(leader->GetSession()).PSendSysMessage("Raid prep {}: OK {}, partial {}, failed {}, "
                    "unsupported {}, unchecked {}.", DiagnosticNames[category], counts[Success], counts[Partial],
                    counts[Failed], counts[Unsupported], counts[Unchecked]);
            }
            ChatHandler(leader->GetSession()).PSendSysMessage("Raid prep slots: applied/retained {}, skipped {}, "
                "failed {}. AI-only partial bots {}. Detailed reasons: module.raidmanager log.",
                job.gearApplied, job.gearSkipped, job.gearFailed, job.aiOnlyPartial);
            auto counts = Counts(job);
            auto [tanks, healers] = Goals(job.profile.capacity);
            ChatHandler(leader->GetSession()).PSendSysMessage("Raid Manager prep: {}. Baseline policy 1; "
                "complete {}, partial {}, failed {}, skipped {}, unprocessed {}. "
                "In-instance tanks {}/{}, healers {}/{}, ranged {}, melee {}. "
                "Save requests are asynchronous; maintenance completion is not verified.",
                job.stopped.empty() ? "finished" : job.stopped, job.complete, job.partial, job.failed, job.skipped,
                job.bots.size() - job.cursor, counts[0], tanks, counts[1], healers, counts[2], counts[3]);
        }
        std::deque<PrepJob> _jobs;
        PreparationBudget _budget;
        bool _heavyPerformed = false;
        uint32 _learnedThisSlice = 0;
        std::chrono::steady_clock::time_point _sliceDeadline{};
        std::set<std::pair<ObjectGuid, uint32>> _baseline;
        std::mutex _mutex;
        std::map<ObjectGuid, uint64> _epochs;
        std::set<ObjectGuid> _lostGroups;
        std::set<ObjectGuid> _changedMembers;
    };
    class PreparationControl final : public PlayerbotOperation
    {
    public:
        PreparationControl(ObjectGuid player, uint64 epoch, bool cancel, uint32 difficulty)
            : _player(player), _epoch(epoch), _cancel(cancel), _difficulty(difficulty)
        {
        }
        bool Execute() override
        {
            PreparationManager::Instance().Request(_player, _epoch, _cancel, _difficulty);
            return true;
        }
        std::string GetName() const override
        {
            return "RaidPreparationControl";
        }
    private:
        ObjectGuid _player;
        uint64 _epoch;
        bool _cancel;
        uint32 _difficulty;
    };
    class PreparationWorld final : public WorldScript
    {
    public:
        PreparationWorld() : WorldScript("raid_manager_preparation_world",
            {WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_AFTER_CONFIG_LOAD})
        {
        }
        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadPreparationProfiles();
        }
        void OnUpdate(uint32 diff) override
        {
            PreparationManager::Instance().Update(diff);
        }
    };
    class PreparationPlayers final : public PlayerScript
    {
    public:
        PreparationPlayers() : PlayerScript("raid_manager_preparation_players",
            {PLAYERHOOK_ON_BEFORE_LOGOUT, PLAYERHOOK_ON_MAP_CHANGED})
        {
        }
        void OnPlayerBeforeLogout(Player* player) override
        {
            PreparationManager::Instance().Logout(player);
        }
        void OnPlayerMapChanged(Player* player) override
        {
            PreparationManager::Instance().MemberChanged(player->GetGUID());
        }
    };
    class PreparationGroups final : public GroupScript
    {
    public:
        PreparationGroups() : GroupScript("raid_manager_preparation_groups",
            {GROUPHOOK_ON_DISBAND, GROUPHOOK_ON_CHANGE_LEADER, GROUPHOOK_ON_REMOVE_MEMBER})
        {
        }
        void OnDisband(Group* group) override
        {
            PreparationManager::Instance().GroupLost(group->GetGUID());
        }
        void OnChangeLeader(Group* group, ObjectGuid /*newLeader*/, ObjectGuid /*oldLeader*/) override
        {
            PreparationManager::Instance().GroupLost(group->GetGUID());
        }
        void OnRemoveMember(Group* /*group*/, ObjectGuid member, RemoveMethod /*method*/, ObjectGuid /*kicker*/,
            char const* /*reason*/) override
        {
            PreparationManager::Instance().MemberChanged(member);
        }
    };
    void QueuePrepControl(ChatHandler* handler, bool cancel, uint32 difficulty)
    {
        if (auto player = handler->GetPlayer())
            if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<PreparationControl>(
                player->GetGUID(), PreparationManager::Instance().Epoch(player->GetGUID()), cancel, difficulty)))
                handler->SendSysMessage("Raid Manager: preparation request queue full; no work scheduled.");
    }
}
void QueuePreparation(ChatHandler* handler, bool cancel)
{
    QueuePrepControl(handler, cancel, 0);
}
void QueueDifficultyRequest(ChatHandler* handler, uint32 level)
{
    QueuePrepControl(handler, false, level);
}
void RegisterPreparationScripts()
{
    new PreparationWorld();
    new PreparationPlayers();
    new PreparationGroups();
}
}
