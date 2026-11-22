/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Tofu Worldview: Self Crafted crafting and guild hub errands. See TofuCraftStrategy.h.

#include "TofuCraftStrategy.h"

#include "Playerbots.h"
#include "Timer.h"
#include "TofuAutonomousAlt.h"

namespace
{
constexpr uint32 CRAFT_CHECK_MS = 3 * IN_MILLISECONDS;
}

void TofuSelfCraftedStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Just above the new rpg movement actions (3.0): a craft cast interrupts walking for its cast time, then the rpg
    // carries on. The trigger only fires every few seconds.
    triggers.push_back(new TriggerNode("tofu craft", {NextAction("tofu craft", 4.0f)}));
}

void TofuHubStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("tofu hub errand", {NextAction("tofu hub errand", ACTION_HIGH + 3)}));
}

bool TofuCraftTrigger::IsActive()
{
    uint32 const now = getMSTime();
    if (lastCheck && getMSTimeDiff(lastCheck, now) < CRAFT_CHECK_MS)
        return false;

    lastCheck = now ? now : 1;
    if (bot->IsInCombat() || !bot->IsAlive() || bot->IsNonMeleeSpellCast(true))
        return false;

    // Not between pulls in a dungeon.
    if (bot->GetMap() && bot->GetMap()->IsDungeon())
        return false;

    return TofuAutonomousAlt::IsSelfCrafted(bot);
}

bool TofuHubErrandTrigger::IsActive()
{
    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    return TofuAutonomousAlt::IsHubVisitActive(guid) && !TofuAutonomousAlt::IsHubVisitDone(guid);
}

bool TofuCraftAction::Execute(Event /*event*/) { return TofuAutonomousAlt::UpdateCrafting(botAI, false); }

bool TofuHubErrandAction::Execute(Event /*event*/)
{
    return TofuAutonomousAlt::UpdateHubErrands(botAI, [this](WorldObject* target) { return MoveTo(target, 2.0f); });
}
