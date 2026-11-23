/*
 * Tofu Worldview: support for mod-autonomous-altbots (Tofu fork). See TofuAutonomousAlt.h.
 *
 * Released under the GNU GPL v2 license, like the rest of mod-playerbots.
 */

#include "TofuAutonomousAlt.h"

#include "BudgetValues.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "GroupReference.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "QuestDef.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "Trainer.h"
#include "TravelMgr.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace TofuAutonomousAlt
{
namespace
{
// Registry and settings. Read from map update threads, written from the world thread.
std::shared_mutex g_RegistryMutex;
std::unordered_set<ObjectGuid::LowType> g_Enabled;
std::unordered_set<ObjectGuid::LowType> g_Hardcore;
Settings g_Settings;

// Last time (getMSTime) the Hardcore flee action ran for a character. A flee counts as ongoing for a few seconds after.
std::mutex g_FleeMutex;
std::unordered_map<ObjectGuid::LowType, uint32> g_LastFlee;
constexpr uint32 FLEE_ONGOING_MS = 5 * IN_MILLISECONDS;

// Parties and the quest-giver relay.
std::unordered_map<ObjectGuid::LowType, uint32> g_PartyOf;
std::unordered_map<uint32, uint32> g_PartyRosterSize;

struct PendingGiver
{
    ObjectGuid giver;
    uint32 since = 0;
};

std::mutex g_RelayMutex;
std::unordered_map<ObjectGuid::LowType, PendingGiver> g_PendingGivers;
constexpr uint32 RELAY_TIMEOUT_MS = 60 * IN_MILLISECONDS;
constexpr float RELAY_RANGE = 40.0f;

// Restock trip state per character.
enum class TripPhase : uint8
{
    None,        // no trip; may start one after the cooldown
    Travel,      // walking to a vendor spawn point (rpg status GO_CAMP)
    AtVendor     // vendor targeted (rpg status WANDER_NPC); the rpg code sells, buys, and repairs on arrival
};

struct TripState
{
    TripPhase phase = TripPhase::None;
    uint32 since = 0;          // getMSTime() when the phase or cooldown started
    uint32 cooldown = 0;       // milliseconds to wait in phase None before checking again
    uint32 vendorEntry = 0;
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

std::mutex g_TripMutex;
std::unordered_map<ObjectGuid::LowType, TripState> g_Trips;

constexpr uint32 TRIP_TRAVEL_TIMEOUT_MS = 20 * MINUTE * IN_MILLISECONDS;
constexpr uint32 TRIP_AT_VENDOR_TIMEOUT_MS = 3 * MINUTE * IN_MILLISECONDS;
constexpr uint32 COOLDOWN_NOT_NEEDED_MS = 1 * MINUTE * IN_MILLISECONDS;
constexpr uint32 COOLDOWN_AFTER_TRIP_MS = 5 * MINUTE * IN_MILLISECONDS;
constexpr uint32 COOLDOWN_NO_VENDOR_MS = 5 * MINUTE * IN_MILLISECONDS;
constexpr uint32 COOLDOWN_FAILED_MS = 10 * MINUTE * IN_MILLISECONDS;
constexpr float VENDOR_ARRIVE_DISTANCE = 25.0f;
constexpr float VENDOR_FIND_RANGE = 60.0f;

// Vendor spawns on non-instanced maps that sell unlimited, gold-priced arrows or bullets.
struct AmmoVendorSpawn
{
    uint32 entry;
    float x;
    float y;
    float z;
    std::vector<uint32> ammoItems;
};

std::once_flag g_VendorCacheOnce;
std::unordered_map<uint32, std::vector<AmmoVendorSpawn>> g_AmmoVendorsByMap;

void BuildAmmoVendorCache()
{
    std::unordered_map<uint32, std::vector<uint32>> ammoByEntry;  // creature entry -> ammo item ids (empty = none)
    uint32 spawnCount = 0;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        MapEntry const* mapEntry = sMapStore.LookupEntry(data.mapid);
        if (!mapEntry || mapEntry->Instanceable())
            continue;

        auto known = ammoByEntry.find(data.id);
        if (known == ammoByEntry.end())
        {
            std::vector<uint32> items;
            CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(data.id);
            if (creatureTemplate && (creatureTemplate->npcflag & UNIT_NPC_FLAG_VENDOR))
            {
                if (VendorItemData const* vendorItems = sObjectMgr->GetNpcVendorItemList(data.id))
                {
                    for (VendorItem const* vendorItem : vendorItems->m_items)
                    {
                        if (!vendorItem || vendorItem->maxcount != 0 || vendorItem->ExtendedCost != 0)
                            continue;

                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
                        if (proto && proto->Class == ITEM_CLASS_PROJECTILE &&
                            (proto->SubClass == ITEM_SUBCLASS_ARROW || proto->SubClass == ITEM_SUBCLASS_BULLET))
                            items.push_back(vendorItem->item);
                    }
                }
            }
            known = ammoByEntry.emplace(data.id, std::move(items)).first;
        }

        if (known->second.empty())
            continue;

        g_AmmoVendorsByMap[data.mapid].push_back({data.id, data.posX, data.posY, data.posZ, known->second});
        ++spawnCount;
    }

    LOG_INFO("playerbots", "TofuAutonomousAlt: cached {} ammo vendor spawns on {} maps", spawnCount,
             g_AmmoVendorsByMap.size());
}

// Ammo subclass the equipped ranged weapon fires, or 0 when it needs none.
uint32 RequiredAmmoSubClass(Player* bot)
{
    Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!ranged)
        return 0;

    ItemTemplate const* proto = ranged->GetTemplate();
    if (!proto || proto->Class != ITEM_CLASS_WEAPON)
        return 0;

    switch (proto->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return ITEM_SUBCLASS_ARROW;
        case ITEM_SUBCLASS_WEAPON_GUN:
            return ITEM_SUBCLASS_BULLET;
        default:
            return 0;
    }
}

uint32 CurrentAmmoCount(Player* bot)
{
    uint32 ammoId = bot->GetUInt32Value(PLAYER_AMMO_ID);
    return ammoId ? bot->GetItemCount(ammoId, false) : 0;
}

bool NeedsAmmo(Player* bot, uint32& ammoSubClass)
{
    if (bot->getClass() != CLASS_HUNTER)
        return false;

    ammoSubClass = RequiredAmmoSubClass(bot);
    if (!ammoSubClass)
        return false;

    return CurrentAmmoCount(bot) < g_Settings.ammoThreshold;
}

uint32 FreeMoneyForAmmo(PlayerbotAI* botAI)
{
    return botAI->GetAiObjectContext()
        ->GetValue<uint32>("free money for", std::to_string(static_cast<uint32>(NeedMoneyFor::ammo)))
        ->Get();
}

// Nearest friendly vendor on the bot's map that sells ammo the bot can use and afford. Returns nullptr if none.
AmmoVendorSpawn const* FindAmmoVendor(Player* bot, uint32 ammoSubClass, uint32 budget, float maxDistance)
{
    std::call_once(g_VendorCacheOnce, BuildAmmoVendorCache);

    auto mapItr = g_AmmoVendorsByMap.find(bot->GetMapId());
    if (mapItr == g_AmmoVendorsByMap.end())
        return nullptr;

    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!botFaction)
        return nullptr;

    AmmoVendorSpawn const* best = nullptr;
    float bestDistance = maxDistance;

    for (AmmoVendorSpawn const& spawn : mapItr->second)
    {
        float distance = bot->GetExactDist(spawn.x, spawn.y, spawn.z);
        if (distance >= bestDistance)
            continue;

        CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(spawn.entry);
        if (!creatureTemplate)
            continue;

        FactionTemplateEntry const* vendorFaction = sFactionTemplateStore.LookupEntry(creatureTemplate->faction);
        if (!vendorFaction || vendorFaction->IsHostileTo(*botFaction))
            continue;

        bool affordable = false;
        for (uint32 itemId : spawn.ammoItems)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto || proto->SubClass != ammoSubClass || proto->RequiredLevel > bot->GetLevel())
                continue;

            if (proto->BuyPrice >= 0 && static_cast<uint32>(proto->BuyPrice) <= budget)
            {
                affordable = true;
                break;
            }
        }

        if (!affordable)
            continue;

        best = &spawn;
        bestDistance = distance;
    }

    return best;
}

void StartCooldown(TripState& state, uint32 now, uint32 cooldown)
{
    state.phase = TripPhase::None;
    state.since = now;
    state.cooldown = cooldown;
}
}  // namespace

void SetEnabled(ObjectGuid::LowType guid, bool enabled)
{
    {
        std::unique_lock lock(g_RegistryMutex);
        if (enabled)
            g_Enabled.insert(guid);
        else
            g_Enabled.erase(guid);
    }

    if (!enabled)
    {
        SetHardcore(guid, false);
        ClearBotState(guid);
    }
}

void SetHardcore(ObjectGuid::LowType guid, bool hardcore)
{
    std::unique_lock lock(g_RegistryMutex);
    if (hardcore)
        g_Hardcore.insert(guid);
    else
        g_Hardcore.erase(guid);
}

bool IsHardcore(Player* player)
{
    if (!player)
        return false;

    {
        std::shared_lock lock(g_RegistryMutex);
        if (!g_Hardcore.contains(player->GetGUID().GetCounter()))
            return false;
    }

    return IsActive(player);
}

void MarkFleeing(ObjectGuid::LowType guid)
{
    std::lock_guard lock(g_FleeMutex);
    g_LastFlee[guid] = getMSTime();
}

bool IsFleeing(ObjectGuid::LowType guid)
{
    std::lock_guard lock(g_FleeMutex);
    auto itr = g_LastFlee.find(guid);
    if (itr == g_LastFlee.end())
        return false;

    if (getMSTimeDiff(itr->second, getMSTime()) > FLEE_ONGOING_MS)
    {
        g_LastFlee.erase(itr);
        return false;
    }

    return true;
}

void ReplaceAll(std::vector<ObjectGuid::LowType> const& guids)
{
    std::unique_lock lock(g_RegistryMutex);
    g_Enabled.clear();
    g_Hardcore.clear();
    g_Enabled.insert(guids.begin(), guids.end());
}

bool IsEnabled(ObjectGuid::LowType guid)
{
    std::shared_lock lock(g_RegistryMutex);
    return g_Enabled.contains(guid);
}

void SetSettings(Settings const& settings)
{
    std::unique_lock lock(g_RegistryMutex);
    g_Settings = settings;
}

Settings GetSettings()
{
    std::shared_lock lock(g_RegistryMutex);
    return g_Settings;
}

bool IsActive(Player* player)
{
    if (!player || !IsEnabled(player->GetGUID().GetCounter()))
        return false;

    // A PlayerbotAI means a bot of some kind; a selfbot is a real client in AFK mode, not an autonomous alt.
    return GET_PLAYERBOT_AI(player) != nullptr && !IsSelfBot(player);
}

bool CanControl(Player* bot, Player* from)
{
    if (!bot || !from || !from->GetSession() || !IsRealPlayer(from))
        return false;

    if (from->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
        return true;

    uint32 botAccount = sCharacterCache->GetCharacterAccountIdByGuid(bot->GetGUID());
    uint32 fromAccount = from->GetSession()->GetAccountId();

    if (botAccount && botAccount == fromAccount)
        return true;

    if (sPlayerbotAIConfig.AllowGuildBots && bot->GetGuildId() && bot->GetGuildId() == from->GetGuildId())
        return true;

    if (sPlayerbotAIConfig.AllowTrustedAccountBots && botAccount &&
        sRandomPlayerbotMgr.IsAccountLinked(botAccount, fromAccount))
        return true;

    return false;
}

void SetPartyMember(ObjectGuid::LowType guid, uint32 partyId)
{
    std::unique_lock lock(g_RegistryMutex);
    if (partyId)
        g_PartyOf[guid] = partyId;
    else
        g_PartyOf.erase(guid);
}

void SetPartyRosterSize(uint32 partyId, uint32 size)
{
    std::unique_lock lock(g_RegistryMutex);
    if (size)
        g_PartyRosterSize[partyId] = size;
    else
        g_PartyRosterSize.erase(partyId);
}

void ClearParties()
{
    std::unique_lock lock(g_RegistryMutex);
    g_PartyOf.clear();
    g_PartyRosterSize.clear();
}

uint32 GetPartyId(ObjectGuid::LowType guid)
{
    std::shared_lock lock(g_RegistryMutex);
    auto itr = g_PartyOf.find(guid);
    return itr != g_PartyOf.end() ? itr->second : 0;
}

uint32 GetPartyRosterSize(uint32 partyId)
{
    std::shared_lock lock(g_RegistryMutex);
    auto itr = g_PartyRosterSize.find(partyId);
    return itr != g_PartyRosterSize.end() ? itr->second : 0;
}

bool IsPartyLeader(Player* bot)
{
    if (!bot || !GetPartyId(bot->GetGUID().GetCounter()) || !IsActive(bot))
        return false;

    Group* group = bot->GetGroup();
    return group && group->GetLeaderGUID() == bot->GetGUID();
}

uint32 CountPresentGroupMembers(Player* bot)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return 1;

    float const range = GetSettings().partyPresentRange;
    uint32 count = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
            continue;

        if (member != bot && !member->IsWithinDistInMap(bot, range))
            continue;

        ++count;
    }

    return std::max<uint32>(count, 1);
}

int32 PartyQuestCapable(Player* bot, Quest const* quest, bool forKeeping)
{
    if (!bot || !quest)
        return -1;

    uint32 const partyId = GetPartyId(bot->GetGUID().GetCounter());
    if (!partyId || !IsActive(bot))
        return -1;

    uint32 count = CountPresentGroupMembers(bot);
    if (forKeeping)
        count = std::max(count, GetPartyRosterSize(partyId));

    uint32 const suggested = std::max<uint32>(quest->GetSuggestedPlayers(), 1);
    uint32 needed = 0;
    if (quest->GetType() == 0)
        needed = suggested;
    else if (quest->GetType() == QUEST_TYPE_ELITE)
        needed = std::max(suggested, GetSettings().partyEliteMinMembers);
    else
        return 0;  // dungeon, raid, PvP, escort, and other special quests are not open-world party content

    return count >= needed ? 1 : 0;
}

int32 PartyCanFightElite(Player* bot)
{
    if (!bot || !GetPartyId(bot->GetGUID().GetCounter()) || !IsActive(bot))
        return -1;

    return CountPresentGroupMembers(bot) >= GetSettings().partyEliteMinMembers ? 1 : 0;
}

void NotifyPartyQuestGiver(Player* leader, ObjectGuid giver)
{
    if (!leader || giver.IsEmpty() || !IsPartyLeader(leader))
        return;

    uint32 const partyId = GetPartyId(leader->GetGUID().GetCounter());
    uint32 const now = getMSTime();

    std::vector<ObjectGuid::LowType> members;
    for (GroupReference* ref = leader->GetGroup()->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == leader || !member->IsInWorld() || member->GetMapId() != leader->GetMapId())
            continue;

        if (!member->IsWithinDistInMap(leader, RELAY_RANGE))
            continue;

        if (GetPartyId(member->GetGUID().GetCounter()) != partyId || !IsActive(member))
            continue;

        members.push_back(member->GetGUID().GetCounter());
    }

    std::lock_guard lock(g_RelayMutex);
    for (ObjectGuid::LowType guid : members)
        g_PendingGivers[guid] = {giver, now};
}

bool GetPendingQuestGiver(ObjectGuid::LowType guid, ObjectGuid& giver)
{
    std::lock_guard lock(g_RelayMutex);
    auto itr = g_PendingGivers.find(guid);
    if (itr == g_PendingGivers.end())
        return false;

    if (getMSTimeDiff(itr->second.since, getMSTime()) > RELAY_TIMEOUT_MS)
    {
        g_PendingGivers.erase(itr);
        return false;
    }

    giver = itr->second.giver;
    return true;
}

void ClearPendingQuestGiver(ObjectGuid::LowType guid)
{
    std::lock_guard lock(g_RelayMutex);
    g_PendingGivers.erase(guid);
}

uint32 LearnClassTrainerSpells(Player* bot, uint32* shortfall)
{
    if (!bot)
        return 0;

    // Class trainers that would teach this character (class and race checks are in IsTrainerValidForPlayer).
    std::vector<Trainer::Trainer const*> trainers;
    for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
    {
        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry);
        if (!trainer || trainer->GetTrainerType() != Trainer::Type::Class)
            continue;

        if (!trainer->IsTrainerValidForPlayer(bot))
            continue;

        trainers.push_back(trainer);
    }

    uint32 learned = 0;

    // Several passes, because learning rank N can make rank N+1 teachable in the same level-up.
    for (uint32 pass = 0; pass < 5; ++pass)
    {
        std::vector<std::pair<Trainer::Trainer const*, Trainer::Spell const*>> candidates;
        std::unordered_set<uint32> seen;

        for (Trainer::Trainer const* trainer : trainers)
        {
            for (Trainer::Spell const& spell : trainer->GetSpells())
            {
                if (seen.contains(spell.SpellId))
                    continue;

                if (!PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, trainer, &spell))
                    continue;

                if (!trainer->CanTeachSpell(bot, &spell))
                    continue;

                seen.insert(spell.SpellId);
                candidates.emplace_back(trainer, &spell);
            }
        }

        if (candidates.empty())
            break;

        std::sort(candidates.begin(), candidates.end(),
                  [](auto const& a, auto const& b)
                  {
                      if (a.second->ReqLevel != b.second->ReqLevel)
                          return a.second->ReqLevel < b.second->ReqLevel;
                      return a.second->MoneyCost < b.second->MoneyCost;
                  });

        uint32 learnedThisPass = 0;
        for (auto const& [trainer, spell] : candidates)
        {
            // Re-check: an earlier spell in this pass may have changed what is teachable.
            if (!trainer->CanTeachSpell(bot, spell))
                continue;

            if (bot->GetMoney() < spell->MoneyCost)
            {
                if (shortfall && pass == 0)
                    *shortfall += spell->MoneyCost;
                continue;  // waits for a later level-up when the character has the gold
            }

            bot->ModifyMoney(-static_cast<int32>(spell->MoneyCost));

            // Same as Trainer::TeachSpell: castable entries are "learn" spells that teach through a cast.
            if (spell->IsCastable())
                bot->CastSpell(bot, spell->SpellId, true);
            else
                bot->learnSpell(spell->SpellId, false);

            ++learned;
            ++learnedThisPass;
        }

        if (!learnedThisPass)
            break;
    }

    if (learned)
        LOG_DEBUG("playerbots", "TofuAutonomousAlt: {} learned {} class trainer spell(s)", bot->GetName(), learned);

    return learned;
}

bool UpdateRestock(PlayerbotAI* botAI)
{
    if (!botAI)
        return false;

    Player* bot = botAI->GetBot();
    if (!bot || !IsActive(bot))
        return false;

    Settings const settings = GetSettings();
    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    uint32 const now = getMSTime();

    std::lock_guard lock(g_TripMutex);
    TripState& state = g_Trips[guid];

    // While a real player leads it, the player decides where it goes.
    if (!settings.restock || botAI->HasGameClientMaster())
    {
        if (state.phase != TripPhase::None)
            StartCooldown(state, now, COOLDOWN_AFTER_TRIP_MS);
        return false;
    }

    NewRpgInfo& info = botAI->rpgInfo;

    switch (state.phase)
    {
        case TripPhase::Travel:
        {
            // Something else took over (combat death, a quest, a command): end the trip.
            if (info.GetStatus() != RPG_GO_CAMP || bot->GetMapId() != state.mapId)
            {
                StartCooldown(state, now, COOLDOWN_AFTER_TRIP_MS);
                return false;
            }

            if (getMSTimeDiff(state.since, now) > TRIP_TRAVEL_TIMEOUT_MS)
            {
                StartCooldown(state, now, COOLDOWN_FAILED_MS);
                info.ChangeToIdle();
                return true;
            }

            if (bot->GetExactDist(state.x, state.y, state.z) > VENDOR_ARRIVE_DISTANCE)
                return false;  // keep walking (NewRpgGoCampAction moves toward the spawn point)

            Creature* vendor = bot->FindNearestCreature(state.vendorEntry, VENDOR_FIND_RANGE);
            if (!vendor)
            {
                StartCooldown(state, now, COOLDOWN_FAILED_MS);
                info.ChangeToIdle();
                return true;
            }

            info.ChangeToWanderNpc();
            if (auto* wander = std::get_if<NewRpgInfo::WanderNpc>(&info.data))
            {
                wander->npcOrGo = vendor->GetGUID();
                wander->lastReach = 0;
            }

            state.phase = TripPhase::AtVendor;
            state.since = now;
            return true;
        }
        case TripPhase::AtVendor:
        {
            if (info.GetStatus() == RPG_WANDER_NPC && getMSTimeDiff(state.since, now) < TRIP_AT_VENDOR_TIMEOUT_MS)
                return false;  // the wander-npc action is walking to the vendor or trading with it

            StartCooldown(state, now, COOLDOWN_AFTER_TRIP_MS);
            return false;
        }
        case TripPhase::None:
        default:
        {
            if (getMSTimeDiff(state.since, now) < state.cooldown)
                return false;

            // Only start from idle, so quests and travel already in progress are not interrupted.
            if (info.GetStatus() != RPG_IDLE)
                return false;

            uint32 ammoSubClass = 0;
            if (!NeedsAmmo(bot, ammoSubClass))
            {
                StartCooldown(state, now, COOLDOWN_NOT_NEEDED_MS);
                return false;
            }

            // Not enough gold: keep grinding and looting; check again later.
            AmmoVendorSpawn const* vendor =
                FindAmmoVendor(bot, ammoSubClass, FreeMoneyForAmmo(botAI), settings.vendorSearchDistance);
            if (!vendor)
            {
                StartCooldown(state, now, COOLDOWN_NO_VENDOR_MS);
                return false;
            }

            info.ChangeToGoCamp(WorldPosition(bot->GetMapId(), vendor->x, vendor->y, vendor->z));

            state.phase = TripPhase::Travel;
            state.since = now;
            state.cooldown = 0;
            state.vendorEntry = vendor->entry;
            state.mapId = bot->GetMapId();
            state.x = vendor->x;
            state.y = vendor->y;
            state.z = vendor->z;

            LOG_DEBUG("playerbots", "TofuAutonomousAlt: {} is low on ammo ({}), walking to vendor {}", bot->GetName(),
                      CurrentAmmoCount(bot), vendor->entry);
            return true;
        }
    }
}

void ClearBotState(ObjectGuid::LowType guid)
{
    {
        std::lock_guard lock(g_TripMutex);
        g_Trips.erase(guid);
    }

    {
        std::lock_guard lock(g_FleeMutex);
        g_LastFlee.erase(guid);
    }

    ClearPendingQuestGiver(guid);
    ClearSelfCraftedState(guid);
    SetHubVisit(guid, false);
    SetLfgRoles(guid, 0);
    SetDungeonBusy(guid, false);
}

bool IsMultiboxAlt(Player* bot)
{
    if (!bot || !GetSettings().multiboxEnable)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    Player* master = botAI->GetMaster();
    return master && master != bot && IsRealPlayer(master) && bot->GetGroup() && master->GetGroup() == bot->GetGroup();
}

bool NeedsAmmoRestock(Player* bot)
{
    uint32 ammoSubClass = 0;
    return bot && IsActive(bot) && NeedsAmmo(bot, ammoSubClass);
}

uint32 AmmoSubClassFor(Player* bot) { return bot ? RequiredAmmoSubClass(bot) : 0; }

// ---- Dungeons ----

namespace
{
std::mutex g_DungeonMutex;
std::unordered_map<ObjectGuid::LowType, uint8> g_LfgRoles;
std::unordered_set<ObjectGuid::LowType> g_DungeonBusy;
std::vector<DungeonClearRequest> g_ClearRequests;
std::unordered_map<ObjectGuid::LowType, bool> g_ClearResults;  // group low guid -> all bosses cleared
}  // namespace

void SetLfgRoles(ObjectGuid::LowType guid, uint8 roles)
{
    std::lock_guard lock(g_DungeonMutex);
    if (roles)
        g_LfgRoles[guid] = roles;
    else
        g_LfgRoles.erase(guid);
}

uint8 GetLfgRoles(ObjectGuid::LowType guid)
{
    std::lock_guard lock(g_DungeonMutex);
    auto itr = g_LfgRoles.find(guid);
    return itr == g_LfgRoles.end() ? 0 : itr->second;
}

void SetDungeonBusy(ObjectGuid::LowType guid, bool busy)
{
    std::lock_guard lock(g_DungeonMutex);
    if (busy)
        g_DungeonBusy.insert(guid);
    else
        g_DungeonBusy.erase(guid);
}

bool IsDungeonBusy(ObjectGuid::LowType guid)
{
    std::lock_guard lock(g_DungeonMutex);
    return g_DungeonBusy.contains(guid);
}

bool IsTofuPartyBot(Player* player)
{
    return player && IsActive(player) && GetPartyId(player->GetGUID().GetCounter()) != 0;
}

void RequestDungeonClear(ObjectGuid leader, bool hardcore)
{
    std::lock_guard lock(g_DungeonMutex);
    for (DungeonClearRequest& request : g_ClearRequests)
    {
        if (request.leader == leader)
        {
            request.hardcore = hardcore;
            return;
        }
    }

    DungeonClearRequest request;
    request.leader = leader;
    request.hardcore = hardcore;
    request.since = getMSTime();
    g_ClearRequests.push_back(request);
}

void CancelDungeonClear(ObjectGuid leader)
{
    std::lock_guard lock(g_DungeonMutex);
    g_ClearRequests.erase(std::remove_if(g_ClearRequests.begin(), g_ClearRequests.end(),
                                         [&](DungeonClearRequest const& r) { return r.leader == leader; }),
                          g_ClearRequests.end());
}

std::vector<DungeonClearRequest> GetDungeonClearRequests()
{
    std::lock_guard lock(g_DungeonMutex);
    return g_ClearRequests;
}

void MarkDungeonClearStarted(ObjectGuid leader)
{
    std::lock_guard lock(g_DungeonMutex);
    for (DungeonClearRequest& request : g_ClearRequests)
        if (request.leader == leader)
            request.started = true;
}

void ReportDungeonClearEnded(Player* tank, bool allCleared)
{
    Group* group = tank ? tank->GetGroup() : nullptr;
    if (!group)
        return;

    ObjectGuid const leader = group->GetLeaderGUID();
    std::lock_guard lock(g_DungeonMutex);
    bool tracked = false;
    for (DungeonClearRequest const& request : g_ClearRequests)
        if (request.leader == leader && request.started)
            tracked = true;

    if (tracked)
        g_ClearResults[group->GetGUID().GetCounter()] = allCleared;
}

bool ConsumeDungeonClearResult(ObjectGuid::LowType groupLow, bool& allCleared)
{
    std::lock_guard lock(g_DungeonMutex);
    auto itr = g_ClearResults.find(groupLow);
    if (itr == g_ClearResults.end())
        return false;

    allCleared = itr->second;
    g_ClearResults.erase(itr);
    return true;
}
}  // namespace TofuAutonomousAlt
