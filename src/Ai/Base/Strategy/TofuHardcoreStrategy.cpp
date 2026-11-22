/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Tofu Worldview: combat survival for Hardcore autonomous alts. See TofuHardcoreStrategy.h.

#include "TofuHardcoreStrategy.h"

#include "Playerbots.h"
#include "TofuAutonomousAlt.h"

#include <cmath>

namespace
{
constexpr float FLEE_STEP_DISTANCE = 30.0f;
}

void TofuHardcoreStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above every combat action, including emergency heals: a Hardcore alt runs below the flee threshold.
    triggers.push_back(
        new TriggerNode("tofu hardcore flee", { NextAction("tofu hardcore flee", ACTION_EMERGENCY + 5) }));

    // Out of combat: eat back up to the rest threshold, above roaming and questing.
    triggers.push_back(new TriggerNode("tofu hardcore rest", { NextAction("eat", ACTION_HIGH + 1) }));
}

bool TofuHardcoreRestTrigger::IsActive()
{
    if (!bot->IsAlive() || bot->IsInCombat() || !TofuAutonomousAlt::IsHardcore(bot))
        return false;

    return bot->GetHealthPct() < TofuAutonomousAlt::GetSettings().hardcoreRestHealthPct;
}

bool TofuHardcoreFleeTrigger::IsActive()
{
    if (!bot->IsAlive() || !bot->IsInCombat() || !TofuAutonomousAlt::IsHardcore(bot))
        return false;

    TofuAutonomousAlt::Settings const settings = TofuAutonomousAlt::GetSettings();
    float const health = bot->GetHealthPct();

    if (health < settings.hardcoreFleeHealthPct)
        return true;

    // Keep running once started, until health is back at the rest threshold (usually: until the fight is over).
    return health < settings.hardcoreRestHealthPct && TofuAutonomousAlt::IsFleeing(bot->GetGUID().GetCounter());
}

bool TofuHardcoreFleeAction::Execute(Event /*event*/)
{
    // Centre of the living attackers; run directly away from it.
    float sumX = 0.0f;
    float sumY = 0.0f;
    uint32 count = 0;

    GuidVector const attackers = AI_VALUE(GuidVector, "attackers");
    for (ObjectGuid const& guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->GetMapId() != bot->GetMapId())
            continue;

        sumX += unit->GetPositionX();
        sumY += unit->GetPositionY();
        ++count;
    }

    if (!count)
    {
        if (Unit* victim = bot->GetVictim())
        {
            sumX = victim->GetPositionX();
            sumY = victim->GetPositionY();
            count = 1;
        }
        else
            return false;
    }

    float dx = bot->GetPositionX() - sumX / count;
    float dy = bot->GetPositionY() - sumY / count;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.1f)
    {
        // Standing on top of the attackers: run the way the bot is not facing.
        float const angle = bot->GetOrientation() + float(M_PI);
        dx = std::cos(angle);
        dy = std::sin(angle);
        length = 1.0f;
    }

    float const x = bot->GetPositionX() + dx / length * FLEE_STEP_DISTANCE;
    float const y = bot->GetPositionY() + dy / length * FLEE_STEP_DISTANCE;
    float z = bot->GetPositionZ();
    bot->UpdateAllowedPositionZ(x, y, z);

    TofuAutonomousAlt::MarkFleeing(bot->GetGUID().GetCounter());

    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);
    bot->AttackStop();

    return MoveTo(bot->GetMapId(), x, y, z, false, false, false, false, MovementPriority::MOVEMENT_FORCED);
}
