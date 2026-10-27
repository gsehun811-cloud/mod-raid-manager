#include "RaidEquipmentSelection.h"
#include "RaidPreparationSets.h"
#include "AiFactory.h"
#include "BisListMgr.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RandomItemMgr.h"
#include "StatsWeightCalculator.h"
#include "StringFormat.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace RaidManager
{
namespace
{
    using SearchClock = std::chrono::steady_clock;

    uint64 ElapsedUs(SearchClock::time_point start, SearchClock::time_point end)
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }

    class SearchTurnTrace
    {
    public:
        explicit SearchTurnTrace(EquipmentSelection& selection) :
            _metrics(selection.metrics), _segment(SearchClock::now())
        {
            if (!_metrics.started)
            {
                _metrics.started = true;
                _metrics.startedAt = _segment;
                _metrics.phaseEntered = _segment;
                _metrics.activePhase = selection.phase;
            }
            ChangePhase(selection.phase);
            ++_metrics.turns;
            ++_metrics.phases[_metrics.activePhase].visits;
        }

        void ChangePhase(uint8 phase)
        {
            if (phase == _metrics.activePhase)
                return;
            auto now = SearchClock::now();
            auto& old = _metrics.phases[_metrics.activePhase];
            old.workUs += ElapsedUs(_segment, now);
            old.wallUs += ElapsedUs(_metrics.phaseEntered, now);
            _metrics.activePhase = phase;
            _metrics.phaseEntered = now;
            _segment = now;
        }

        ~SearchTurnTrace()
        {
            auto now = SearchClock::now();
            _metrics.phases[_metrics.activePhase].workUs += ElapsedUs(_segment, now);
            _metrics.finishedAt = now;
        }

    private:
        EquipmentSearchMetrics& _metrics;
        SearchClock::time_point _segment;
    };

    bool RuntimeUnsupported(ItemTemplate const* proto)
    {
        constexpr uint32 playableClasses = 1535;
        constexpr uint32 playableRaces = 1791;
        return !proto || proto->RandomProperty || proto->RandomSuffix || proto->Area || proto->Map ||
            proto->RequiredCityRank || proto->RequiredHonorRank || RandomItemMgr::IsInternalItem(proto) ||
            !(proto->AllowableClass & playableClasses) || !(proto->AllowableRace & playableRaces);
    }

    void ValidateIndexSlice(CandidateIndexValidation& validation, SearchClock::time_point deadline)
    {
        if (validation.complete)
            return;
        uint32 checked = 0;
        while (validation.cursor < GetCandidateCatalogSize() && checked++ < 256 && SearchClock::now() < deadline)
        {
            auto entry = GetCatalogCandidate(validation.cursor);
            auto proto = sObjectMgr->GetItemTemplate(entry->item);
            uint16 classMask = proto ? static_cast<uint16>(proto->AllowableClass & 1535) : 0;
            if (!proto || !MatchesCandidateIndexSnapshot(validation.cursor, classMask, RuntimeUnsupported(proto)))
            {
                validation.mismatchItem = entry->item;
                LOG_WARN("module.raidmanager", "Candidate index snapshot mismatch: item {} ordinal {} present {} "
                    "classMask expected {} actual {} unsupported expected {} actual {} "
                    "randomProperty {} randomSuffix {} area {} map {} cityRank {} honorRank {} internal {}. "
                    "Legacy exhaustive search retained; no protection bypassed.",
                    entry->item, validation.cursor, proto != nullptr, GetSnapshotClassMask(validation.cursor),
                    classMask, GetSnapshotUnsupported(validation.cursor), RuntimeUnsupported(proto),
                    proto ? proto->RandomProperty : 0, proto ? proto->RandomSuffix : 0,
                    proto ? proto->Area : 0, proto ? proto->Map : 0, proto ? proto->RequiredCityRank : 0,
                    proto ? proto->RequiredHonorRank : 0, proto && RandomItemMgr::IsInternalItem(proto));
                validation.compatible = false;
                validation.complete = true;
                return;
            }
            ++validation.cursor;
        }
        validation.complete = validation.cursor == GetCandidateCatalogSize();
    }

    bool ClassIndexMatchesPlayer(Player* bot, uint8 slot)
    {
        uint8 cls = bot->getClass();
        if (cls < 1 || cls > 11 || !(1535u & (1u << (cls - 1))) || !(1791u & bot->getRaceMask()))
            return false;
        for (std::size_t index = 0; auto skill = GetCandidateProficiencySkill(index); ++index)
            if (!IndexAllowsProficiency(index, cls) && bot->HasSkill(skill))
                return false;
        // Heirloom armor can bypass a missing proficiency through class-context hooks.
        for (auto armorClass : {CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_SHAMAN})
            if (bot->IsClass(armorClass, CLASS_CONTEXT_EQUIP_ARMOR_CLASS) && cls != armorClass)
                return false;
        // Custom class-context hooks can grant another class's relic access.
        if (slot == EQUIPMENT_SLOT_RANGED)
            for (auto relicClass : {CLASS_PALADIN, CLASS_DRUID, CLASS_SHAMAN, CLASS_DEATH_KNIGHT})
                if (bot->IsClass(relicClass, CLASS_CONTEXT_EQUIP_RELIC) && cls != relicClass)
                    return false;
        return true;
    }

    bool CeilingMatchesRuntime(CandidateLimits const& limits)
    {
        auto anchor = sObjectMgr->GetItemTemplate(limits.anchorItem);
        return anchor && anchor->ItemLevel == limits.ilvl;
    }

    bool EvaluatorMatchesRole(Player* bot, PrepRole role)
    {
        if (ClassifyPrepRole(bot) != role)
            return false;
        // StatsWeightCalculator checks heal, caster, tank, melee in that order.
        if (role == PrepRole::Tank)
            return PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsCaster(bot);
        if (role == PrepRole::Healer)
            return PlayerbotAI::IsHeal(bot);
        if (PlayerbotAI::IsTank(bot) || PlayerbotAI::IsHeal(bot))
            return false;
        if (role == PrepRole::Melee)
            return !PlayerbotAI::IsCaster(bot) && PlayerbotAI::IsMelee(bot);
        return PlayerbotAI::IsCaster(bot) || !PlayerbotAI::IsMelee(bot);
    }

    CollectorType CollectorFor(Player* bot, PrepRole role)
    {
        if (role == PrepRole::Tank)
            return CollectorType::MELEE_TANK;
        if (role == PrepRole::Healer)
            return CollectorType::SPELL_HEAL;
        if (PlayerbotAI::IsCaster(bot))
            return CollectorType::SPELL_DMG;
        return role == PrepRole::Melee ? CollectorType::MELEE_DMG : CollectorType::RANGED;
    }

    float Score(StatsWeightCalculator& calculator, ItemTemplate const* proto, uint8 slot, Item const* owned = nullptr)
    {
        float score = calculator.CalculateItem(proto->ItemId, 0, slot);
        uint32 sockets = 0;
        for (auto const& socket : proto->Socket)
            if (socket.Color)
                ++sockets;
        // Empty sockets and unearned socket bonuses must not manufacture an upgrade.
        score /= 1.0f + 0.03f * sockets;
        if (proto->socketBonus)
            score -= calculator.CalculateEnchant(proto->socketBonus);
        if (owned)
            for (uint8 index = 0; index < MAX_INSPECTED_ENCHANTMENT_SLOT; ++index)
                if (auto enchant = owned->GetEnchantmentId(EnchantmentSlot(index)))
                    score += calculator.CalculateEnchant(enchant);
        return std::isfinite(score) ? std::max(0.0f, score) : score;
    }

    void CollectActualStats(StatsCollector& collector, ItemTemplate const* proto, Item const* owned = nullptr)
    {
        collector.CollectItemStats(proto);
        if (proto->socketBonus)
            if (auto enchant = sSpellItemEnchantmentStore.LookupEntry(proto->socketBonus))
            {
                StatsCollector bonus = collector;
                bonus.Reset();
                bonus.CollectEnchantStats(enchant);
                for (uint8 index = 0; index < STATS_TYPE_MAX; ++index)
                    collector.stats[index] -= bonus.stats[index];
            }
        if (owned)
            for (uint8 index = 0; index < MAX_INSPECTED_ENCHANTMENT_SLOT; ++index)
            {
                auto id = owned->GetEnchantmentId(EnchantmentSlot(index));
                if (auto enchant = sSpellItemEnchantmentStore.LookupEntry(id))
                    collector.CollectEnchantStats(enchant);
            }
    }

    bool RoleFloors(Player* bot, PrepRole role, ItemTemplate const* proto, Item const* old, uint8 slot)
    {
        StatsCollector candidate(CollectorFor(bot, role), bot->getClass());
        CollectActualStats(candidate, proto);
        auto const& stats = candidate.stats;
        bool caster = PlayerbotAI::IsCaster(bot);
        float relevant = role == PrepRole::Healer ? stats[STATS_TYPE_HEAL_POWER] +
            stats[STATS_TYPE_MANA_REGENERATION] + stats[STATS_TYPE_INTELLECT] : caster ?
            stats[STATS_TYPE_SPELL_POWER] + stats[STATS_TYPE_INTELLECT] + stats[STATS_TYPE_HIT] :
            stats[STATS_TYPE_STRENGTH] + stats[STATS_TYPE_AGILITY] + stats[STATS_TYPE_ATTACK_POWER] +
            stats[STATS_TYPE_STAMINA] + stats[STATS_TYPE_MELEE_DPS] + stats[STATS_TYPE_RANGED_DPS];
        if (relevant <= 0.0f)
            return false;
        if (!old)
            return true;
        StatsCollector current(CollectorFor(bot, role), bot->getClass());
        CollectActualStats(current, old->GetTemplate(), old);
        auto protect = [&](StatsCollector const& collected)
        {
            auto const& s = collected.stats;
            ProtectedStats result{s[STATS_TYPE_STAMINA], s[STATS_TYPE_ARMOR], s[STATS_TYPE_DEFENSE],
                s[STATS_TYPE_DODGE], s[STATS_TYPE_PARRY], s[STATS_TYPE_BLOCK_VALUE], s[STATS_TYPE_BLOCK_RATING],
                s[STATS_TYPE_HEAL_POWER], s[STATS_TYPE_MANA_REGENERATION], s[STATS_TYPE_INTELLECT],
                s[STATS_TYPE_SPIRIT], s[STATS_TYPE_SPELL_POWER], s[STATS_TYPE_HIT], s[STATS_TYPE_EXPERTISE]};
            if (!caster && role != PrepRole::Healer && bot->getClass() != CLASS_DRUID)
            {
                if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND)
                    result.weaponDps = s[STATS_TYPE_MELEE_DPS];
                if (slot == EQUIPMENT_SLOT_RANGED && bot->getClass() == CLASS_HUNTER)
                    result.weaponDps = s[STATS_TYPE_RANGED_DPS];
            }
            return result;
        };
        auto protection = role == PrepRole::Tank ? ProtectionRole::Tank : role == PrepRole::Healer ?
            ProtectionRole::Healer : caster ? ProtectionRole::Caster : ProtectionRole::Physical;
        return PreservesRoleFloors(protection, protect(current), protect(candidate));
    }

    bool WeaponFitsRole(Player* bot, PrepRole role, ItemTemplate const* proto, uint8 slot)
    {
        if (role == PrepRole::Tank && proto->Class == ITEM_CLASS_ARMOR && slot != EQUIPMENT_SLOT_BACK &&
            proto->SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH && proto->SubClass <= ITEM_SUBCLASS_ARMOR_PLATE)
        {
            uint32 required = bot->getClass() == CLASS_DRUID ? ITEM_SUBCLASS_ARMOR_LEATHER :
                bot->GetLevel() >= 40 ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;
            if (proto->SubClass != required)
                return false;
        }
        if (slot == EQUIPMENT_SLOT_OFFHAND && role == PrepRole::Tank &&
            (bot->getClass() == CLASS_WARRIOR || bot->getClass() == CLASS_PALADIN))
            return proto->InventoryType == INVTYPE_SHIELD;
        if (proto->Class != ITEM_CLASS_WEAPON)
            return true;
        if (proto->InventoryType == INVTYPE_2HWEAPON && !bot->CanTitanGrip() &&
            bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            return false; // Defer coupled weapon replacement; never displace an unscored offhand.
        auto tab = AiFactory::GetPlayerSpecTab(bot);
        if (bot->getClass() == CLASS_DEATH_KNIGHT && slot == EQUIPMENT_SLOT_MAINHAND &&
            (role == PrepRole::Tank || tab == DEATH_KNIGHT_TAB_BLOOD || tab == DEATH_KNIGHT_TAB_UNHOLY))
            return proto->InventoryType == INVTYPE_2HWEAPON;
        if (((bot->getClass() == CLASS_WARRIOR && tab == WARRIOR_TAB_FURY && !bot->CanTitanGrip()) ||
            (bot->getClass() == CLASS_SHAMAN && tab == SHAMAN_TAB_ENHANCEMENT)) && bot->CanDualWield())
            return proto->InventoryType != INVTYPE_2HWEAPON;
        if (role == PrepRole::Tank && bot->getClass() != CLASS_DEATH_KNIGHT && bot->getClass() != CLASS_DRUID)
            return proto->InventoryType != INVTYPE_2HWEAPON;
        if (slot == EQUIPMENT_SLOT_MAINHAND &&
            ((bot->getClass() == CLASS_WARRIOR && tab == WARRIOR_TAB_ARMS) ||
             (bot->getClass() == CLASS_PALADIN && role == PrepRole::Melee) ||
             (bot->getClass() == CLASS_DRUID && (role == PrepRole::Tank || role == PrepRole::Melee))))
            return proto->InventoryType == INVTYPE_2HWEAPON;
        if (bot->getClass() == CLASS_ROGUE && tab == ROGUE_TAB_ASSASSINATION)
            return proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
        return true;
    }

    bool Evaluate(Player* bot, PrepRole role, RaidProfile const& profile, uint8 slot,
        CandidateCatalogEntry const& entry, EquipmentChoice& result, bool& usableNotUpgrade,
        SearchPhaseMetrics* metrics = nullptr)
    {
        auto limits = GetCandidateLimits(profile.map, profile.difficulty, profile.capacity);
        if (!CeilingMatchesRuntime(limits))
            return false;
        if (CheckCandidateLimits(entry, limits, bot->GetLevel()) != CandidateRejection::None)
            return false;
        auto proto = sObjectMgr->GetItemTemplate(entry.item);
        if (!proto || proto->ItemLevel != entry.ilvl || proto->RequiredLevel != entry.requiredLevel ||
            proto->InventoryType != entry.inventoryType || proto->Class != entry.itemClass ||
            proto->SubClass != entry.subclass || proto->Quality != entry.quality ||
            proto->RandomProperty || proto->RandomSuffix || !RandomItemMgr::IsValidItem(proto) ||
            static_cast<int32>(proto->Quality) > sPlayerbotAIConfig.autoGearQualityLimit ||
            !WeaponFitsRole(bot, role, proto, slot))
            return false;
        auto policy = GetEquipmentSourcePolicy(profile);
        auto diagnostic = DiagnoseEquipmentCandidate(bot, slot, entry.item, policy);
        if (diagnostic.missingTemplate || diagnostic.wrongSlot || diagnostic.wrongFaction ||
            diagnostic.source.verdict == SourceVerdict::LaterStage ||
            diagnostic.source.verdict == SourceVerdict::DifficultyBlocked ||
            bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return false;
        uint16 destination = 0;
        if (bot->CanEquipNewItem(slot, destination, entry.item, true) != EQUIP_ERR_OK)
            return false;
        auto equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        auto old = equipped && bot->CanUseItem(equipped) == EQUIP_ERR_OK ? equipped : nullptr;
        if (old && (old->GetItemRandomPropertyId() ||
            (old->GetTemplate()->ItemSet && old->GetTemplate()->ItemSet != proto->ItemSet)))
            return false; // Do not guess random suffix scaling or sacrifice existing set bonuses.
        if (!RoleFloors(bot, role, proto, old, slot))
            return false;
        if (metrics)
            ++metrics->scoreCandidates;
        StatsWeightCalculator calculator(bot);
        calculator.SetQualityBlend(false);
        calculator.SetItemSetBonus(false);
        calculator.SetExcludeResilience(true);
        float score = Score(calculator, proto, slot);
        float oldScore = old ? Score(calculator, old->GetTemplate(), slot, old) : 0.0f;
        if (!IsImprovement(oldScore, score))
        {
            usableNotUpgrade = old && score > 0.0f;
            return false;
        }
        result.item = entry.item;
        result.score = score;
        result.verifiedSource = diagnostic.source.verdict == SourceVerdict::Allowed ||
            (entry.dungeonStage && entry.dungeonStage <= limits.stage);
        result.reason = result.verifiedSource ? "SQL snapshot source permitted" :
            "unverified source accepted under ilvl ceiling, era guard and core/role checks";
        return true;
    }

    bool Retain(Player* bot, uint8 slot, EquipmentSelection& selection, char const* reason)
    {
        selection.choice = {};
        auto old = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (old && bot->CanUseItem(old) == EQUIP_ERR_OK)
        {
            selection.choice.item = old->GetEntry();
            selection.choice.route = CandidateRoute::Retained;
        }
        selection.choice.reason = reason;
        return true;
    }
}

bool SelectEquipmentStep(Player* bot, PrepRole role, RaidProfile const& profile, uint8 slot,
    uint32 exactItem, EquipmentSelection& selection, CandidateIndexValidation& validation,
    std::chrono::steady_clock::time_point deadline)
{
    SearchTurnTrace trace(selection);
    if (SearchClock::now() >= deadline)
        return false;
    auto limits = GetCandidateLimits(profile.map, profile.difficulty, profile.capacity);
    if (!limits.ilvl || !CeilingMatchesRuntime(limits) || !EvaluatorMatchesRole(bot, role))
        return Retain(bot, slot, selection, "unknown/stale ceiling or evaluator/role mismatch; equipment preserved");
    uint8 tab = role == PrepRole::Tank && bot->getClass() == CLASS_DRUID ? 10 : AiFactory::GetPlayerSpecTab(bot);
    uint8 faction = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 2;
    auto prepared = GetPreparedSet(profile.map, profile.difficulty, profile.capacity, bot->getClass(), tab,
        static_cast<uint8>(role), faction, profile.bisKey);
    if (!selection.started)
    {
        auto current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (current && exactItem == current->GetEntry() && bot->CanUseItem(current) == EQUIP_ERR_OK)
            return Retain(bot, slot, selection, "current raid BiS already equipped");
        if (current && current->GetItemRandomPropertyId() && bot->CanUseItem(current) == EQUIP_ERR_OK)
            return Retain(bot, slot, selection, "random-property equipment preserved without speculative comparison");
        selection.started = true;
        selection.previousKeys = GetPreviousBisKeys(limits.stage);
        auto range = GetCandidatePool(slot, limits.ilvl);
        selection.cursor = range.first;
        selection.end = range.second;
    }
    auto evaluate = [&](uint32 item, CandidateRoute route)
    {
        auto entry = GetCandidateMetadata(item);
        EquipmentChoice candidate;
        bool notUpgrade = false;
        auto& metrics = selection.metrics.phases[selection.metrics.activePhase];
        if (entry)
            ++metrics.detailed;
        if (entry && Evaluate(bot, role, profile, slot, *entry, candidate, notUpgrade, &metrics))
        {
            candidate.route = route;
            if (PreferCandidate(candidate.route, candidate.score, candidate.item,
                selection.choice.route, selection.choice.score, selection.choice.item))
                selection.choice = candidate;
            return true;
        }
        if (route == CandidateRoute::CurrentBis && notUpgrade)
            return Retain(bot, slot, selection, "current equipment is at least as suitable as valid current BiS");
        return false;
    };
    if (selection.phase == 0)
    {
        auto record = prepared ? GetPreparedBis(profile.bisKey, bot->getClass(), tab, faction) : nullptr;
        if (prepared && PreparedBisItem(record, slot) == exactItem)
        {
            exactItem = PreparedBisItem(record, slot);
            ++selection.metrics.preparedHits;
        }
        else
            ++selection.metrics.preparedMisses;
        ++selection.phase;
        if (exactItem)
            LogEquipmentSourceDiagnostic(bot, slot, exactItem, profile);
        if (exactItem && evaluate(exactItem, CandidateRoute::CurrentBis))
            return true;
        return false;
    }
    if (selection.phase == 1)
    {
        if (selection.tier < selection.previousKeys.size())
        {
            uint16 key = selection.previousKeys[selection.tier++];
            auto items = sBisListMgr->GetBisFor(key, bot->getClass(), tab, faction);
            auto record = prepared ? GetPreparedBis(key, bot->getClass(), tab, faction) : nullptr;
            auto found = items.find(slot);
            if (prepared && PreparedBisMatches(record, items))
            {
                ++selection.metrics.preparedHits;
                uint32 item = PreparedBisItem(record, slot);
                return item && evaluate(item, CandidateRoute::PreviousBis);
            }
            ++selection.metrics.preparedMisses;
            return found != items.end() && evaluate(found->second, CandidateRoute::PreviousBis);
        }
        ++selection.phase;
        trace.ChangePhase(selection.phase);
        ++selection.metrics.phases[selection.phase].visits;
    }
    auto start = std::chrono::steady_clock::now();
    deadline = std::min(deadline, start + std::chrono::milliseconds(3));
    auto validationStart = SearchClock::now();
    ValidateIndexSlice(validation, std::min(deadline, start + std::chrono::microseconds(400)));
    selection.metrics.validationUs += ElapsedUs(validationStart, SearchClock::now());
    bool classCompatible = ClassIndexMatchesPlayer(bot, slot);
    if (!selection.poolStarted)
    {
        selection.poolStarted = true;
        selection.indexed = validation.complete && validation.compatible && classCompatible;
        selection.indexMode = selection.indexed ? "stage-class16" : !classCompatible ? "legacy-class-override" :
            !validation.complete ? "legacy-validation-pending" : "legacy-snapshot-mismatch";
        auto range = selection.indexed ? GetIndexedCandidatePool(limits.stage, bot->getClass(), slot, limits.ilvl) :
            GetCandidatePool(slot, limits.ilvl);
        selection.cursor = range.first;
        selection.end = range.second;
    }
    else if (selection.indexed && !classCompatible)
    {
        // Restart pool search if normal maintenance grants an out-of-profile proficiency.
        selection.indexed = false;
        selection.indexMode = "legacy-class-override";
        selection.choice = {};
        selection.phase = 2;
        auto range = GetCandidatePool(slot, limits.ilvl);
        selection.cursor = range.first;
        selection.end = range.second;
        ++selection.metrics.restarts;
        trace.ChangePhase(selection.phase);
        return false;
    }
    uint32 examined = 0;
    uint32 scored = 0;
    while (selection.cursor < selection.end && examined++ < 256 && scored < 8 &&
        std::chrono::steady_clock::now() < deadline)
    {
        auto entry = selection.indexed ? GetIndexedPoolCandidate(selection.cursor++) :
            GetPoolCandidate(selection.cursor++);
        ++selection.metrics.phases[selection.phase].scanned;
        if (!entry || CheckCandidateLimits(*entry, limits, bot->GetLevel()) != CandidateRejection::None ||
            static_cast<int32>(entry->requiredLevel) < static_cast<int32>(bot->GetLevel()) - 20)
            continue;
        if (selection.phase == 2 && (!entry->dungeonStage || entry->dungeonStage > limits.stage))
            continue;
        ++scored;
        if (PreparedHintContains(prepared, slot, entry->item))
            ++selection.metrics.hintEvaluations;
        evaluate(entry->item, selection.phase == 2 ? CandidateRoute::Dungeon : CandidateRoute::IlvlFallback);
    }
    if (selection.cursor < selection.end)
    {
        auto& metrics = selection.metrics.phases[selection.phase];
        if (scored >= 8)
            ++metrics.evaluationStops;
        else if (examined >= 256)
            ++metrics.metadataStops;
        else
            ++metrics.timeStops;
        return false;
    }
    if (selection.choice.item)
        return true;
    if (selection.phase == 2)
    {
        ++selection.phase;
        selection.cursor = selection.indexed ? GetIndexedCandidatePool(limits.stage, bot->getClass(), slot,
            limits.ilvl).first : GetCandidatePool(slot, limits.ilvl).first;
        return false;
    }
    return Retain(bot, slot, selection, "no safe improvement; existing equipment and bags preserved");
}

std::string EquipmentSearchSummary(EquipmentSelection const& selection)
{
    auto const& metrics = selection.metrics;
    if (!metrics.started)
        return "search-not-started";
    uint64 work = 0;
    for (auto const& phase : metrics.phases)
        work += phase.workUs;
    std::string result = Acore::StringFormat("index={} turns={} wall_ms={} work_us={} validation_us={} restarts={}",
        selection.indexMode, metrics.turns, ElapsedUs(metrics.startedAt, metrics.finishedAt) / 1000,
        work, metrics.validationUs, metrics.restarts);
    result += Acore::StringFormat(" sets=strict preparedHits={} preparedFallback={} advisoryHints={}",
        metrics.preparedHits, metrics.preparedMisses, metrics.hintEvaluations);
    char const* names[] = {"exact", "previous", "dungeon", "ilvl"};
    for (uint8 phase = 0; phase < 4; ++phase)
    {
        auto const& p = metrics.phases[phase];
        uint64 wall = p.wallUs + (phase == metrics.activePhase ?
            ElapsedUs(metrics.phaseEntered, metrics.finishedAt) : 0);
        result += Acore::StringFormat(" {}[visits={},scan={},eval={},score={},wall_ms={},work_us={},"
            "stop8/256/time={}/{}/{}]",
            names[phase], p.visits, p.scanned, p.detailed, p.scoreCandidates, wall / 1000, p.workUs,
            p.evaluationStops, p.metadataStops, p.timeStops);
    }
    return result;
}

bool RevalidateEquipmentChoice(Player* bot, PrepRole role, RaidProfile const& profile, uint8 slot,
    EquipmentChoice const& choice)
{
    if (!EvaluatorMatchesRole(bot, role))
        return false;
    auto metadata = GetCandidateMetadata(choice.item);
    EquipmentChoice checked;
    bool notUpgrade = false;
    return metadata && Evaluate(bot, role, profile, slot, *metadata, checked, notUpgrade);
}

bool IsOptionalOffhand(Player* bot, PrepRole role)
{
    auto main = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (main && main->GetTemplate()->InventoryType == INVTYPE_2HWEAPON && !bot->CanTitanGrip())
        return true;
    auto tab = AiFactory::GetPlayerSpecTab(bot);
    return (bot->getClass() == CLASS_DRUID && (role == PrepRole::Tank || role == PrepRole::Melee)) ||
        (bot->getClass() == CLASS_WARRIOR && tab == WARRIOR_TAB_ARMS) ||
        (bot->getClass() == CLASS_PALADIN && role == PrepRole::Melee) ||
        (bot->getClass() == CLASS_DEATH_KNIGHT &&
            (role == PrepRole::Tank || tab == DEATH_KNIGHT_TAB_BLOOD || tab == DEATH_KNIGHT_TAB_UNHOLY));
}

char const* DescribeCandidateRoute(CandidateRoute route)
{
    switch (route)
    {
        case CandidateRoute::None: return "none";
        case CandidateRoute::CurrentBis: return "current raid BiS";
        case CandidateRoute::PreviousBis: return "previous tier BiS";
        case CandidateRoute::Dungeon: return "verified dungeon pool";
        case CandidateRoute::IlvlFallback: return "ilvl fallback pool";
        case CandidateRoute::Retained: return "existing equipment retained";
    }
    return "none";
}
}
