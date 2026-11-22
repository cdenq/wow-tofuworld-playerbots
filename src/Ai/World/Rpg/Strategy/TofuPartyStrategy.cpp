/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Tofu Worldview: quest-giver relay for autonomous alt parties. See TofuPartyStrategy.h.

#include "TofuPartyStrategy.h"

#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "TofuAutonomousAlt.h"

void TofuPartyStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above following the leader, so the follower walks to the giver and back.
    triggers.push_back(
        new TriggerNode("tofu party quest giver", { NextAction("tofu party quest giver", ACTION_HIGH + 2) }));
}

bool TofuPartyQuestGiverTrigger::IsActive()
{
    if (bot->IsInCombat() || !bot->IsAlive())
        return false;

    ObjectGuid giver;
    return TofuAutonomousAlt::GetPendingQuestGiver(bot->GetGUID().GetCounter(), giver);
}

bool TofuPartyQuestGiverAction::Execute(Event /*event*/)
{
    ObjectGuid::LowType const self = bot->GetGUID().GetCounter();

    ObjectGuid giver;
    if (!TofuAutonomousAlt::GetPendingQuestGiver(self, giver))
        return false;

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, giver);
    if (!object || !object->IsInWorld() || object->GetMapId() != bot->GetMapId())
    {
        TofuAutonomousAlt::ClearPendingQuestGiver(self);
        return false;
    }

    if (!IsWithinInteractionDist(object))
        return MoveWorldObjectTo(giver);

    InteractWithNpcOrGameObjectForQuest(giver);
    TofuAutonomousAlt::ClearPendingQuestGiver(self);
    return true;
}
