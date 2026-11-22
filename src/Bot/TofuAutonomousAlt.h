/*
 * Tofu Worldview: support for mod-autonomous-altbots (Tofu fork).
 *
 * An "autonomous alt" is an ordinary player character that its owner has enrolled with `.autoalt start`. While no
 * client is driving it, mod-autonomous-altbots logs it in as a masterless playerbot. This file holds the in-memory
 * list of enrolled characters (filled by the module, so playerbots builds and runs without it) and the extra
 * behaviour those characters get: full activity, paid class-trainer spells and automatic talents on level-up,
 * teleporting to a level-appropriate zone on level-up, and paid ammo restocking trips to a vendor.
 *
 * Released under the GNU GPL v2 license, like the rest of mod-playerbots.
 */

#ifndef _PLAYERBOT_TOFUAUTONOMOUSALT_H
#define _PLAYERBOT_TOFUAUTONOMOUSALT_H

#include "Define.h"
#include "ObjectGuid.h"

#include <functional>
#include <string>
#include <vector>

class Creature;
class Player;
class PlayerbotAI;
class Quest;
class Unit;
class WorldObject;
struct ItemTemplate;

namespace TofuAutonomousAlt
{
struct Settings
{
    bool teleportForLevel = true;      // move to a zone for the new level on level-up
    bool learnTrainerSpells = true;    // learn class-trainer spells on level-up, paying the trainer cost
    bool autoTalents = true;           // spend free talent points on level-up in the current tree
    bool restock = true;               // travel to a vendor and buy ammo when it runs low
    uint32 ammoThreshold = 200;        // a hunter with less ammo than this goes shopping
    float vendorSearchDistance = 3000.0f;  // only vendors on the same map within this many yards

    // Hardcore survival (only for alts the module marks Hardcore with SetHardcore).
    float hardcoreRestHealthPct = 80.0f;   // start no new fight below this health
    float hardcoreFleeHealthPct = 30.0f;   // run from a fight below this health
    uint32 hardcoreMaxLevelsAbove = 2;     // grind targets at most this many levels above (stock: 4)

    // Parties (only for alts the module puts in a party with SetPartyMember).
    uint32 partyEliteMinMembers = 3;       // group quests and elite mobs need this many members present
    float partyPresentRange = 100.0f;      // a member counts as present within this many yards, on the same map

    // Restock trips to the guild hub (the module decides when; these are the thresholds it asks about).
    float restockBagPct = 50.0f;           // bags this full (percent of slots) call for a trip
    float restockDurabilityPct = 25.0f;    // lowest equipped item durability below this calls for a trip
};

// The guild hub: a place with class and profession trainers, a forge and an anvil, and a vendor that buys and repairs.
// Written by the module. Errands only look for NPCs and objects within `radius` of the hub point.
struct HubSettings
{
    bool enable = false;
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
    float radius = 120.0f;
    uint32 sellRepairEntry = 0;    // creature entry that buys and repairs (Kreemo)
    uint32 ammoVendorEntry = 0;    // npc_vendor list sold through sellRepairEntry for ammo (0 = its own list)
    uint32 errandStuckMs = 20000;  // a bot that has not reached an errand NPC after this long steps next to it
};

// Restock needs, as bits (GetRestockNeeds).
enum RestockNeed : uint32
{
    RESTOCK_NONE = 0x00,
    RESTOCK_BAGS = 0x01,        // bags at or above restockBagPct
    RESTOCK_DURABILITY = 0x02,  // an equipped item below restockDurabilityPct
    RESTOCK_AMMO = 0x04,        // a hunter below the ammo threshold
    RESTOCK_TOOLS = 0x08,       // Self Crafted: a profession tool is missing
    RESTOCK_STATION = 0x10,     // Self Crafted: a craft that needs a forge or anvil is ready (mats in bags)
    RESTOCK_TRAINING = 0x20     // a profession rank or class spell is waiting at a trainer and affordable
};

// Registry, written by mod-autonomous-altbots.
void SetEnabled(ObjectGuid::LowType guid, bool enabled);
void ReplaceAll(std::vector<ObjectGuid::LowType> const& guids);
bool IsEnabled(ObjectGuid::LowType guid);
void SetSettings(Settings const& settings);
Settings GetSettings();

// True when this character is enrolled and is currently running as a playerbot (not driven by a game client).
bool IsActive(Player* player);

// Hardcore flag, written by mod-autonomous-altbots while the character's Hardcore challenge is active.
void SetHardcore(ObjectGuid::LowType guid, bool hardcore);
// True when IsActive(player) and the character is flagged Hardcore.
bool IsHardcore(Player* player);

// Flee state for Hardcore alts: once a flee starts below the flee threshold it continues until health is back at the
// rest threshold or the fight is over. MarkFleeing is called each time the flee action runs.
void MarkFleeing(ObjectGuid::LowType guid);
bool IsFleeing(ObjectGuid::LowType guid);

// True when `from` may take control of this autonomous alt: a GM, a character on the same account, a member of the
// same guild (if Playerbots.AllowGuildBots), or a linked account (if Playerbots.AllowTrustedAccountBots).
bool CanControl(Player* bot, Player* from);

// Parties, written by mod-autonomous-altbots. A party id of 0 means none. The roster size counts every member,
// online or not.
void SetPartyMember(ObjectGuid::LowType guid, uint32 partyId);
void SetPartyRosterSize(uint32 partyId, uint32 size);
void ClearParties();
uint32 GetPartyId(ObjectGuid::LowType guid);
uint32 GetPartyRosterSize(uint32 partyId);

// True when IsActive(bot), the bot is in a party, and it leads its group.
bool IsPartyLeader(Player* bot);

// Members of the bot's group (the bot included) that are online, alive, on the same map, and within
// partyPresentRange of it. 1 when the bot has no group.
uint32 CountPresentGroupMembers(Player* bot);

// Quest rule for party members: -1 when the bot is not an autonomous alt in a party (use the stock rule), else 1 or 0.
// A normal quest needs its suggested group size present; a group (elite) quest needs at least partyEliteMinMembers.
// With forKeeping, the party roster counts too, so a party that is one member short keeps its quests.
int32 PartyQuestCapable(Player* bot, Quest const* quest, bool forKeeping);

// Elite rule for party members: -1 when not an autonomous alt in a party, else 1 when at least partyEliteMinMembers
// are present.
int32 PartyCanFightElite(Player* bot);

// Quest-giver relay: when a party leader talks to a quest giver, its present party members are told to walk to the
// same giver and accept and turn in there too (a bot leader sends no client packets for followers to copy).
void NotifyPartyQuestGiver(Player* leader, ObjectGuid giver);
bool GetPendingQuestGiver(ObjectGuid::LowType guid, ObjectGuid& giver);
void ClearPendingQuestGiver(ObjectGuid::LowType guid);

// Learns every class-trainer spell the character can learn now and can pay for, cheapest first within each required
// level, deducting the trainer cost. Never teaches profession recipes, riding, or weapon skills. Returns the count.
uint32 LearnClassTrainerSpells(Player* bot);

// Called at the start of NewRpgStatusUpdateAction. Starts, advances, or ends an ammo restocking trip. Returns true
// when it changed the bot's rpg status this tick.
bool UpdateRestock(PlayerbotAI* botAI);

// Forget per-character trip state (on logout or when the character is disabled).
void ClearBotState(ObjectGuid::LowType guid);

// ---- Self Crafted (TofuSelfCrafted.cpp) ----

// Written by the module while the character's Self Crafted challenge is active: the primary profession skill ids it
// learns into free slots, in order. An empty list turns Self Crafted handling off for the character.
void SetSelfCrafted(ObjectGuid::LowType guid, std::vector<uint32> const& professionSkills);
std::vector<uint32> GetSelfCraftedProfessions(ObjectGuid::LowType guid);
// True when IsActive(bot) and the character has a Self Crafted profession list.
bool IsSelfCrafted(Player* bot);
void ClearSelfCraftedState(ObjectGuid::LowType guid);

// Learns, paying the trainer cost: the listed professions into free primary slots, then every rank and recipe the
// trainers teach for the primary professions the character knows. With `trainerNpc`, only that NPC's trainer list
// (a hub errand); without, any trainer in the world. Also buys vendor recipes it can learn when `vendorNpc` is given.
uint32 LearnProfessions(Player* bot, Creature* trainerNpc = nullptr);
uint32 BuyVendorRecipes(Player* bot, Creature* vendorNpc);

// Buys missing profession tools and the vendor-sold reagents the current craft goal needs from this NPC's list.
uint32 BuyCraftSupplies(Player* bot, Creature* vendorNpc);

// One crafting step for a Self Crafted alt: refresh the craft goal, then disenchant, prospect, or craft one item that
// the goal needs. With `atStation`, crafts that need a nearby forge or anvil are allowed when one is in reach.
// Returns true when it cast something this tick.
bool UpdateCrafting(PlayerbotAI* botAI, bool atStation);

// True when a craft that needs a station (forge, anvil) has all its reagents in the bags.
bool HasPendingStationCraft(Player* bot);
// The primary profession a tradeskill trainer NPC teaches (0 = not a profession trainer).
uint32 TrainerProfessionSkill(Creature* creature);
// True when a known primary profession is at its skill cap and a trainer would teach the next rank now.
bool HasTrainableProfessionRank(Player* bot);
// True when a hunter is below the ammo threshold.
bool NeedsAmmoRestock(Player* bot);
// ITEM_SUBCLASS_ARROW or ITEM_SUBCLASS_BULLET for the ranged weapon equipped, 0 for none.
uint32 AmmoSubClassFor(Player* bot);
// True when a gathering or craft spell the character knows needs a tool it does not carry.
bool IsMissingTool(Player* bot);

// The goal as text for `.autoalt status`: professions, skill, and what it is working toward.
std::string DescribeSelfCrafted(Player* bot);

// Grind steering: below 1.0 makes the creature look nearer to GrindTargetValue (skinnable beasts when the goal needs
// leather, humanoids when it needs cloth). 1.0 for everyone else.
float GrindPreferenceFactor(Player* bot, Unit* unit);
// New rpg steering: the status the goal wants from idle (RPG_DO_GATHER for ore, RPG_GO_GRIND for leather or cloth),
// or -1 for no preference.
int32 PreferredRpgStatus(PlayerbotAI* botAI);

// Item usage for Self Crafted: equipment the character can never wear because it did not craft it. Enchanters
// disenchant it (DISENCHANT); everyone else sells it.
bool IsUnwearableForSelfCrafted(Player* bot, ItemTemplate const* proto);
bool CanDisenchantForSelfCrafted(Player* bot, ItemTemplate const* proto);
// True while ItemUsageValue should judge an item as if this character had crafted it (set by the craft goal search,
// or when the character carries a copy it crafted itself).
bool AssumeCraftedByBot(Player* bot, uint32 itemId);

// ---- Restock and the guild hub (TofuHub.cpp) ----

void SetHubSettings(HubSettings const& settings);
HubSettings GetHubSettings();

// What a trip to the hub would fix now (RestockNeed bits). World thread.
uint32 GetRestockNeeds(Player* bot);

// Hub visit state per character, written by the module: active from the teleport in until the teleport back.
void SetHubVisit(ObjectGuid::LowType guid, bool active);
bool IsHubVisitActive(ObjectGuid::LowType guid);
bool IsHubVisitDone(ObjectGuid::LowType guid);

// One step of the errand list while a hub visit is active: walk to the vendor (sell, repair, ammo), the class
// trainer, each profession trainer (ranks, recipes, tools, reagents), and the forge and anvil (station crafts).
// Marks the visit done after the last errand. Returns true when it moved or acted.
// `moveTo` walks the bot toward an object (the action's MovementAction::MoveTo); it returns false when it cannot.
bool UpdateHubErrands(PlayerbotAI* botAI, std::function<bool(WorldObject*)> const& moveTo);

// ---- Dungeons (Phase 2) ----

// Dungeon Finder role override for party members (PLAYER_ROLE_* bits; 0 = playerbots' own choice).
void SetLfgRoles(ObjectGuid::LowType guid, uint8 roles);
uint8 GetLfgRoles(ObjectGuid::LowType guid);

// Set by the module while a party is queued or inside its dungeon: members accept the proposal, keep their group,
// and decline outside invites.
void SetDungeonBusy(ObjectGuid::LowType guid, bool busy);
bool IsDungeonBusy(ObjectGuid::LowType guid);

// True when IsActive(player) and it is in a party (dungeon-clear checks this before taking its commands).
bool IsTofuPartyBot(Player* player);

// Dungeon clear requests: the module asks for a run when its party is queued; mod-dungeon-clear (Tofu fork) starts the
// clear once the whole group is inside and reports when the run ends.
struct DungeonClearRequest
{
    ObjectGuid leader;       // the party's group leader
    bool hardcore = false;   // no post-combat resurrection
    bool started = false;
    uint32 since = 0;        // getMSTime() of the request
};

void RequestDungeonClear(ObjectGuid leader, bool hardcore);
void CancelDungeonClear(ObjectGuid leader);
std::vector<DungeonClearRequest> GetDungeonClearRequests();
void MarkDungeonClearStarted(ObjectGuid leader);
// Called by mod-dungeon-clear when a run on `tank`'s group ends (map thread).
void ReportDungeonClearEnded(Player* tank, bool allCleared);
// The module reads the result once per group.
bool ConsumeDungeonClearResult(ObjectGuid::LowType groupLow, bool& allCleared);
}  // namespace TofuAutonomousAlt

#endif
