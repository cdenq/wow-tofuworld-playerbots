/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Tofu Worldview: combat survival for Hardcore autonomous alts (see Bot/TofuAutonomousAlt.h).
// Strategy "tofu hardcore": mod-autonomous-altbots adds it to the combat and non-combat engines of every enrolled alt
// whose Hardcore challenge is active.
// - Trigger "tofu hardcore flee" (combat): health below the flee threshold, or a flee already under way and health
//   still below the rest threshold. Action "tofu hardcore flee": run 30 yards away from the attackers, again each
//   tick, until the fight ends (the mobs give up) or health recovers.
// - Trigger "tofu hardcore rest" (out of combat): health below the rest threshold. Action: the stock "eat", so the
//   bot recovers before GrindTargetValue lets it pick a new fight.

#ifndef PLAYERBOTS_TOFUHARDCORESTRATEGY_H
#define PLAYERBOTS_TOFUHARDCORESTRATEGY_H

#include "MovementActions.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

class TofuHardcoreStrategy : public Strategy
{
public:
    TofuHardcoreStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "tofu hardcore"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class TofuHardcoreFleeTrigger : public Trigger
{
public:
    TofuHardcoreFleeTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tofu hardcore flee") {}

    bool IsActive() override;
};

class TofuHardcoreRestTrigger : public Trigger
{
public:
    TofuHardcoreRestTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tofu hardcore rest") {}

    bool IsActive() override;
};

class TofuHardcoreFleeAction : public MovementAction
{
public:
    TofuHardcoreFleeAction(PlayerbotAI* botAI) : MovementAction(botAI, "tofu hardcore flee") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

#endif
