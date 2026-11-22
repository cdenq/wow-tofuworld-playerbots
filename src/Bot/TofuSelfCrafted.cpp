/*
 * Tofu Worldview: Self Crafted autonomous alts (mod-autonomous-altbots Tofu fork). See TofuAutonomousAlt.h.
 *
 * A Self Crafted character may only wear what it crafted itself (mod-challenge-modes). These alts learn a fixed list
 * of professions, pick a craft goal (the best upgrade they can craft, else a recipe that still gives skill-ups), and
 * steer their play toward the materials the goal needs: mining for ore, skinnable beasts for leather, humanoids for
 * cloth, disenchanting for enchanting materials, prospecting for gems, and vendors for the rest.
 *
 * Released under the GNU GPL v2 license, like the rest of mod-playerbots.
 */

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "Bag.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "TofuAutonomousAlt.h"
#include "Trainer.h"
#include "WorldSession.h"

namespace TofuAutonomousAlt
{
namespace
{
constexpr uint32 SPELL_DISENCHANT = 13262;
constexpr uint32 SPELL_PROSPECTING = 31252;
constexpr uint32 SPELL_MINING = 2575;  // the base gather spells playerbots casts
constexpr uint32 SPELL_SKINNING = 8613;
constexpr uint32 PROSPECT_STACK = 5;
constexpr uint32 GOAL_REFRESH_MS = 60 * IN_MILLISECONDS;
constexpr uint32 LEARN_REFRESH_MS = 5 * MINUTE * IN_MILLISECONDS;
constexpr uint32 STATION_ORE_STACK = 20;  // this much ore for a known smelting recipe makes a forge trip worth it
constexpr float STATION_SEARCH_RANGE = 15.0f;

// What the craft goal still needs from the world, as bits.
enum MatNeed : uint32
{
    NEED_ORE = 0x01,
    NEED_LEATHER = 0x02,
    NEED_CLOTH = 0x04,
    NEED_VENDOR = 0x08
};

std::shared_mutex g_ListMutex;
std::unordered_map<ObjectGuid::LowType, std::vector<uint32>> g_SelfCrafted;

struct CraftState
{
    uint32 nextGoalRefresh = 0;
    uint32 nextLearn = 0;
    uint32 goalSpell = 0;  // the recipe the goal ends in (0 = none)
    uint32 goalItem = 0;
    bool goalIsGear = false;
    uint32 needs = 0;           // MatNeed bits
    bool stationReady = false;  // a station craft has its reagents
    bool missingTool = false;
    std::vector<std::pair<uint32, uint32>> vendorReagents;  // item, count still missing
    std::unordered_set<uint32> missingToolCategories;
    std::unordered_set<uint32> missingToolItems;
    std::string text;
};

std::mutex g_CraftMutex;
std::unordered_map<ObjectGuid::LowType, CraftState> g_Craft;

// ItemUsageValue asks AssumeCraftedByBot while the goal search evaluates a recipe's item.
thread_local uint32 t_AssumeCraftedItem = 0;

struct ProfessionTrainerSpell
{
    Trainer::Trainer const* trainer;
    Trainer::Spell const* spell;
    uint32 learnedSpell;
};

std::once_flag g_IndexOnce;
std::unordered_map<uint32, std::vector<ProfessionTrainerSpell>> g_TrainerSpellsBySkill;  // skill -> trainer spells
std::unordered_set<uint32> g_VendorSoldItems;  // sold for gold, unlimited, somewhere

uint32 SkillOfSpell(uint32 spellId)
{
    SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
    {
        SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(itr->second->SkillLine);
        if (skillLine && skillLine->categoryId == SKILL_CATEGORY_PROFESSION)
            return skillLine->id;
    }
    return 0;
}

uint32 LearnedSpellOf(uint32 trainerSpellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(trainerSpellId);
    if (!info)
        return 0;

    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
            return effect.TriggerSpell;

    return trainerSpellId;
}

void BuildIndex()
{
    for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
    {
        if (Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry))
        {
            if (trainer->GetTrainerType() == Trainer::Type::Tradeskill)
            {
                for (Trainer::Spell const& spell : trainer->GetSpells())
                {
                    uint32 const learned = LearnedSpellOf(spell.SpellId);
                    uint32 const skill = learned ? SkillOfSpell(learned) : 0;
                    if (skill)
                        g_TrainerSpellsBySkill[skill].push_back({trainer, &spell, learned});
                }
            }
        }

        if (!(creatureTemplate.npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;

        if (VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry))
            for (VendorItem const* vendorItem : items->m_items)
                if (vendorItem && vendorItem->maxcount == 0 && vendorItem->ExtendedCost == 0)
                    g_VendorSoldItems.insert(vendorItem->item);
    }

    LOG_INFO("playerbots",
             "TofuAutonomousAlt: Self Crafted index: {} profession(s) with trainer spells, {} vendor item(s)",
             g_TrainerSpellsBySkill.size(), g_VendorSoldItems.size());
}

void EnsureIndex() { std::call_once(g_IndexOnce, BuildIndex); }

bool IsPrimaryProfessionSkill(uint32 skill)
{
    SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(skill);
    return skillLine && skillLine->categoryId == SKILL_CATEGORY_PROFESSION;
}

// Every primary profession the character knows (its list or not: existing professions are kept and used).
std::vector<uint32> KnownProfessions(Player* bot)
{
    std::vector<uint32> skills;
    EnsureIndex();
    for (auto const& [skill, spells] : g_TrainerSpellsBySkill)
        if (bot->HasSkill(skill))
            skills.push_back(skill);
    return skills;
}

SpellEffectInfo const* CreateItemEffect(SpellInfo const* info)
{
    if (!info)
        return nullptr;
    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.IsEffect(SPELL_EFFECT_CREATE_ITEM) && effect.ItemType)
            return &effect;
    return nullptr;
}

struct Recipe
{
    uint32 spellId;
    uint32 itemId;
    uint32 skill;
    SpellInfo const* info;
};

// Known craft recipes of the character's primary professions, keyed by the item they make.
std::multimap<uint32, Recipe> KnownRecipes(Player* bot)
{
    std::multimap<uint32, Recipe> recipes;
    std::vector<uint32> const professions = KnownProfessions(bot);
    std::unordered_set<uint32> const professionSet(professions.begin(), professions.end());

    for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        SpellEffectInfo const* effect = CreateItemEffect(info);
        if (!effect || info->Reagent[0] <= 0)
            continue;

        uint32 const skill = SkillOfSpell(spellId);
        if (!skill || !professionSet.contains(skill))
            continue;

        recipes.emplace(effect->ItemType, Recipe{spellId, effect->ItemType, skill, info});
    }

    return recipes;
}

bool FocusInReach(Player* bot, uint32 focusId)
{
    if (!focusId)
        return true;

    GameObject* found = nullptr;
    Acore::GameObjectFocusCheck check(bot, focusId);
    Acore::GameObjectSearcher<Acore::GameObjectFocusCheck> searcher(bot, found, check);
    Cell::VisitObjects(bot, searcher, STATION_SEARCH_RANGE);
    return found != nullptr;
}

bool HasTools(Player* bot, SpellInfo const* info, CraftState* state)
{
    bool ok = true;
    for (uint32 category : info->TotemCategory)
    {
        if (category && !bot->HasItemTotemCategory(category))
        {
            ok = false;
            if (state)
                state->missingToolCategories.insert(category);
        }
    }

    for (uint32 item : info->Totem)
    {
        if (item && !bot->HasItemCount(item, 1))
        {
            ok = false;
            if (state)
                state->missingToolItems.insert(item);
        }
    }
    return ok;
}

// Works out what is missing for `count` casts of `recipe`; true when every missing reagent has a source.
bool ResolveNeeds(Player* bot, std::multimap<uint32, Recipe> const& recipes, Recipe const& recipe, uint32 count,
                  uint32 depth, CraftState& state, std::vector<uint32>& chain)
{
    if (depth > 2)
        return false;

    HasTools(bot, recipe.info, &state);
    bool const isJewelcrafter = bot->HasSkill(SKILL_JEWELCRAFTING);

    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (recipe.info->Reagent[i] <= 0)
            continue;

        uint32 const item = static_cast<uint32>(recipe.info->Reagent[i]);
        uint32 const need = recipe.info->ReagentCount[i] * count;
        uint32 const have = bot->GetItemCount(item, false);
        if (have >= need)
            continue;

        uint32 const missing = need - have;

        auto const sub = recipes.find(item);
        if (sub != recipes.end())
        {
            if (!ResolveNeeds(bot, recipes, sub->second, missing, depth + 1, state, chain))
                return false;
            chain.push_back(sub->second.spellId);
            continue;
        }

        if (g_VendorSoldItems.contains(item))
        {
            state.vendorReagents.emplace_back(item, missing);
            state.needs |= NEED_VENDOR;
            continue;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        if (!proto || proto->Class != ITEM_CLASS_TRADE_GOODS)
            return false;

        switch (proto->SubClass)
        {
            case ITEM_SUBCLASS_METAL_STONE:
                if (!bot->HasSkill(SKILL_MINING))
                    return false;
                state.needs |= NEED_ORE;
                break;
            case ITEM_SUBCLASS_LEATHER:
                if (!bot->HasSkill(SKILL_SKINNING))
                    return false;
                state.needs |= NEED_LEATHER;
                break;
            case ITEM_SUBCLASS_CLOTH:
                state.needs |= NEED_CLOTH;
                break;
            case ITEM_SUBCLASS_ENCHANTING:
                if (!bot->HasSkill(SKILL_ENCHANTING))
                    return false;
                break;  // comes from disenchanting unwearable gear
            case ITEM_SUBCLASS_JEWELCRAFTING:
                if (!isJewelcrafter || !bot->HasSkill(SKILL_MINING))
                    return false;
                state.needs |= NEED_ORE;  // gems come from prospecting ore
                break;
            default:
                return false;
        }
    }

    return true;
}

ItemUsage UsageAsCrafted(PlayerbotAI* botAI, uint32 itemId)
{
    t_AssumeCraftedItem = itemId;
    ItemUsageValue value(botAI);
    value.Qualify(std::to_string(itemId));
    ItemUsage const usage = value.Calculate();
    t_AssumeCraftedItem = 0;
    return usage;
}

void RefreshGoal(PlayerbotAI* botAI, Player* bot, CraftState& state)
{
    state.goalSpell = 0;
    state.goalItem = 0;
    state.goalIsGear = false;
    state.needs = 0;
    state.stationReady = false;
    state.vendorReagents.clear();
    state.missingToolCategories.clear();
    state.missingToolItems.clear();

    std::multimap<uint32, Recipe> const recipes = KnownRecipes(bot);

    // Gathering tools for the gathering professions it has.
    if (bot->HasSkill(SKILL_MINING))
        if (SpellInfo const* mining = sSpellMgr->GetSpellInfo(SPELL_MINING))
            HasTools(bot, mining, &state);
    if (bot->HasSkill(SKILL_SKINNING))
        if (SpellInfo const* skinning = sSpellMgr->GetSpellInfo(SPELL_SKINNING))
            HasTools(bot, skinning, &state);

    // Best upgrade it can craft.
    StatsWeightCalculator calculator(bot);
    calculator.SetItemSetBonus(false);
    calculator.SetOverflowPenalty(false);

    std::vector<std::pair<float, Recipe const*>> gear;
    std::vector<Recipe const*> skillUps;
    for (auto const& [itemId, recipe] : recipes)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        if ((proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
            proto->InventoryType != INVTYPE_NON_EQUIP && proto->RequiredLevel <= bot->GetLevel())
        {
            ItemUsage const usage = UsageAsCrafted(botAI, itemId);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                gear.emplace_back(calculator.CalculateItem(itemId), &recipe);
        }

        if (ItemUsageValue::SpellGivesSkillUp(recipe.spellId, bot))
            skillUps.push_back(&recipe);
    }

    std::sort(gear.begin(), gear.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

    std::vector<uint32> chain;
    auto tryGoal = [&](Recipe const& recipe, bool isGear) -> bool
    {
        CraftState trial;
        std::vector<uint32> trialChain;
        if (!ResolveNeeds(bot, recipes, recipe, 1, 0, trial, trialChain))
            return false;

        state.goalSpell = recipe.spellId;
        state.goalItem = recipe.itemId;
        state.goalIsGear = isGear;
        state.needs = trial.needs;
        state.vendorReagents = trial.vendorReagents;
        state.missingToolCategories.insert(trial.missingToolCategories.begin(), trial.missingToolCategories.end());
        state.missingToolItems.insert(trial.missingToolItems.begin(), trial.missingToolItems.end());
        chain = trialChain;
        chain.push_back(recipe.spellId);
        return true;
    };

    bool found = false;
    for (auto const& [score, recipe] : gear)
        if ((found = tryGoal(*recipe, true)))
            break;

    if (!found)
    {
        // Prefer a skill-up whose reagents are already in the bags, else the first one with a source.
        std::stable_sort(skillUps.begin(), skillUps.end(),
                         [&](Recipe const* a, Recipe const* b)
                         {
                             CraftState sa, sb;
                             std::vector<uint32> ca, cb;
                             bool const ra = ResolveNeeds(bot, recipes, *a, 1, 0, sa, ca) && !sa.needs;
                             bool const rb = ResolveNeeds(bot, recipes, *b, 1, 0, sb, cb) && !sb.needs;
                             return ra && !rb;
                         });
        for (Recipe const* recipe : skillUps)
            if ((found = tryGoal(*recipe, false)))
                break;
    }

    // A station craft is ready when a step that needs a forge or anvil has its reagents, or when there is a stack of
    // ore for a known smelting recipe.
    for (uint32 spellId : chain)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->RequiresSpellFocus)
            continue;

        bool hasAll = true;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (info->Reagent[i] > 0 && !bot->HasItemCount(info->Reagent[i], info->ReagentCount[i]))
                hasAll = false;

        if (hasAll)
            state.stationReady = true;
    }

    for (auto const& [itemId, recipe] : recipes)
    {
        if (!recipe.info->RequiresSpellFocus || recipe.info->Reagent[1] > 0 || recipe.info->Reagent[0] <= 0)
            continue;  // smelting: one reagent (the ore), at a forge

        ItemTemplate const* reagent = sObjectMgr->GetItemTemplate(recipe.info->Reagent[0]);
        if (reagent && reagent->Class == ITEM_CLASS_TRADE_GOODS && reagent->SubClass == ITEM_SUBCLASS_METAL_STONE &&
            bot->HasItemCount(reagent->ItemId, STATION_ORE_STACK))
            state.stationReady = true;
    }

    state.missingTool = !state.missingToolCategories.empty() || !state.missingToolItems.empty();

    std::ostringstream text;
    text << "Self Crafted:";
    for (uint32 skill : KnownProfessions(bot))
        if (SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(skill))
            text << " " << skillLine->name[0] << " " << bot->GetSkillValue(skill) << "/"
                 << bot->GetPureMaxSkillValue(skill) << ";";
    if (state.goalItem)
    {
        ItemTemplate const* goalProto = sObjectMgr->GetItemTemplate(state.goalItem);
        text << " goal " << (state.goalIsGear ? "upgrade " : "skill-up ") << (goalProto ? goalProto->Name1 : "?");
        if (state.needs & NEED_ORE)
            text << ", needs ore";
        if (state.needs & NEED_LEATHER)
            text << ", needs leather";
        if (state.needs & NEED_CLOTH)
            text << ", needs cloth";
        if (state.needs & NEED_VENDOR)
            text << ", needs vendor reagents";
    }
    else
        text << " no craft goal";
    if (state.missingTool)
        text << ", missing a tool";
    if (state.stationReady)
        text << ", forge or anvil craft ready";
    state.text = text.str();
}

CraftState& StateFor(ObjectGuid::LowType guid) { return g_Craft[guid]; }

// Buys `units` purchases (each BuyCount items) of `itemId` from `vendor`'s list, keeping a quarter of the gold.
uint32 BuyFromList(Player* bot, Creature* vendor, uint32 itemId, uint32 units)
{
    VendorItemData const* items = vendor->GetVendorItems();
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!items || !proto)
        return 0;

    // BuyItemFromVendorSlot reads the session's current vendor list when one is set (a multivendor menu).
    bot->GetSession()->SetCurrentVendor(0);

    uint32 bought = 0;
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* vendorItem = items->GetItem(slot);
        if (!vendorItem || vendorItem->item != itemId || vendorItem->ExtendedCost != 0)
            continue;

        for (uint32 i = 0; i < units; ++i)
        {
            uint32 const price = proto->BuyPrice;
            if (price && bot->GetMoney() < price * 4 / 3)
                break;

            uint32 const before = bot->GetItemCount(itemId, false);
            bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, itemId, 1, NULL_BAG, NULL_SLOT);
            if (bot->GetItemCount(itemId, false) <= before)
                break;
            ++bought;
        }
        break;
    }
    return bought;
}
}  // namespace

void SetSelfCrafted(ObjectGuid::LowType guid, std::vector<uint32> const& professionSkills)
{
    std::unique_lock lock(g_ListMutex);
    if (professionSkills.empty())
        g_SelfCrafted.erase(guid);
    else
        g_SelfCrafted[guid] = professionSkills;
}

std::vector<uint32> GetSelfCraftedProfessions(ObjectGuid::LowType guid)
{
    std::shared_lock lock(g_ListMutex);
    auto itr = g_SelfCrafted.find(guid);
    return itr == g_SelfCrafted.end() ? std::vector<uint32>() : itr->second;
}

bool IsSelfCrafted(Player* bot)
{
    if (!bot || !IsActive(bot))
        return false;

    std::shared_lock lock(g_ListMutex);
    return g_SelfCrafted.contains(bot->GetGUID().GetCounter());
}

void ClearSelfCraftedState(ObjectGuid::LowType guid)
{
    std::lock_guard lock(g_CraftMutex);
    g_Craft.erase(guid);
}

uint32 LearnProfessions(Player* bot, Creature* trainerNpc)
{
    if (!bot)
        return 0;

    EnsureIndex();

    Trainer::Trainer const* onlyTrainer = trainerNpc ? sObjectMgr->GetTrainer(trainerNpc->GetEntry()) : nullptr;
    if (trainerNpc && !onlyTrainer)
        return 0;

    std::vector<uint32> skills;
    if (IsSelfCrafted(bot))
        for (uint32 skill : GetSelfCraftedProfessions(bot->GetGUID().GetCounter()))
            if (IsPrimaryProfessionSkill(skill) && std::find(skills.begin(), skills.end(), skill) == skills.end())
                skills.push_back(skill);

    for (uint32 skill : KnownProfessions(bot))
        if (std::find(skills.begin(), skills.end(), skill) == skills.end())
            skills.push_back(skill);

    uint32 learned = 0;
    for (uint32 skill : skills)
    {
        // A listed profession the character does not have needs a free primary slot (CanTeachSpell checks it).
        auto const itr = g_TrainerSpellsBySkill.find(skill);
        if (itr == g_TrainerSpellsBySkill.end())
            continue;

        for (uint32 pass = 0; pass < 5; ++pass)
        {
            std::vector<ProfessionTrainerSpell const*> candidates;
            std::unordered_set<uint32> seen;
            for (ProfessionTrainerSpell const& entry : itr->second)
            {
                if (onlyTrainer && entry.trainer != onlyTrainer)
                    continue;
                if (seen.contains(entry.spell->SpellId) || bot->HasSpell(entry.learnedSpell))
                    continue;
                if (!entry.trainer->IsTrainerValidForPlayer(bot) || !entry.trainer->CanTeachSpell(bot, entry.spell))
                    continue;

                seen.insert(entry.spell->SpellId);
                candidates.push_back(&entry);
            }

            if (candidates.empty())
                break;

            std::sort(candidates.begin(), candidates.end(),
                      [](ProfessionTrainerSpell const* a, ProfessionTrainerSpell const* b)
                      {
                          if (a->spell->ReqSkillRank != b->spell->ReqSkillRank)
                              return a->spell->ReqSkillRank < b->spell->ReqSkillRank;
                          return a->spell->MoneyCost < b->spell->MoneyCost;
                      });

            uint32 learnedThisPass = 0;
            for (ProfessionTrainerSpell const* entry : candidates)
            {
                if (bot->HasSpell(entry->learnedSpell) || !entry->trainer->CanTeachSpell(bot, entry->spell))
                    continue;
                if (bot->GetMoney() < entry->spell->MoneyCost)
                    continue;

                // A profession the character does not know is only started when it is on its list.
                if (!bot->HasSkill(skill) && !IsSelfCrafted(bot))
                    continue;

                bot->ModifyMoney(-static_cast<int32>(entry->spell->MoneyCost));
                if (entry->spell->IsCastable())
                    bot->CastSpell(bot, entry->spell->SpellId, true);
                else
                    bot->learnSpell(entry->spell->SpellId, false);

                ++learned;
                ++learnedThisPass;
            }

            if (!learnedThisPass)
                break;
        }
    }

    if (learned)
    {
        LOG_DEBUG("playerbots", "TofuAutonomousAlt: {} learned {} profession spell(s)", bot->GetName(), learned);
        std::lock_guard lock(g_CraftMutex);
        StateFor(bot->GetGUID().GetCounter()).nextGoalRefresh = 0;
    }

    return learned;
}

uint32 BuyVendorRecipes(Player* bot, Creature* vendorNpc)
{
    if (!bot || !vendorNpc)
        return 0;

    VendorItemData const* items = vendorNpc->GetVendorItems();
    if (!items)
        return 0;

    uint32 learned = 0;
    for (VendorItem const* vendorItem : items->m_items)
    {
        if (!vendorItem || vendorItem->ExtendedCost != 0)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
        if (!proto || proto->Class != ITEM_CLASS_RECIPE || !proto->RequiredSkill ||
            !bot->HasSkill(proto->RequiredSkill))
            continue;

        if (bot->GetSkillValue(proto->RequiredSkill) < proto->RequiredSkillRank ||
            bot->GetLevel() < proto->RequiredLevel)
            continue;

        // A recipe item teaches through its second spell (the first is the generic "learning" cast).
        uint32 recipeSpell = 0;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0 && SkillOfSpell(spell.SpellId) == proto->RequiredSkill)
                recipeSpell = spell.SpellId;

        if (!recipeSpell || bot->HasSpell(recipeSpell))
            continue;

        uint32 const price = proto->BuyPrice;
        if (bot->GetMoney() < price * 4)
            continue;  // recipes only from spare gold

        bot->ModifyMoney(-static_cast<int32>(price));
        bot->learnSpell(recipeSpell, false);
        ++learned;
    }

    if (learned)
    {
        std::lock_guard lock(g_CraftMutex);
        StateFor(bot->GetGUID().GetCounter()).nextGoalRefresh = 0;
    }
    return learned;
}

uint32 BuyCraftSupplies(Player* bot, Creature* vendorNpc)
{
    if (!bot || !vendorNpc)
        return 0;

    VendorItemData const* items = vendorNpc->GetVendorItems();
    if (!items)
        return 0;

    std::unordered_set<uint32> toolCategories;
    std::unordered_set<uint32> toolItems;
    std::vector<std::pair<uint32, uint32>> reagents;
    {
        std::lock_guard lock(g_CraftMutex);
        CraftState const& state = StateFor(bot->GetGUID().GetCounter());
        toolCategories = state.missingToolCategories;
        toolItems = state.missingToolItems;
        reagents = state.vendorReagents;
    }

    uint32 bought = 0;
    for (VendorItem const* vendorItem : items->m_items)
    {
        if (!vendorItem || vendorItem->ExtendedCost != 0)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
        if (!proto)
            continue;

        bool const isTool = toolItems.contains(proto->ItemId) ||
                            (proto->TotemCategory && toolCategories.contains(proto->TotemCategory));
        if (isTool && !bot->HasItemCount(proto->ItemId, 1))
        {
            if (BuyFromList(bot, vendorNpc, proto->ItemId, 1))
            {
                ++bought;
                toolCategories.erase(proto->TotemCategory);
                toolItems.erase(proto->ItemId);
            }
            continue;
        }

        for (auto const& [item, count] : reagents)
        {
            if (item != proto->ItemId)
                continue;

            uint32 const have = bot->GetItemCount(item, false);
            if (have >= count)
                break;

            uint32 const perBuy = std::max<uint32>(proto->BuyCount, 1);
            bought += BuyFromList(bot, vendorNpc, item, (count - have + perBuy - 1) / perBuy);
            break;
        }
    }

    if (bought)
    {
        std::lock_guard lock(g_CraftMutex);
        StateFor(bot->GetGUID().GetCounter()).nextGoalRefresh = 0;
    }
    return bought;
}

bool UpdateCrafting(PlayerbotAI* botAI, bool atStation)
{
    if (!botAI)
        return false;

    Player* bot = botAI->GetBot();
    if (!IsSelfCrafted(bot) || !bot->IsAlive() || bot->IsInCombat() || bot->IsNonMeleeSpellCast(true) ||
        bot->IsInFlight() || bot->IsBeingTeleported())
        return false;

    EnsureIndex();

    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    uint32 const now = getMSTime();

    bool learn = false;
    {
        std::lock_guard lock(g_CraftMutex);
        CraftState& state = StateFor(guid);
        if (getMSTimeDiff(state.nextLearn, now) < LEARN_REFRESH_MS && state.nextLearn)
            learn = false;
        else
        {
            learn = true;
            state.nextLearn = now ? now : 1;
        }
    }
    if (learn)
        LearnProfessions(bot);

    uint32 goalSpell = 0;
    {
        std::lock_guard lock(g_CraftMutex);
        CraftState& state = StateFor(guid);
        if (!state.nextGoalRefresh || getMSTimeDiff(state.nextGoalRefresh, now) >= GOAL_REFRESH_MS)
        {
            RefreshGoal(botAI, bot, state);
            state.nextGoalRefresh = now ? now : 1;
        }
        goalSpell = state.goalSpell;
    }

    // Enchanters break down gear they cannot wear.
    if (bot->HasSkill(SKILL_ENCHANTING) && bot->HasSpell(SPELL_DISENCHANT))
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item && CanDisenchantForSelfCrafted(bot, item->GetTemplate()) &&
                IsUnwearableForSelfCrafted(bot, item->GetTemplate()))
                return botAI->CastSpell(SPELL_DISENCHANT, bot, item);
        }

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag* pBag = bot->GetBagByPos(bag);
            if (!pBag)
                continue;
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                Item* item = pBag->GetItemByPos(slot);
                if (item && CanDisenchantForSelfCrafted(bot, item->GetTemplate()) &&
                    IsUnwearableForSelfCrafted(bot, item->GetTemplate()))
                    return botAI->CastSpell(SPELL_DISENCHANT, bot, item);
            }
        }
    }

    if (!goalSpell)
        return false;

    std::multimap<uint32, Recipe> const recipes = KnownRecipes(bot);

    // Walk the goal's chain from the bottom: the first step that is needed and has everything is cast.
    std::function<bool(Recipe const&, uint32)> step = [&](Recipe const& recipe, uint32 depth) -> bool
    {
        if (depth > 2)
            return false;

        bool hasAll = true;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (recipe.info->Reagent[i] <= 0)
                continue;

            uint32 const item = static_cast<uint32>(recipe.info->Reagent[i]);
            if (bot->HasItemCount(item, recipe.info->ReagentCount[i]))
                continue;

            hasAll = false;
            auto const sub = recipes.find(item);
            if (sub != recipes.end() && step(sub->second, depth + 1))
                return true;

            // Gems: prospect a stack of ore.
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (proto && proto->Class == ITEM_CLASS_TRADE_GOODS && proto->SubClass == ITEM_SUBCLASS_JEWELCRAFTING &&
                bot->HasSpell(SPELL_PROSPECTING))
            {
                for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                {
                    Item* ore = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                    if (ore && ore->GetTemplate()->HasFlag(ITEM_FLAG_IS_PROSPECTABLE) &&
                        ore->GetCount() >= PROSPECT_STACK &&
                        bot->GetSkillValue(SKILL_JEWELCRAFTING) >= ore->GetTemplate()->RequiredSkillRank)
                        return botAI->CastSpell(SPELL_PROSPECTING, bot, ore);
                }
            }
        }

        if (!hasAll || !HasTools(bot, recipe.info, nullptr))
            return false;

        if (recipe.info->RequiresSpellFocus && (!atStation || !FocusInReach(bot, recipe.info->RequiresSpellFocus)))
            return false;

        return botAI->CastSpell(recipe.spellId, bot);
    };

    for (auto const& [itemId, recipe] : recipes)
    {
        if (recipe.spellId != goalSpell)
            continue;

        if (step(recipe, 0))
        {
            std::lock_guard lock(g_CraftMutex);
            StateFor(guid).nextGoalRefresh = 0;  // re-judge after each craft (an upgrade is equipped and replaced)
            return true;
        }
        break;
    }

    // At a station: smelt ore that still gives skill-ups or that the goal can use.
    if (atStation)
    {
        for (auto const& [itemId, recipe] : recipes)
        {
            if (!recipe.info->RequiresSpellFocus || !FocusInReach(bot, recipe.info->RequiresSpellFocus))
                continue;

            bool hasAll = true;
            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (recipe.info->Reagent[i] > 0 &&
                    !bot->HasItemCount(recipe.info->Reagent[i], recipe.info->ReagentCount[i]))
                    hasAll = false;

            if (!hasAll || !HasTools(bot, recipe.info, nullptr))
                continue;

            ItemTemplate const* reagent = sObjectMgr->GetItemTemplate(recipe.info->Reagent[0]);
            bool const smelting = reagent && reagent->Class == ITEM_CLASS_TRADE_GOODS &&
                                  reagent->SubClass == ITEM_SUBCLASS_METAL_STONE && recipe.info->Reagent[1] <= 0;
            if (smelting || ItemUsageValue::SpellGivesSkillUp(recipe.spellId, bot))
                return botAI->CastSpell(recipe.spellId, bot);
        }
    }

    return false;
}

uint32 TrainerProfessionSkill(Creature* creature)
{
    Trainer::Trainer const* trainer = creature ? sObjectMgr->GetTrainer(creature->GetEntry()) : nullptr;
    if (!trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill)
        return 0;

    EnsureIndex();
    for (auto const& [skill, spells] : g_TrainerSpellsBySkill)
        for (ProfessionTrainerSpell const& entry : spells)
            if (entry.trainer == trainer)
                return skill;
    return 0;
}

bool HasTrainableProfessionRank(Player* bot)
{
    if (!bot)
        return false;

    EnsureIndex();
    for (uint32 skill : KnownProfessions(bot))
    {
        if (bot->GetPureSkillValue(skill) < bot->GetPureMaxSkillValue(skill))
            continue;  // only when the skill is capped: the next rank is what it needs

        auto const itr = g_TrainerSpellsBySkill.find(skill);
        if (itr == g_TrainerSpellsBySkill.end())
            continue;

        for (ProfessionTrainerSpell const& entry : itr->second)
        {
            SpellInfo const* learned = sSpellMgr->GetSpellInfo(entry.learnedSpell);
            if (!learned || !learned->IsPrimaryProfession() || bot->HasSpell(entry.learnedSpell))
                continue;
            if (entry.trainer->CanTeachSpell(bot, entry.spell) && bot->GetMoney() >= entry.spell->MoneyCost)
                return true;
        }
    }
    return false;
}

bool HasPendingStationCraft(Player* bot)
{
    if (!IsSelfCrafted(bot))
        return false;

    std::lock_guard lock(g_CraftMutex);
    auto itr = g_Craft.find(bot->GetGUID().GetCounter());
    return itr != g_Craft.end() && itr->second.stationReady;
}

bool IsMissingTool(Player* bot)
{
    if (!IsSelfCrafted(bot))
        return false;

    std::lock_guard lock(g_CraftMutex);
    auto itr = g_Craft.find(bot->GetGUID().GetCounter());
    return itr != g_Craft.end() && itr->second.missingTool;
}

std::string DescribeSelfCrafted(Player* bot)
{
    if (!IsSelfCrafted(bot))
        return "";

    std::lock_guard lock(g_CraftMutex);
    auto itr = g_Craft.find(bot->GetGUID().GetCounter());
    return itr == g_Craft.end() || itr->second.text.empty() ? "Self Crafted: no craft goal yet" : itr->second.text;
}

float GrindPreferenceFactor(Player* bot, Unit* unit)
{
    Creature* creature = unit ? unit->ToCreature() : nullptr;
    if (!creature || !IsSelfCrafted(bot))
        return 1.0f;

    uint32 needs = 0;
    {
        std::lock_guard lock(g_CraftMutex);
        auto itr = g_Craft.find(bot->GetGUID().GetCounter());
        if (itr != g_Craft.end())
            needs = itr->second.needs;
    }

    CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();
    if (!creatureTemplate)
        return 1.0f;

    if ((needs & NEED_LEATHER) && creatureTemplate->SkinLootId && bot->HasSkill(SKILL_SKINNING))
        return 0.35f;

    if ((needs & NEED_CLOTH) && creatureTemplate->type == CREATURE_TYPE_HUMANOID)
        return 0.35f;

    return 1.0f;
}

int32 PreferredRpgStatus(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    if (!IsSelfCrafted(bot))
        return -1;

    uint32 needs = 0;
    {
        std::lock_guard lock(g_CraftMutex);
        auto itr = g_Craft.find(bot->GetGUID().GetCounter());
        if (itr != g_Craft.end())
            needs = itr->second.needs;
    }

    if (urand(1, 100) > 70)
        return -1;  // still quest and wander some of the time

    if ((needs & NEED_ORE) && bot->HasSkill(SKILL_MINING))
        return RPG_DO_GATHER;

    if (needs & (NEED_LEATHER | NEED_CLOTH))
        return RPG_GO_GRIND;

    return -1;
}

bool IsUnwearableForSelfCrafted(Player* bot, ItemTemplate const* proto)
{
    if (!proto || !IsSelfCrafted(bot))
        return false;

    if ((proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR) ||
        proto->InventoryType == INVTYPE_NON_EQUIP)
        return false;

    if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;  // class, level, or skill: not the challenge, so the stock rules decide

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    // Wearable when not crafted, then exempt (quest items, starting gear, tools, shirts, tabards).
    Item* plain = Item::CreateItem(proto->ItemId, 1, bot);
    if (!plain)
        return false;

    uint16 dest = 0;
    InventoryResult const plainResult = botAI->CanEquipItem(NULL_SLOT, dest, plain, true, true);
    plain->SetGuidValue(ITEM_FIELD_CREATOR, bot->GetGUID());
    InventoryResult const craftedResult = botAI->CanEquipItem(NULL_SLOT, dest, plain, true, true);
    plain->RemoveFromUpdateQueueOf(bot);
    delete plain;

    bool const plainOk = plainResult == EQUIP_ERR_OK || plainResult == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS;
    bool const craftedOk = craftedResult == EQUIP_ERR_OK || craftedResult == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS;
    if (plainOk || !craftedOk)
        return false;

    // A copy it crafted itself is wearable.
    return !AssumeCraftedByBot(bot, proto->ItemId);
}

bool CanDisenchantForSelfCrafted(Player* bot, ItemTemplate const* proto)
{
    return proto && bot && bot->HasSkill(SKILL_ENCHANTING) && proto->Quality >= ITEM_QUALITY_UNCOMMON &&
           proto->Quality <= ITEM_QUALITY_EPIC && proto->DisenchantID &&
           bot->GetSkillValue(SKILL_ENCHANTING) >= proto->RequiredDisenchantSkill;
}

bool AssumeCraftedByBot(Player* bot, uint32 itemId)
{
    if (!bot || !itemId)
        return false;

    if (t_AssumeCraftedItem == itemId)
        return true;

    if (!IsSelfCrafted(bot))
        return false;

    Item* item = bot->GetItemByEntry(itemId);
    return item && item->GetGuidValue(ITEM_FIELD_CREATOR) == bot->GetGUID();
}
}  // namespace TofuAutonomousAlt
