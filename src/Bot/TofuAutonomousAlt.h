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

#include <vector>

class Player;
class PlayerbotAI;

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

// Learns every class-trainer spell the character can learn now and can pay for, cheapest first within each required
// level, deducting the trainer cost. Never teaches profession recipes, riding, or weapon skills. Returns the count.
uint32 LearnClassTrainerSpells(Player* bot);

// Called at the start of NewRpgStatusUpdateAction. Starts, advances, or ends an ammo restocking trip. Returns true
// when it changed the bot's rpg status this tick.
bool UpdateRestock(PlayerbotAI* botAI);

// Forget per-character trip state (on logout or when the character is disabled).
void ClearBotState(ObjectGuid::LowType guid);
}  // namespace TofuAutonomousAlt

#endif
