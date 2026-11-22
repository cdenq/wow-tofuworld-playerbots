/*
 * Tofu Worldview: restock trips to the guild hub for autonomous alts (mod-autonomous-altbots Tofu fork).
 * See TofuAutonomousAlt.h.
 *
 * The module decides when a party or a solo alt goes home (GetRestockNeeds) and teleports it to the hub. There each
 * bot walks its own errand list: the vendor that buys and repairs (and sells ammo), its class trainer, the trainer of
 * each profession it has, and, for Self Crafted alts, the forge and anvil. The module teleports it back when every
 * member is done or the time cap is reached.
 *
 * Released under the GNU GPL v2 license, like the rest of mod-playerbots.
 */

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "Bag.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureData.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"
#include "TofuAutonomousAlt.h"
#include "Trainer.h"
#include "WorldSession.h"

namespace TofuAutonomousAlt
{
namespace
{
std::shared_mutex g_HubSettingsMutex;
HubSettings g_HubSettings;

enum class ErrandKind : uint8
{
    Vendor,  // sell, repair, ammo
    ClassTrainer,
    ProfessionTrainer,
    Station  // forge and anvil crafts
};

struct Errand
{
    ErrandKind kind;
    ObjectGuid target;
};

struct HubVisit
{
    bool active = false;
    bool done = false;
    bool planned = false;
    std::vector<Errand> errands;
    size_t index = 0;
    uint32 stepSince = 0;
    uint32 lastCast = 0;
};

std::mutex g_VisitMutex;
std::unordered_map<ObjectGuid::LowType, HubVisit> g_Visits;

constexpr float ERRAND_REACH = 4.0f;
constexpr uint32 STATION_IDLE_DONE_MS = 4 * IN_MILLISECONDS;
constexpr uint32 STATION_MAX_MS = 3 * MINUTE * IN_MILLISECONDS;
constexpr uint32 AMMO_TARGET = 1000;  // a hunter fills up to about this much ammo at the hub

struct InRange
{
    WorldObject const* center;
    float range;
    bool operator()(Creature* creature) const
    {
        return creature->IsAlive() && center->IsWithinDistInMap(creature, range);
    }
    bool operator()(GameObject* go) const { return go->isSpawned() && center->IsWithinDistInMap(go, range); }
};

uint32 BagFillPercent(Player* bot)
{
    uint32 total = 0;
    uint32 used = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        ++total;
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++used;
    }

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag* pBag = bot->GetBagByPos(bag);
        if (!pBag || pBag->GetTemplate()->BagFamily)
            continue;  // profession and ammo bags do not count
        for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
        {
            ++total;
            if (pBag->GetItemByPos(slot))
                ++used;
        }
    }

    return total ? used * 100 / total : 0;
}

float LowestDurabilityPercent(Player* bot)
{
    float lowest = 100.0f;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        uint32 const maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (!maxDurability)
            continue;

        lowest = std::min(lowest, 100.0f * item->GetUInt32Value(ITEM_FIELD_DURABILITY) / maxDurability);
    }
    return lowest;
}

void PlanErrands(Player* bot, HubVisit& visit)
{
    HubSettings const hub = GetHubSettings();
    visit.errands.clear();
    visit.index = 0;
    visit.planned = true;

    std::list<Creature*> creatures;
    InRange check{bot, hub.radius};
    Acore::CreatureListSearcher<InRange> creatureSearcher(bot, creatures, check);
    Cell::VisitObjects(bot, creatureSearcher, hub.radius);

    auto nearest = [&](auto predicate) -> Creature*
    {
        Creature* best = nullptr;
        for (Creature* creature : creatures)
            if (predicate(creature) && (!best || bot->GetExactDist(creature) < bot->GetExactDist(best)))
                best = creature;
        return best;
    };

    Creature* vendor = nearest(
        [&](Creature* c)
        {
            if (hub.sellRepairEntry)
                return c->GetEntry() == hub.sellRepairEntry;
            return c->HasNpcFlag(UNIT_NPC_FLAG_REPAIR);
        });
    if (vendor)
        visit.errands.push_back({ErrandKind::Vendor, vendor->GetGUID()});

    Creature* classTrainer = nearest(
        [&](Creature* c)
        {
            Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(c->GetEntry());
            return trainer && trainer->GetTrainerType() == Trainer::Type::Class &&
                   trainer->IsTrainerValidForPlayer(bot);
        });
    if (classTrainer)
        visit.errands.push_back({ErrandKind::ClassTrainer, classTrainer->GetGUID()});

    // One trainer per profession it knows or, for Self Crafted, still has to learn.
    std::vector<uint32> skills;
    if (IsSelfCrafted(bot))
        skills = GetSelfCraftedProfessions(bot->GetGUID().GetCounter());

    std::unordered_set<uint32> done;
    for (Creature* creature : creatures)
    {
        uint32 const skill = TrainerProfessionSkill(creature);
        if (!skill || done.contains(skill))
            continue;
        if (!bot->HasSkill(skill) && std::find(skills.begin(), skills.end(), skill) == skills.end())
            continue;

        done.insert(skill);
        visit.errands.push_back({ErrandKind::ProfessionTrainer, creature->GetGUID()});
    }

    if (IsSelfCrafted(bot))
    {
        std::list<GameObject*> objects;
        Acore::GameObjectListSearcher<InRange> goSearcher(bot, objects, check);
        Cell::VisitObjects(bot, goSearcher, hub.radius);

        GameObject* forge = nullptr;
        for (GameObject* go : objects)
            if (go->GetGOInfo()->type == GAMEOBJECT_TYPE_SPELL_FOCUS && go->GetGOInfo()->spellFocus.focusId == 3 &&
                (!forge || bot->GetExactDist(go) < bot->GetExactDist(forge)))
                forge = go;

        if (forge)
            visit.errands.push_back({ErrandKind::Station, forge->GetGUID()});
    }

    LOG_DEBUG("playerbots", "TofuAutonomousAlt: {} planned {} hub errand(s)", bot->GetName(), visit.errands.size());
}

void BuyHubAmmo(Player* bot, Creature* vendor, uint32 listEntry)
{
    uint32 const subClass = AmmoSubClassFor(bot);
    if (!subClass)
        return;

    VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(listEntry);
    if (!items)
        return;

    // The best ammo it can use from the list.
    uint32 bestSlot = 0;
    ItemTemplate const* best = nullptr;
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* vendorItem = items->GetItem(slot);
        ItemTemplate const* proto = vendorItem ? sObjectMgr->GetItemTemplate(vendorItem->item) : nullptr;
        if (!proto || vendorItem->ExtendedCost || proto->Class != ITEM_CLASS_PROJECTILE || proto->SubClass != subClass)
            continue;
        if (proto->RequiredLevel > bot->GetLevel() || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        if (!best || proto->ItemLevel > best->ItemLevel)
        {
            best = proto;
            bestSlot = slot;
        }
    }

    if (!best)
        return;

    bot->GetSession()->SetCurrentVendor(listEntry);
    for (uint32 i = 0; i < 20 && bot->GetItemCount(best->ItemId, false) < AMMO_TARGET; ++i)
    {
        uint32 const price = best->BuyPrice;
        if (price && bot->GetMoney() < price * 4 / 3)
            break;

        uint32 const before = bot->GetItemCount(best->ItemId, false);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), bestSlot, best->ItemId, 1, NULL_BAG, NULL_SLOT);
        if (bot->GetItemCount(best->ItemId, false) <= before)
            break;
    }
    bot->GetSession()->SetCurrentVendor(0);

    if (bot->GetItemCount(best->ItemId, false) > 0)
        bot->SetAmmo(best->ItemId);
}

// Does the errand's work once the bot stands next to its target. Returns true when the errand is finished.
bool DoErrand(PlayerbotAI* botAI, Player* bot, Errand const& errand, HubVisit& visit, uint32 now)
{
    switch (errand.kind)
    {
        case ErrandKind::Vendor:
        {
            Creature* vendor = botAI->GetCreature(errand.target);
            if (!vendor)
                return true;

            botAI->DoSpecificAction("sell", Event("sell", "vendor"), true);
            bot->DurabilityRepairAll(true, 1.0f, false);

            HubSettings const hub = GetHubSettings();
            if (hub.ammoVendorEntry)
                BuyHubAmmo(bot, vendor, hub.ammoVendorEntry);
            else
                botAI->DoSpecificAction("buy", Event("buy", "vendor"), true);
            return true;
        }
        case ErrandKind::ClassTrainer:
            LearnClassTrainerSpells(bot);
            return true;
        case ErrandKind::ProfessionTrainer:
        {
            Creature* trainer = botAI->GetCreature(errand.target);
            if (!trainer)
                return true;

            LearnProfessions(bot, trainer);
            BuyVendorRecipes(bot, trainer);
            BuyCraftSupplies(bot, trainer);
            return true;
        }
        case ErrandKind::Station:
        {
            if (bot->IsNonMeleeSpellCast(true))
            {
                visit.lastCast = now;
                return false;
            }

            if (getMSTimeDiff(visit.stepSince, now) > STATION_MAX_MS)
                return true;

            if (UpdateCrafting(botAI, true))
            {
                visit.lastCast = now;
                return false;
            }

            return getMSTimeDiff(visit.lastCast ? visit.lastCast : visit.stepSince, now) > STATION_IDLE_DONE_MS;
        }
    }
    return true;
}
}  // namespace

void SetHubSettings(HubSettings const& settings)
{
    std::unique_lock lock(g_HubSettingsMutex);
    g_HubSettings = settings;
}

HubSettings GetHubSettings()
{
    std::shared_lock lock(g_HubSettingsMutex);
    return g_HubSettings;
}

uint32 GetRestockNeeds(Player* bot)
{
    if (!bot || !IsActive(bot))
        return RESTOCK_NONE;

    Settings const settings = GetSettings();
    uint32 needs = RESTOCK_NONE;

    if (BagFillPercent(bot) >= settings.restockBagPct)
        needs |= RESTOCK_BAGS;
    if (LowestDurabilityPercent(bot) < settings.restockDurabilityPct)
        needs |= RESTOCK_DURABILITY;
    // Only when the hub sells ammo; otherwise the ammo trip to a world vendor (UpdateRestock) handles it.
    if (GetHubSettings().ammoVendorEntry && NeedsAmmoRestock(bot))
        needs |= RESTOCK_AMMO;
    if (IsMissingTool(bot))
        needs |= RESTOCK_TOOLS;
    if (HasPendingStationCraft(bot))
        needs |= RESTOCK_STATION;
    if (HasTrainableProfessionRank(bot))
        needs |= RESTOCK_TRAINING;

    return needs;
}

void SetHubVisit(ObjectGuid::LowType guid, bool active)
{
    std::lock_guard lock(g_VisitMutex);
    if (!active)
    {
        g_Visits.erase(guid);
        return;
    }

    HubVisit& visit = g_Visits[guid];
    visit = HubVisit();
    visit.active = true;
    visit.stepSince = getMSTime();
}

bool IsHubVisitActive(ObjectGuid::LowType guid)
{
    std::lock_guard lock(g_VisitMutex);
    auto itr = g_Visits.find(guid);
    return itr != g_Visits.end() && itr->second.active;
}

bool IsHubVisitDone(ObjectGuid::LowType guid)
{
    std::lock_guard lock(g_VisitMutex);
    auto itr = g_Visits.find(guid);
    return itr != g_Visits.end() && itr->second.done;
}

bool UpdateHubErrands(PlayerbotAI* botAI, std::function<bool(WorldObject*)> const& moveTo)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!bot || !bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInCombat())
        return false;

    HubSettings const hub = GetHubSettings();
    if (!hub.enable || bot->GetMapId() != hub.mapId || bot->GetExactDist(hub.x, hub.y, hub.z) > hub.radius)
        return false;  // not arrived yet (the module's teleport is in flight)

    uint32 const now = getMSTime();
    std::lock_guard lock(g_VisitMutex);
    auto itr = g_Visits.find(bot->GetGUID().GetCounter());
    if (itr == g_Visits.end() || !itr->second.active || itr->second.done)
        return false;

    HubVisit& visit = itr->second;
    if (!visit.planned)
    {
        PlanErrands(bot, visit);
        visit.stepSince = now;
    }

    while (visit.index < visit.errands.size())
    {
        Errand const& errand = visit.errands[visit.index];
        WorldObject* target = errand.kind == ErrandKind::Station
                                  ? static_cast<WorldObject*>(botAI->GetGameObject(errand.target))
                                  : static_cast<WorldObject*>(botAI->GetCreature(errand.target));
        if (!target)
        {
            ++visit.index;
            visit.stepSince = now;
            visit.lastCast = 0;
            continue;
        }

        if (!bot->IsWithinDistInMap(target, ERRAND_REACH))
        {
            if (getMSTimeDiff(visit.stepSince, now) > hub.errandStuckMs)
            {
                // Stairs or a navmesh gap: step next to it, as a player would take the short way round.
                bot->NearTeleportTo(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ() + 0.5f,
                                    bot->GetOrientation());
                visit.stepSince = now;
                return true;
            }

            if (moveTo(target))
                return true;

            // Cannot path: next tick the stuck timer takes it there.
            return false;
        }

        if (!DoErrand(botAI, bot, errand, visit, now))
            return true;

        ++visit.index;
        visit.stepSince = now;
        visit.lastCast = 0;
        return true;
    }

    visit.done = true;
    LOG_DEBUG("playerbots", "TofuAutonomousAlt: {} finished its hub errands", bot->GetName());
    return false;
}
}  // namespace TofuAutonomousAlt
