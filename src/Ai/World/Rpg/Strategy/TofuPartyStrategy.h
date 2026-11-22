/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Tofu Worldview: quest-giver relay for autonomous alt parties (see Bot/TofuAutonomousAlt.h).
// Strategy "tofu party" (non-combat): mod-autonomous-altbots adds it to party followers. When the party leader talks
// to a quest giver (NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest), its present members get that giver as a
// pending target. Trigger "tofu party quest giver": a pending giver exists and the bot is out of combat. Action
// "tofu party quest giver": walk to the giver, then accept and turn in there with the new rpg quest code.

#ifndef PLAYERBOTS_TOFUPARTYSTRATEGY_H
#define PLAYERBOTS_TOFUPARTYSTRATEGY_H

#include "NewRpgBaseAction.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

class TofuPartyStrategy : public Strategy
{
public:
    TofuPartyStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "tofu party"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class TofuPartyQuestGiverTrigger : public Trigger
{
public:
    TofuPartyQuestGiverTrigger(PlayerbotAI* botAI) : Trigger(botAI, "tofu party quest giver") {}

    bool IsActive() override;
};

class TofuPartyQuestGiverAction : public NewRpgBaseAction
{
public:
    TofuPartyQuestGiverAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "tofu party quest giver") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

#endif
