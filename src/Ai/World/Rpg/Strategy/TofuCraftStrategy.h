/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Tofu Worldview: Self Crafted crafting and guild hub errands for autonomous alts (see Bot/TofuAutonomousAlt.h).
// Strategy "tofu self crafted" (non-combat): mod-autonomous-altbots adds it while the alt's Self Crafted challenge is
// active. Trigger "tofu craft" fires every few seconds out of combat; action "tofu craft" refreshes the craft goal and
// crafts, disenchants, or prospects one step toward it (TofuAutonomousAlt::UpdateCrafting).
// Strategy "tofu hub" (non-combat): added while the module has the alt on a restock trip to the guild hub. Trigger
// "tofu hub errand" fires while the visit is unfinished; action "tofu hub errand" walks the errand list
// (TofuAutonomousAlt::UpdateHubErrands).

#ifndef PLAYERBOTS_TOFUCRAFTSTRATEGY_H
#define PLAYERBOTS_TOFUCRAFTSTRATEGY_H

#include "MovementActions.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

class TofuSelfCraftedStrategy : public Strategy
{
public:
    TofuSelfCraftedStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "tofu self crafted"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class TofuHubStrategy : public Strategy
{
public:
    TofuHubStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "tofu hub"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class TofuCraftTrigger : public Trigger
{
public:
    TofuCraftTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tofu craft") {}

    bool IsActive() override;

private:
    uint32 lastCheck = 0;
};

class TofuHubErrandTrigger : public Trigger
{
public:
    TofuHubErrandTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tofu hub errand") {}

    bool IsActive() override;
};

class TofuCraftAction : public Action
{
public:
    TofuCraftAction(PlayerbotAI* botAI) : Action(botAI, "tofu craft") {}

    bool Execute(Event event) override;
};

class TofuHubErrandAction : public MovementAction
{
public:
    TofuHubErrandAction(PlayerbotAI* botAI) : MovementAction(botAI, "tofu hub errand") {}

    bool Execute(Event event) override;
};

#endif
