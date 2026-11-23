/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptQuestAction.h"

#include <mutex>
#include <unordered_map>

#include "Event.h"
#include "ObjectAccessor.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"

namespace
{
// Tofu: quest id -> item that starts it (item_template.startquest), built once.
uint32 QuestStarterItem(uint32 questId)
{
    static std::once_flag built;
    static std::unordered_map<uint32, uint32> starters;
    std::call_once(built, []
    {
        for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
            if (proto.StartQuest)
                starters.emplace(proto.StartQuest, itemId);
    });

    auto const it = starters.find(questId);
    return it != starters.end() ? it->second : 0;
}

// Tofu: a shared quest that an item starts and that needs that item to be turned in comes with the item.
void GiveSharedQuestStarterItem(Player* bot, Quest const* quest)
{
    uint32 const itemId = QuestStarterItem(quest->GetQuestId());
    if (!itemId || bot->HasItemCount(itemId, 1, true))
        return;

    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i] == itemId)
        {
            bot->StoreNewItemInBestSlots(itemId, std::max<uint32>(quest->RequiredItemCount[i], 1));
            return;
        }
}
}  // namespace

bool AcceptAllQuestsAction::ProcessQuest(Quest const* quest, Object* questGiver)
{
    if (!AcceptQuest(quest, questGiver->GetGUID())) return false;

    auto text_quest = ChatHelper::FormatQuest(quest);
    bot->PlayDistanceSound(620);

    if (botAI->HasStrategy("debug quest", BotState::BOT_STATE_NON_COMBAT) || botAI->HasStrategy("debug rpg", BotState::BOT_STATE_COMBAT))
    {
        LOG_INFO("playerbots", "{} => Quest [{}] accepted", bot->GetName(), quest->GetTitle());
        std::string text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_accept_debug",
            "Quest [%quest] accepted",
            {{"%quest", text_quest}});
        bot->Say(text, LANG_UNIVERSAL);
    }

    return true;
}

bool AcceptQuestAction::Execute(Event event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    Player* bot = botAI->GetBot();
    uint64_t guid = 0;
    uint32 quest = 0;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(requester);
    quest = ch.extractQuestId(text);

    bool hasAccept = false;

    if (event.getPacket().empty())
    {
        GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
        for (auto i = npcs.begin(); i != npcs.end(); i++)
        {
            Unit* unit = botAI->GetUnit(*i);
            if (unit && quest && unit->hasQuest(quest))
            {
                guid = unit->GetGUID().GetRawValue();
                break;
            }
            if (unit && text == "*" && bot->GetDistance(unit) <= INTERACTION_DISTANCE)
                hasAccept |= QuestAction::ProcessQuests(unit);
        }
        GuidVector gos = AI_VALUE(GuidVector, "nearest game objects no los");
        for (auto i = gos.begin(); i != gos.end(); i++)
        {
            GameObject* go = botAI->GetGameObject(*i);
            if (go && quest && go->hasQuest(quest))
            {
                guid = go->GetGUID().GetRawValue();
                break;
            }
            if (go && text == "*" && bot->GetDistance(go) <= INTERACTION_DISTANCE)
                hasAccept |= QuestAction::ProcessQuests(go);
        }
    }
    else
    {
        WorldPacket& p = event.getPacket();
        p.rpos(0);
        p >> guid >> quest;
    }

    if (!quest || !guid)
        return false;

    Quest const* qInfo = sObjectMgr->GetQuestTemplate(quest);
    if (!qInfo)
        return false;

    hasAccept |= AcceptQuest(qInfo, ObjectGuid(guid));

    if (hasAccept)
    {
        std::stringstream ss;
        ss << "AcceptQuestAction [" << qInfo->GetTitle() << "] - [" << std::to_string(qInfo->GetQuestId()) << "]";
        LOG_DEBUG("playerbots", "{}", ss.str().c_str());
        // botAI->TellMaster(ss.str());
    }

    return hasAccept;
}

bool AcceptQuestShareAction::Execute(Event event)
{
    Player* bot = botAI->GetBot();
    if (!bot->GetDivider())
        return false;  // not a share (the bot opened a quest giver itself)

    // Tofu: SMSG_QUESTGIVER_QUEST_DETAILS is giver guid, divider guid, quest id.
    WorldPacket& p = event.getPacket();
    p.rpos(p.GetOpcode() == SMSG_QUESTGIVER_QUEST_DETAILS ? 16 : 0);
    uint32 quest;
    p >> quest;

    // The player who shared it (the master for the old master-packet path).
    Player* master = ObjectAccessor::FindPlayer(bot->GetDivider());
    if (!master)
        master = GetMaster();

    Quest const* qInfo = sObjectMgr->GetQuestTemplate(quest);
    if (!qInfo || !master)
    {
        bot->SetDivider(ObjectGuid::Empty);
        return false;
    }

    quest = qInfo->GetQuestId();

    if (bot->HasQuest(quest))
    {
        bot->SetDivider(ObjectGuid::Empty);
        botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_already_have_error", "I have this quest", {}));
        return false;
    }

    if (!bot->CanTakeQuest(qInfo, false))
    {
        // can't take quest
        bot->SetDivider(ObjectGuid::Empty);
        botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_cant_take_error", "I can't take this quest", {}));

        return false;
    }

    if (!bot->GetDivider().IsEmpty())
    {
        // send msg to quest giving player
        master->SendPushToPartyResponse(bot, QUEST_PARTY_MSG_ACCEPT_QUEST);
        bot->SetDivider(ObjectGuid::Empty);
    }

    if (bot->CanAddQuest(qInfo, false))
    {
        bot->AddQuest(qInfo, master);
        GiveSharedQuestStarterItem(bot, qInfo);

        if (bot->CanCompleteQuest(quest))
            bot->CompleteQuest(quest);

        // Runsttren: did not add typeid switch from WorldSession::HandleQuestgiverAcceptQuestOpcode!
        // I think it's not needed, cause typeid should be TYPEID_PLAYER - and this one is not handled
        // there and there is no default case also.

        if (qInfo->GetSrcSpell() > 0)
        {
            bot->CastSpell(bot, qInfo->GetSrcSpell(), true);
        }

        botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "quest_accept", "Quest accepted", {}));
        return true;
    }

    return false;
}

bool ConfirmQuestAction::Execute(Event event)
{
    Player* bot = botAI->GetBot();
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    WorldPacket& p = event.getPacket();
    p.rpos(0);
    uint32 quest;
    p >> quest;
    Quest const* qInfo = sObjectMgr->GetQuestTemplate(quest);

    if (!qInfo)
        return false;

    quest = qInfo->GetQuestId();
    if (!bot->CanTakeQuest(qInfo, false))
    {
        // can't take quest
        // botAI->TellError("quest_cant_take");
        return false;
    }

    if (bot->CanAddQuest(qInfo, false))
    {
        bot->AddQuest(qInfo, requester);

        if (bot->CanCompleteQuest(quest))
            bot->CompleteQuest(quest);

        if (qInfo->GetSrcSpell() > 0)
        {
            bot->CastSpell(bot, qInfo->GetSrcSpell(), true);
        }

        // botAI->TellMaster("quest_accept");
        return true;
    }

    return false;
}
