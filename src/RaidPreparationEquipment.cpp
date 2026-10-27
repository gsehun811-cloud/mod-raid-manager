#include "RaidPreparationEquipment.h"
#include "Item.h"
#include "Bag.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
namespace RaidManager
{
bool ApplyPreparedItem(Player* bot, uint8 slot, uint32 item, std::string& reason)
{
    auto proto = sObjectMgr->GetItemTemplate(item);
    if (!proto || slot >= EQUIPMENT_SLOT_END || slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
    {
        reason = "invalid BiS item/slot";
        return false;
    }
    if (Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); current && current->GetEntry() == item)
    {
        if (proto->RequiredLevel <= bot->GetLevel() && bot->CanUseItem(proto) == EQUIP_ERR_OK &&
            static_cast<int32>(proto->Quality) <= sPlayerbotAIConfig.autoGearQualityLimit)
            return true;
        reason = "existing BiS entry no longer meets use requirements";
        return false;
    }
    Item* staged = nullptr;
    for (uint8 index = INVENTORY_SLOT_ITEM_START; index < INVENTORY_SLOT_ITEM_END && !staged; ++index)
        if (auto owned = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, index); owned && owned->GetEntry() == item)
            staged = owned;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END && !staged; ++bag)
        if (auto container = bot->GetBagByPos(bag))
            for (uint32 index = 0; index < container->GetBagSize() && !staged; ++index)
                if (auto owned = container->GetItemByPos(index); owned && owned->GetEntry() == item)
                    staged = owned;
    uint16 equipmentPosition = 0;
    InventoryResult equip = staged ? bot->CanEquipItem(slot, equipmentPosition, staged, true) :
        bot->CanEquipNewItem(slot, equipmentPosition, item, true);
    if (proto->RequiredLevel > bot->GetLevel() || bot->CanUseItem(proto) != EQUIP_ERR_OK)
    {
        reason = "wear requirements not met (level/class/race/skill/reputation), core result " +
            std::to_string(static_cast<uint32>(bot->CanUseItem(proto)));
        return false;
    }
    if (static_cast<int32>(proto->Quality) > sPlayerbotAIConfig.autoGearQualityLimit)
    {
        reason = "candidate exceeds Playerbots quality safety limit";
        return false;
    }
    if (equip != EQUIP_ERR_OK)
    {
        reason = "slot/weapon/unique-equipped restriction, core result " + std::to_string(static_cast<uint32>(equip));
        return false;
    }
    Item* old = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    ItemPosCountVec oldStorage;
    if (old && bot->CanUnequipItem((INVENTORY_SLOT_BAG_0 << 8) | slot, false) != EQUIP_ERR_OK)
    {
        reason = "old equipment cannot be unequipped; preserved";
        return false;
    }
    if (old && bot->CanStoreItem(NULL_BAG, NULL_SLOT, oldStorage, old, false) != EQUIP_ERR_OK)
    {
        reason = "insufficient compatible bag space to preserve old equipment";
        return false;
    }
    // Stage the new item in inventory, then use the same validated swap path as a player.
    // Do not delete existing items or grant reputation to make a candidate fit.
    ItemPosCountVec storage;
    if (!staged && bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, storage, item, 1) != EQUIP_ERR_OK)
    {
        reason = "no inventory space to stage BiS item";
        return false;
    }
    Item* created = staged ? staged : bot->StoreNewItem(storage, item, true);
    if (!created)
    {
        reason = "item creation failed";
        return false;
    }
    ObjectGuid createdGuid = created->GetGUID();
    bot->SwapItem((created->GetBagSlot() << 8) | created->GetSlot(), (INVENTORY_SLOT_BAG_0 << 8) | slot);
    Item* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (equipped && equipped->GetGUID() == createdGuid)
        return true;
    // The newly staged item stays in bags on failure. Never undo existing item changes or destroy it.
    reason = "equip verification failed; staged item retained in inventory";
    return false;
}
}
