#include "bot_ai.h"
#include "botactivity.h"
#include "botchatter.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botdefine.h"
#include "botmgr.h"
#include "bottext.h"
#include "botworldevents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Containers.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
NpcBot World Events, see botworldevents.h
*/

namespace
{
    constexpr uint32 WORLD_EVENTS_UPDATE_INTERVAL = 2 * IN_MILLISECONDS;
    constexpr time_t OFFER_TIMEOUT = 60;
    constexpr time_t OFFER_DECLINED_COOLDOWN = 15 * MINUTE;
    constexpr time_t QUEST_HELP_RETRY_DELAY = 30;
    constexpr time_t FAREWELL_DELAY = 4;
    constexpr float QUEST_HELP_RANGE = 40.0f;
    constexpr uint8 QUEST_HELP_MAX_LEVEL_DIFF = 5;
    constexpr float ORGANIZER_RANGE = 250.0f;
    constexpr float GROUP_QUEST_COMPANION_RANGE = 250.0f;
    constexpr uint8 MIN_GROUP_FINDER_LEVEL = 10;
    constexpr uint8 MIN_WORLD_EVENT_LEVEL = 5;
    constexpr float EVENT_SEARCH_RANGE = 250.0f;
    constexpr float EVENT_MIN_TARGET_DISTANCE = 40.0f;
    constexpr float EVENT_RECRUIT_RANGE = 500.0f;
    constexpr float EVENT_RALLY_DISTANCE = 45.0f;
    constexpr float EVENT_ATTACK_RANGE = 40.0f;
    constexpr float EVENT_THANKS_RANGE = 80.0f;
    constexpr float CAMP_RADIUS = 25.0f;
    constexpr uint32 CAMP_MIN_CREATURES = 4;
    constexpr uint32 EVENT_MIN_BOTS = 2;
    constexpr time_t EVENT_GATHER_TIME = 90;
    constexpr time_t EVENT_FIGHT_TIME = 8 * MINUTE;
    constexpr time_t GROUP_QUEST_TIME = HOUR;
    constexpr time_t QUEST_HELP_TIME = 30 * MINUTE;
    constexpr time_t DUNGEON_TIME = 3 * HOUR;
    constexpr time_t RAID_TIME = 4 * HOUR;
    constexpr float WORLD_BOSS_VISIT_RANGE = 200.0f;
    constexpr float WORLD_BOSS_LEAVE_RANGE = 1500.0f;

    enum OfferType : uint8
    {
        OFFER_GROUP_QUEST = 0,
        OFFER_DUNGEON,
        OFFER_QUEST_HELP,
        OFFER_WORLD_BOSS,
        OFFER_RAID
    };

    // a group invite sent by a bot, waiting for the player's answer
    struct Offer
    {
        OfferType type;
        uint32 organizer;       // bot entry
        uint32 contentId;       // quest id, dungeon map id or world boss spawn id
        time_t expires;
    };

    // a bot hired through an offer, leaves when the task is done
    struct Contract
    {
        ObjectGuid player;
        OfferType type;
        uint32 contentId;
        time_t until;
        bool speaker;           // the organizer talks for the group
        bool visited;           // the player was in the dungeon or at the world boss
        time_t leaveAt;
    };

    // camp raid or rare hunt
    struct WorldEvent
    {
        uint32 id;
        bool hunt;
        uint32 mapId;
        Position target;
        ObjectGuid rare;
        std::string enemyName;
        std::vector<uint32> bots;   // the first one leads
        time_t attackAt;
        time_t endAt;
        bool attacking;
    };

    struct PlayerTimers
    {
        time_t nextGroupOffer = 0;
        time_t nextQuestHelp = 0;
        time_t nextEvent = 0;
        time_t nextRaidOffer = 0;
    };

    struct WorldBoss
    {
        uint32 entry;
        uint8 level;
    };

    constexpr std::array WorldBosses =
    {
        WorldBoss{ 6109, 63 },  // Azuregos
        WorldBoss{ 12397, 63 }, // Lord Kazzak
        WorldBoss{ 14887, 63 }, // Ysondre
        WorldBoss{ 14888, 63 }, // Lethon
        WorldBoss{ 14889, 63 }, // Emeriss
        WorldBoss{ 14890, 63 }, // Taerar
        WorldBoss{ 18728, 73 }, // Doom Lord Kazzak
        WorldBoss{ 17711, 73 }, // Doomwalker
    };

    std::unordered_map<ObjectGuid, Offer> Offers;
    std::unordered_map<uint32 /*bot entry*/, Contract> Contracts;
    std::vector<WorldEvent> Events;
    std::unordered_map<ObjectGuid, PlayerTimers> Timers;
    uint32 UpdateTimer = 0;
    uint32 NextEventId = 1;

    time_t Now()
    {
        return GameTime::GetGameTime().count();
    }

    // next time something is offered: around the configured interval
    time_t NextTime(uint32 interval)
    {
        return Now() + time_t(urand(interval / 2, interval + interval / 2));
    }

    Creature* GetBot(uint32 entry)
    {
        return const_cast<Creature*>(BotDataMgr::FindBot(entry));
    }

    bool IsReserved(uint32 entry)
    {
        if (Contracts.contains(entry))
            return true;
        for (auto const& [_, offer] : Offers)
            if (offer.organizer == entry)
                return true;
        for (WorldEvent const& event : Events)
            if (std::ranges::find(event.bots, entry) != event.bots.end())
                return true;
        return false;
    }

    // a free wandering bot of the player's faction doing nothing important, can be hired by the player
    bool IsAvailableWanderer(Creature const* bot, Player const* player)
    {
        bot_ai const* ai = bot->GetBotAI();
        if (!ai || !bot->IsInWorld() || !bot->IsAlive() || !ai->IsWanderer() || !ai->IAmFree() ||
            ai->IsDuringTeleport() || ai->GetBG() || bot->GetMap()->Instanceable() || ai->GetActivity().IsInEvent() ||
            ai->GetActivity().IsTellingStory())
            return false;

        if (!BotCfg::IsWanderingClassEnabled(ai->GetBotClass()) ||
            BotDataMgr::GetMinLevelForBotClass(ai->GetBotClass()) > player->GetLevel())
            return false;

        if (BotDataMgr::GetTeamIdForFaction(bot->GetFaction()) != player->GetTeamId())
            return false;

        return !IsReserved(bot->GetEntry());
    }

    // available wanderers, nearest first: same map within range, then (if range is 0) any map
    std::vector<Creature*> FindWanderers(Player const* player, float range, bool anyMap)
    {
        std::vector<std::pair<float, Creature*>> found;
        for (Creature const* cbot : BotDataMgr::GetExistingNPCBots())
        {
            if (!IsAvailableWanderer(cbot, player))
                continue;

            float dist;
            if (cbot->GetMap() == player->GetMap())
                dist = player->GetExactDist2d(cbot);
            else if (anyMap)
                dist = 100000.0f + float(cbot->GetMapId());
            else
                continue;

            if (!anyMap && dist > range)
                continue;

            found.emplace_back(dist, const_cast<Creature*>(cbot));
        }

        std::ranges::sort(found, {}, &std::pair<float, Creature*>::first);
        std::vector<Creature*> bots;
        bots.reserve(found.size());
        for (auto const& [_, bot] : found)
            bots.push_back(bot);
        return bots;
    }

    bool IsInCombatWithCreatures(Player const* player)
    {
        return player->IsInCombat() || !player->getAttackers().empty();
    }

    // texts and names in the language the bots talk in
    LocaleConstant DbcLocale()
    {
        return sWorld->GetAvailableDbcLocale(BotChatter::GetServerLocale());
    }

    std::string QuestTitle(Quest const* quest)
    {
        std::string title = quest->GetTitle();
        if (QuestLocale const* locale = sObjectMgr->GetQuestLocale(quest->GetQuestId()))
            ObjectMgr::GetLocaleString(locale->Title, BotChatter::GetServerLocale(), title);
        return title;
    }

    std::string CreatureName(uint32 entry)
    {
        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(entry);
        if (!proto)
            return "";

        std::string name = proto->Name;
        if (CreatureLocale const* locale = sObjectMgr->GetCreatureLocale(entry))
            ObjectMgr::GetLocaleString(locale->Name, BotChatter::GetServerLocale(), name);
        return name;
    }

    std::string ZoneName(uint32 zoneId)
    {
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
        return zone ? zone->area_name[DbcLocale()] : "";
    }

    std::string MapName(uint32 mapId)
    {
        // the dungeon finder names fit best ("Deadmines" instead of "The Deadmines" etc. per locale)
        for (LFGDungeonEntry const* dungeon : sLFGDungeonStore)
            if (dungeon && dungeon->MapID == mapId && *dungeon->Name[DbcLocale()])
                return dungeon->Name[DbcLocale()];

        MapEntry const* map = sMapStore.LookupEntry(mapId);
        return map ? map->name[DbcLocale()] : "";
    }

    void SendBotInvite(Player* player, Creature const* bot)
    {
        std::string const& name = bot->GetNameForLocaleIdx(player->GetSession()->GetSessionDbLocaleIndex());
        WorldPacket data(SMSG_GROUP_INVITE, 10 + name.size());
        data << uint8(1);   // invited
        data << name;
        data << uint32(0);
        data << uint8(0);   // count
        data << uint32(0);
        player->SendDirectMessage(&data);
    }

    // players may get offers while leading their own group (or being alone), not while a real invite is pending
    bool CanReceiveOffer(Player* player)
    {
        if (!player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive() || player->IsGameMaster() ||
            player->GetGroupInvite() || player->InBattleground() || player->InArena() ||
            Offers.contains(player->GetGUID()))
            return false;

        Group const* group = player->GetGroup();
        return !group || (group->IsLeader(player->GetGUID()) && !group->isLFGGroup() && !group->isBGGroup());
    }

    uint32 GetGroupSize(Player const* player)
    {
        Group const* group = player->GetGroup();
        return group ? group->GetMembersCount() : 1;
    }

    bool HasContractOfType(Player const* player, std::initializer_list<OfferType> types)
    {
        for (auto const& [_, contract] : Contracts)
            if (contract.player == player->GetGUID() && std::ranges::find(types, contract.type) != types.end())
                return true;
        return false;
    }

    void SendOffer(Player* player, Creature* organizer, OfferType type, uint32 contentId, uint32 textId,
        BotChatter::TextVars vars)
    {
        Offers[player->GetGUID()] = { type, organizer->GetEntry(), contentId, Now() + OFFER_TIMEOUT };

        bot_ai* ai = organizer->GetBotAI();
        ai->GetChatter().Announce(textId, std::move(vars), CHAT_MSG_WHISPER, player);
        SendBotInvite(player, organizer);
    }

    // a quest in the player's log needing a creature, nullptr if none
    Quest const* GetKillQuestFor(Player* player, Creature const* creature)
    {
        CreatureTemplate const* proto = creature->GetCreatureTemplate();
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = player->GetQuestSlotQuestId(slot);
            if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            auto itr = player->getQuestStatusMap().find(questId);
            if (itr == player->getQuestStatusMap().end())
                continue;

            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 required = quest->RequiredNpcOrGo[i];
                if (required <= 0 || itr->second.CreatureOrGOCount[i] >= quest->RequiredNpcOrGoCount[i])
                    continue;
                if (uint32(required) == creature->GetEntry() || uint32(required) == proto->KillCredit[0] ||
                    uint32(required) == proto->KillCredit[1])
                    return quest;
            }
        }
        return nullptr;
    }

    // an unfinished elite quest of the player's log, made for more than one player
    Quest const* GetGroupQuest(Player* player)
    {
        std::vector<Quest const*> quests;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = player->GetQuestSlotQuestId(slot);
            if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || quest->GetType() == QUEST_TYPE_DUNGEON || quest->GetType() == QUEST_TYPE_RAID ||
                quest->IsPVPQuest())
                continue;

            if (quest->GetType() == QUEST_TYPE_ELITE || quest->GetSuggestedPlayers() > 1)
                quests.push_back(quest);
        }
        return quests.empty() ? nullptr : Acore::Containers::SelectRandomContainerElement(quests);
    }

    // a normal dungeon for the player's level the bots may enter
    LFGDungeonEntry const* GetDungeonFor(Player const* player, uint8 type)
    {
        uint8 level = player->GetLevel();
        uint32 expansion = sWorld->getIntConfig(CONFIG_EXPANSION);

        std::vector<LFGDungeonEntry const*> dungeons;
        std::unordered_set<uint32> maps;
        for (LFGDungeonEntry const* dungeon : sLFGDungeonStore)
        {
            if (!dungeon || dungeon->TypeID != type || dungeon->Difficulty != 0 || dungeon->ExpansionLevel > expansion)
                continue;
            if (level < dungeon->MinLevel || level > dungeon->MaxLevel)
                continue;
            // the recommended levels, not everything the dungeon finder allows
            if (type == lfg::LFG_TYPE_DUNGEON && dungeon->TargetLevel &&
                (level + 3 < dungeon->TargetLevel || level > dungeon->TargetLevel + 6))
                continue;
            if (!BotCfg::IsMapIdAllowedForBots(dungeon->MapID) || !maps.insert(dungeon->MapID).second)
                continue;
            dungeons.push_back(dungeon);
        }
        return dungeons.empty() ? nullptr : Acore::Containers::SelectRandomContainerElement(dungeons);
    }

    // the spawn of a world boss, its creature if loaded
    struct WorldBossSpawn
    {
        uint32 entry = 0;
        ObjectGuid::LowType spawnId = 0;
        uint32 mapId = 0;
        Position pos;
    };

    std::vector<WorldBossSpawn> const& GetWorldBossSpawns()
    {
        static std::vector<WorldBossSpawn> spawns;
        static bool initialized = false;
        if (!initialized)
        {
            initialized = true;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                for (WorldBoss const& boss : WorldBosses)
                {
                    if (data.id == boss.entry)
                    {
                        WorldBossSpawn& spawn = spawns.emplace_back();
                        spawn.entry = boss.entry;
                        spawn.spawnId = spawnId;
                        spawn.mapId = data.mapid;
                        spawn.pos.Relocate(data.posX, data.posY, data.posZ);
                    }
                }
            }
        }
        return spawns;
    }

    Creature* FindWorldBoss(WorldBossSpawn const& spawn)
    {
        Map* map = sMapMgr->FindBaseMap(spawn.mapId);
        if (!map)
            return nullptr;

        auto bounds = map->GetCreatureBySpawnIdStore().equal_range(spawn.spawnId);
        return bounds.first != bounds.second ? bounds.first->second : nullptr;
    }

    WorldBossSpawn const* FindWorldBossSpawn(ObjectGuid::LowType spawnId)
    {
        for (WorldBossSpawn const& spawn : GetWorldBossSpawns())
            if (spawn.spawnId == spawnId)
                return &spawn;
        return nullptr;
    }

    // world bosses the player could take on, alive or not loaded yet
    std::vector<WorldBossSpawn const*> GetWorldBossesFor(Player const* player)
    {
        if (sWorld->getIntConfig(CONFIG_EXPANSION) < EXPANSION_THE_BURNING_CRUSADE && player->GetLevel() > 70)
            return {};

        std::vector<WorldBossSpawn const*> bosses;
        for (WorldBossSpawn const& spawn : GetWorldBossSpawns())
        {
            auto boss = std::ranges::find(WorldBosses, spawn.entry, &WorldBoss::entry);
            if (boss == WorldBosses.end() || player->GetLevel() + 5 < boss->level ||
                player->GetLevel() > boss->level + 10)
                continue;
            if (!BotCfg::IsMapIdAllowedForBots(spawn.mapId))
                continue;
            if (Creature const* creature = FindWorldBoss(spawn); creature && !creature->IsAlive())
                continue;
            bosses.push_back(&spawn);
        }
        return bosses;
    }

    // the bot fills a role in the group: switches spec and role if needed
    void AssignRole(Creature* bot, uint32 role)
    {
        bot_ai* ai = bot->GetBotAI();
        if (role == BOT_ROLE_DPS || ai->HasRole(role))
            return;

        if (!(BotDataMgr::GetViableRolesForClass(ai->GetBotClass()) & role))
            return;

        ai->SetSpec(BotDataMgr::SelectBotSpecForRoles(ai->GetBotClass(), role));
        ai->ToggleRole(role, true);
        if (role == BOT_ROLE_TANK && ai->HasRole(BOT_ROLE_HEAL))
            ai->ToggleRole(BOT_ROLE_HEAL, true);
        if (role == BOT_ROLE_HEAL && ai->HasRole(BOT_ROLE_TANK))
            ai->ToggleRole(BOT_ROLE_TANK, true);
    }

    bool CanFillRole(Creature const* bot, uint32 role)
    {
        bot_ai const* ai = bot->GetBotAI();
        return role == BOT_ROLE_DPS || ai->HasRole(role) ||
            (BotDataMgr::GetViableRolesForClass(ai->GetBotClass()) & role);
    }

    // tanks and healers present in the player's group (bots only, players count as damage dealers)
    std::pair<uint32, uint32> CountGroupRoles(Player const* player)
    {
        uint32 tanks = 0, healers = 0;
        if (Group const* group = player->GetGroup())
        {
            for (GroupBotReference const* itr = group->GetFirstBotMember(); itr != nullptr; itr = itr->next())
            {
                Creature const* member = itr->GetSource();
                if (!member || !member->GetBotAI())
                    continue;
                if (member->GetBotAI()->HasRole(BOT_ROLE_TANK))
                    ++tanks;
                else if (member->GetBotAI()->HasRole(BOT_ROLE_HEAL))
                    ++healers;
            }
        }
        return { tanks, healers };
    }

    bool HireForOffer(Player* player, Creature* bot, Offer const& offer, bool speaker, uint32 role)
    {
        bot_ai* ai = bot->GetBotAI();
        if (ai->GetActivity().GetMode() != BOT_ACTIVITY_ACTIVE)
            ai->GetActivity().Stop();

        BotMgr::HireInvitedBot(player, bot, false);
        if (ai->IAmFree() || ai->GetBotOwner() != player)
            return false;

        AssignRole(bot, role);

        time_t duration;
        switch (offer.type)
        {
            case OFFER_GROUP_QUEST: duration = GROUP_QUEST_TIME; break;
            case OFFER_QUEST_HELP:  duration = QUEST_HELP_TIME;  break;
            case OFFER_DUNGEON:     duration = DUNGEON_TIME;     break;
            default:                duration = RAID_TIME;        break;
        }

        Contracts[bot->GetEntry()] =
            { player->GetGUID(), offer.type, offer.contentId, Now() + duration, speaker, false, 0 };
        return true;
    }

    // further bots joining the organizer: roles first, the nearest bots preferred
    void HireCompanions(Player* player, Offer const& offer, uint32 count, uint32 tanks, uint32 healers, float range)
    {
        if (!count)
            return;

        std::vector<Creature*> candidates = FindWanderers(player, range, range <= 0.0f);
        std::vector<Creature*> picked;
        std::vector<uint32> roles;

        auto pick = [&](uint32 role, uint32 wanted) {
            for (auto itr = candidates.begin(); itr != candidates.end() && wanted && picked.size() < count;)
            {
                if (CanFillRole(*itr, role))
                {
                    picked.push_back(*itr);
                    roles.push_back(role);
                    itr = candidates.erase(itr);
                    --wanted;
                }
                else
                    ++itr;
            }
        };

        pick(BOT_ROLE_TANK, tanks);
        pick(BOT_ROLE_HEAL, healers);
        pick(BOT_ROLE_DPS, count);

        for (std::size_t i = 0; i < picked.size(); ++i)
            if (!HireForOffer(player, picked[i], offer, false, roles[i]))
                break; // bot limits reached
    }

    void AcceptOffer(Player* player, Offer const& offer)
    {
        Creature* organizer = GetBot(offer.organizer);
        if (!organizer || !organizer->IsInWorld() || !organizer->GetBotAI() || !organizer->GetBotAI()->IAmFree() ||
            !organizer->IsAlive())
        {
            if (organizer && organizer->IsInWorld() && organizer->GetBotAI())
                organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_EVENT_OFFER_GONE, {}, CHAT_MSG_WHISPER, player);
            else
                ChatHandler(player->GetSession()).SendSysMessage(
                    bot_ai::LocalizedNpcText(player, BOT_TEXT_BOTGIVER__BOT_BUSY));
            return;
        }

        uint32 groupSize = GetGroupSize(player);
        auto [tanks, healers] = CountGroupRoles(player);

        // the organizer takes the most needed role
        uint32 organizerRole = BOT_ROLE_DPS;
        if (offer.type == OFFER_DUNGEON || offer.type == OFFER_RAID || offer.type == OFFER_WORLD_BOSS)
        {
            if (!tanks && CanFillRole(organizer, BOT_ROLE_TANK))
                organizerRole = BOT_ROLE_TANK, ++tanks;
            else if (!healers && CanFillRole(organizer, BOT_ROLE_HEAL))
                organizerRole = BOT_ROLE_HEAL, ++healers;
        }

        if (!HireForOffer(player, organizer, offer, true, organizerRole))
            return;
        ++groupSize;

        switch (offer.type)
        {
            case OFFER_GROUP_QUEST:
            {
                Quest const* quest = sObjectMgr->GetQuestTemplate(offer.contentId);
                uint32 wanted = quest ? std::clamp<uint32>(quest->GetSuggestedPlayers(), 3, 5) : 3;
                HireCompanions(player, offer, wanted > groupSize ? std::min<uint32>(wanted - groupSize, 2) : 0, 0, 0,
                    GROUP_QUEST_COMPANION_RANGE);
                break;
            }
            case OFFER_DUNGEON:
            {
                uint32 missing = groupSize < MAXGROUPSIZE ? MAXGROUPSIZE - groupSize : 0;
                HireCompanions(player, offer, missing, tanks ? 0 : 1, healers ? 0 : 1, 0.0f);
                break;
            }
            case OFFER_WORLD_BOSS:
            case OFFER_RAID:
            {
                uint32 size = std::max<uint32>(BotCfg::GetBotRaidOffersSize(), MAXGROUPSIZE);
                uint32 missing = groupSize < size ? size - groupSize : 0;
                uint32 wantedTanks = 2, wantedHealers = std::max<uint32>(2, (size + 3) / 4);
                HireCompanions(player, offer, missing, wantedTanks > tanks ? wantedTanks - tanks : 0,
                    wantedHealers > healers ? wantedHealers - healers : 0, 0.0f);
                break;
            }
            default:
                break;
        }

        organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_EVENT_OFFER_ACCEPTED, {}, CHAT_MSG_PARTY, player);
        organizer->GetBotAI()->GetChatter().NoteRelation(player, BOT_RELATION_HIRED);
    }

    void DeclineOffer(Player* player, Offer const& offer)
    {
        if (Creature* organizer = GetBot(offer.organizer); organizer && organizer->IsInWorld() && organizer->GetBotAI())
            organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_EVENT_OFFER_DECLINED, {}, CHAT_MSG_WHISPER, player);

        PlayerTimers& timers = Timers[player->GetGUID()];
        time_t cooldown = Now() + OFFER_DECLINED_COOLDOWN;
        if (offer.type == OFFER_QUEST_HELP)
            timers.nextQuestHelp = std::max(timers.nextQuestHelp, cooldown);
        else if (offer.type == OFFER_WORLD_BOSS || offer.type == OFFER_RAID)
            timers.nextRaidOffer = std::max(timers.nextRaidOffer, cooldown);
        else
            timers.nextGroupOffer = std::max(timers.nextGroupOffer, cooldown);
    }

    // quest help: a bot nearby joins the fight against a creature the player needs for a quest
    void TryQuestHelp(Player* player, PlayerTimers& timers)
    {
        if (!BotCfg::IsBotQuestHelpEnabled() || timers.nextQuestHelp > Now() || !IsInCombatWithCreatures(player) ||
            player->GetMap()->Instanceable() || !CanReceiveOffer(player) || GetGroupSize(player) >= MAXGROUPSIZE)
            return;

        Unit* victim = player->GetVictim();
        Creature* creature = victim ? victim->ToCreature() : nullptr;
        if (!creature || !creature->IsAlive() || creature->IsNPCBot())
            return;

        Quest const* quest = GetKillQuestFor(player, creature);
        if (!quest)
            return;

        timers.nextQuestHelp = Now() + QUEST_HELP_RETRY_DELAY;
        if (!roll_chance_i(BotCfg::GetBotQuestHelpChance()))
            return;

        Creature* helper = nullptr;
        for (Creature* bot : FindWanderers(player, QUEST_HELP_RANGE, false))
        {
            if (bot->IsInCombat() || !bot->IsValidAttackTarget(creature) ||
                std::abs(int32(bot->GetLevel()) - int32(player->GetLevel())) > QUEST_HELP_MAX_LEVEL_DIFF)
                continue;
            helper = bot;
            break;
        }
        if (!helper)
            return;

        timers.nextQuestHelp = Now() + BotCfg::GetBotQuestHelpCooldown();

        bot_ai* ai = helper->GetBotAI();
        if (ai->GetActivity().GetMode() != BOT_ACTIVITY_ACTIVE)
            ai->GetActivity().Stop();

        BotChatter::TextVars vars{ { "%quest", QuestTitle(quest) }, { "%enemy", CreatureName(creature->GetEntry()) } };
        ai->GetChatter().Announce(BOT_TEXT_EVENT_QUEST_HELP_JOIN, vars, CHAT_MSG_MONSTER_SAY, player);
        helper->Attack(creature, !ai->HasRole(BOT_ROLE_RANGED));
        SendOffer(player, helper, OFFER_QUEST_HELP, quest->GetQuestId(), BOT_TEXT_EVENT_QUEST_HELP_OFFER,
            std::move(vars));
    }

    // group finder: an elite quest of the player's log, otherwise a dungeon
    void TryGroupFinder(Player* player, PlayerTimers& timers)
    {
        if (!BotCfg::IsBotGroupFinderEnabled() || timers.nextGroupOffer > Now())
            return;
        if (player->GetLevel() < MIN_GROUP_FINDER_LEVEL || IsInCombatWithCreatures(player) ||
            player->GetMap()->Instanceable() || !CanReceiveOffer(player) ||
            HasContractOfType(player, { OFFER_GROUP_QUEST, OFFER_DUNGEON, OFFER_RAID, OFFER_WORLD_BOSS }))
            return;

        timers.nextGroupOffer = NextTime(BotCfg::GetBotGroupFinderInterval());
        if (!roll_chance_i(BotCfg::GetBotGroupFinderChance()))
            return;

        std::vector<Creature*> nearby = FindWanderers(player, ORGANIZER_RANGE, false);
        if (nearby.empty())
            return;
        Creature* organizer = nearby.front();
        bot_ai* ai = organizer->GetBotAI();

        bool tryQuest = GetGroupSize(player) < MAXGROUPSIZE && roll_chance_i(60);
        if (Quest const* quest = tryQuest ? GetGroupQuest(player) : nullptr)
        {
            SendOffer(player, organizer, OFFER_GROUP_QUEST, quest->GetQuestId(), BOT_TEXT_EVENT_GROUP_QUEST_OFFER,
                { { "%quest", QuestTitle(quest) } });
            return;
        }

        if (GetGroupSize(player) > 2)
            return;

        LFGDungeonEntry const* dungeon = GetDungeonFor(player, lfg::LFG_TYPE_DUNGEON);
        if (!dungeon)
            return;

        BotChatter::TextVars vars{ { "%dungeon", MapName(dungeon->MapID) } };
        ai->GetChatter().Announce(BOT_TEXT_EVENT_DUNGEON_CALL, vars, CHAT_MSG_CHANNEL);
        SendOffer(player, organizer, OFFER_DUNGEON, dungeon->MapID, BOT_TEXT_EVENT_DUNGEON_OFFER, std::move(vars));
    }

    // raids: a world boss or a raid instance with a whole raid of bots
    void TryRaidOffer(Player* player, PlayerTimers& timers)
    {
        if (!BotCfg::IsBotRaidOffersEnabled() || timers.nextRaidOffer > Now())
            return;
        if (player->GetLevel() < 58 || IsInCombatWithCreatures(player) || player->GetMap()->Instanceable() ||
            !CanReceiveOffer(player) ||
            HasContractOfType(player, { OFFER_GROUP_QUEST, OFFER_DUNGEON, OFFER_RAID, OFFER_WORLD_BOSS }))
            return;

        timers.nextRaidOffer = NextTime(BotCfg::GetBotRaidOffersInterval());

        std::vector<Creature*> organizers = FindWanderers(player, ORGANIZER_RANGE, false);
        if (organizers.empty())
            organizers = FindWanderers(player, 0.0f, true);
        if (organizers.empty())
            return;
        Creature* organizer = organizers.front();
        bot_ai* ai = organizer->GetBotAI();

        std::vector<WorldBossSpawn const*> bosses = GetWorldBossesFor(player);
        LFGDungeonEntry const* raid = GetDungeonFor(player, lfg::LFG_TYPE_RAID);
        if (bosses.empty() && !raid)
            return;

        if (!bosses.empty() && (!raid || roll_chance_i(40)))
        {
            WorldBossSpawn const* boss = Acore::Containers::SelectRandomContainerElement(bosses);
            uint32 zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, boss->mapId, boss->pos);
            BotChatter::TextVars vars{ { "%boss", CreatureName(boss->entry) }, { "%place", ZoneName(zoneId) } };
            ai->GetChatter().Announce(BOT_TEXT_EVENT_BOSS_CALL, vars, CHAT_MSG_CHANNEL);
            SendOffer(player, organizer, OFFER_WORLD_BOSS, boss->spawnId, BOT_TEXT_EVENT_BOSS_OFFER, std::move(vars));
            return;
        }

        BotChatter::TextVars vars{ { "%raid", MapName(raid->MapID) } };
        ai->GetChatter().Announce(BOT_TEXT_EVENT_RAID_CALL, vars, CHAT_MSG_CHANNEL);
        SendOffer(player, organizer, OFFER_RAID, raid->MapID, BOT_TEXT_EVENT_RAID_OFFER, std::move(vars));
    }

    // hostile creatures around a position the leader could fight
    struct EventTargetCheck
    {
        EventTargetCheck(Player const* player, float range) : _player(player), _range(range) { }

        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsNPCBot() && !creature->IsCritter() && !creature->IsCivilian() &&
                !creature->IsSummon() && creature->GetSpawnId() && !creature->IsInCombat() &&
                !creature->isWorldBoss() && !creature->IsDungeonBoss() && creature->IsHostileTo(_player) &&
                _player->IsWithinDistInMap(creature, _range);
        }

    private:
        Player const* _player;
        float _range;
    };

    bool FitsEventLevel(Creature const* creature, Player const* player)
    {
        int32 diff = int32(creature->GetLevel()) - int32(player->GetLevel());
        return diff >= -6 && diff <= 3;
    }

    // a rare creature around the player, or the center of a camp of hostile creatures
    bool FindEventTarget(Player* player, WorldEvent& event)
    {
        std::list<Creature*> creatures;
        EventTargetCheck check(player, EVENT_SEARCH_RANGE);
        Acore::CreatureListSearcher<EventTargetCheck> searcher(player, creatures, check);
        Cell::VisitObjects(player, searcher, EVENT_SEARCH_RANGE);

        std::vector<Creature*> rares, normals;
        for (Creature* creature : creatures)
        {
            if (player->GetExactDist2d(creature) < EVENT_MIN_TARGET_DISTANCE)
                continue;

            uint32 rank = creature->GetCreatureTemplate()->rank;
            if (rank == CREATURE_ELITE_RARE || rank == CREATURE_ELITE_RAREELITE)
            {
                int32 diff = int32(creature->GetLevel()) - int32(player->GetLevel());
                if (diff >= -6 && diff <= 4)
                    rares.push_back(creature);
            }
            else if (rank == CREATURE_ELITE_NORMAL && FitsEventLevel(creature, player))
                normals.push_back(creature);
        }

        if (!rares.empty())
        {
            Creature* rare = Acore::Containers::SelectRandomContainerElement(rares);
            event.hunt = true;
            event.rare = rare->GetGUID();
            event.target.Relocate(rare);
            event.enemyName = CreatureName(rare->GetEntry());
            return true;
        }

        Creature* center = nullptr;
        uint32 bestCount = 0;
        for (Creature* creature : normals)
        {
            uint32 count = 0;
            for (Creature* other : normals)
                if (creature->GetExactDist2d(other) <= CAMP_RADIUS)
                    ++count;
            if (count > bestCount)
            {
                bestCount = count;
                center = creature;
            }
        }

        if (!center || bestCount < CAMP_MIN_CREATURES)
            return false;

        event.hunt = false;
        event.target.Relocate(center);
        event.enemyName = CreatureName(center->GetEntry());
        return true;
    }

    // events: bots gather near the player, then raid a camp or hunt a rare creature
    void TryWorldEvent(Player* player, PlayerTimers& timers)
    {
        if (!BotCfg::IsBotWorldEventsEnabled() || timers.nextEvent > Now())
            return;
        if (player->GetLevel() < MIN_WORLD_EVENT_LEVEL || IsInCombatWithCreatures(player) ||
            !player->GetMap()->GetEntry()->IsContinent() || !player->IsAlive() || player->IsGameMaster() ||
            player->IsInFlight())
            return;

        timers.nextEvent = NextTime(BotCfg::GetBotWorldEventsInterval());

        // one event around a player at a time
        for (WorldEvent const& other : Events)
            if (other.mapId == player->GetMapId() && player->GetExactDist2d(other.target) < EVENT_RECRUIT_RANGE)
                return;

        WorldEvent event;
        if (!FindEventTarget(player, event))
            return;

        std::vector<Creature*> bots;
        for (Creature* bot : FindWanderers(player, EVENT_RECRUIT_RANGE, false))
        {
            if (bot->IsInCombat())
                continue;
            if (std::abs(int32(bot->GetLevel()) - int32(player->GetLevel())) > 8)
                continue;
            bots.push_back(bot);
            if (bots.size() >= std::max<uint32>(BotCfg::GetBotWorldEventsMaxBots(), EVENT_MIN_BOTS))
                break;
        }
        if (bots.size() < EVENT_MIN_BOTS)
            return;

        // rally point between the player and the target, not too close to the enemies
        Position rally;
        float dist = player->GetExactDist2d(event.target);
        if (dist <= EVENT_RALLY_DISTANCE + 15.0f)
            rally.Relocate(player);
        else
        {
            float angle = event.target.GetAbsoluteAngle(player);
            float x = event.target.GetPositionX() + EVENT_RALLY_DISTANCE * std::cos(angle);
            float y = event.target.GetPositionY() + EVENT_RALLY_DISTANCE * std::sin(angle);
            float z = std::max(player->GetPositionZ(), event.target.GetPositionZ());
            player->UpdateAllowedPositionZ(x, y, z);
            rally.Relocate(x, y, z);
        }

        event.id = NextEventId++;
        event.mapId = player->GetMapId();
        event.attackAt = Now() + EVENT_GATHER_TIME;
        event.endAt = event.attackAt + EVENT_FIGHT_TIME;
        event.attacking = false;

        for (Creature* bot : bots)
            if (bot->GetBotAI()->GetActivity().JoinEvent(event.id, rally))
                event.bots.push_back(bot->GetEntry());
        if (event.bots.size() < EVENT_MIN_BOTS)
        {
            for (uint32 entry : event.bots)
                if (Creature* bot = GetBot(entry))
                    bot->GetBotAI()->GetActivity().Stop();
            return;
        }

        Creature* leader = GetBot(event.bots.front());
        BotChatter::TextVars vars{ { "%enemy", event.enemyName } };
        BotChatter& chatter = leader->GetBotAI()->GetChatter();
        chatter.Announce(event.hunt ? BOT_TEXT_EVENT_HUNT_CALL : BOT_TEXT_EVENT_CAMP_CALL, vars,
            CHAT_MSG_MONSTER_YELL, player);
        chatter.Announce(event.hunt ? BOT_TEXT_EVENT_HUNT_CHANNEL : BOT_TEXT_EVENT_CAMP_CHANNEL, vars,
            CHAT_MSG_CHANNEL, player);

        BOT_LOG_DEBUG("npcbots", "World event {}: {} bots {} {} near player {} map {} {}", event.id,
            uint32(event.bots.size()), event.hunt ? "hunt" : "raid camp of", event.enemyName, player->GetName(),
            event.mapId, event.target.ToString());

        Events.push_back(std::move(event));
    }

    // camp creatures or the rare still standing
    std::vector<Creature*> GetEventEnemies(WorldEvent const& event, Creature* leader)
    {
        std::vector<Creature*> enemies;
        if (event.hunt)
        {
            if (Creature* rare = leader->GetMap()->GetCreature(event.rare); rare && rare->IsAlive())
                enemies.push_back(rare);
            return enemies;
        }

        struct CampCreatureCheck
        {
            CampCreatureCheck(Creature const* leader, Position const& center) : _leader(leader), _center(center) { }

            bool operator()(Creature* creature) const
            {
                return creature->IsAlive() && !creature->IsNPCBot() && !creature->IsCritter() &&
                    !creature->IsCivilian() && creature->IsHostileTo(_leader) &&
                    creature->GetExactDist2d(_center) <= CAMP_RADIUS + 5.0f;
            }

        private:
            Creature const* _leader;
            Position const& _center;
        };

        std::list<Creature*> creatures;
        CampCreatureCheck check(leader, event.target);
        Acore::CreatureListSearcher<CampCreatureCheck> searcher(leader, creatures, check);
        Cell::VisitObjects(event.target.GetPositionX(), event.target.GetPositionY(), leader->GetMap(), searcher,
            CAMP_RADIUS + 5.0f);
        enemies.assign(creatures.begin(), creatures.end());
        return enemies;
    }

    void EndEvent(WorldEvent const& event, bool victory)
    {
        Creature* leader = nullptr;
        for (uint32 entry : event.bots)
        {
            Creature* bot = GetBot(entry);
            if (!bot || !bot->GetBotAI() || bot->GetBotAI()->GetActivity().GetEventId() != event.id)
                continue;
            if (!leader && bot->IsAlive())
                leader = bot;

            // players who fought along are remembered
            if (victory && bot->IsAlive())
            {
                Map::PlayerList const& players = bot->GetMap()->GetPlayers();
                for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
                    if (Player* player = itr->GetSource(); player && player->IsAlive() &&
                        player->GetExactDist2d(event.target) <= EVENT_THANKS_RANGE)
                        bot->GetBotAI()->GetChatter().NoteRelation(player, BOT_RELATION_FIGHT_TOGETHER);
            }

            bot->GetBotAI()->GetActivity().Stop();
        }

        if (leader && !leader->IsInCombat())
            leader->GetBotAI()->GetChatter().Announce(victory ? BOT_TEXT_EVENT_VICTORY : BOT_TEXT_EVENT_FAILED,
                { { "%enemy", event.enemyName } }, CHAT_MSG_MONSTER_SAY);

        BOT_LOG_DEBUG("npcbots", "World event {} ended, {}", event.id, victory ? "victory" : "failed");
    }

    // returns false when the event is over
    bool UpdateEvent(WorldEvent& event)
    {
        // bots hired, despawned or teleported away left the event
        std::erase_if(event.bots, [&event](uint32 entry) {
            Creature* bot = GetBot(entry);
            return !bot || !bot->IsInWorld() || bot->GetMapId() != event.mapId || !bot->GetBotAI() ||
                bot->GetBotAI()->GetActivity().GetEventId() != event.id;
        });

        Creature* leader = nullptr;
        for (uint32 entry : event.bots)
            if (Creature* bot = GetBot(entry); bot && bot->IsAlive())
                leader = leader ? leader : bot;

        if (!leader)
        {
            EndEvent(event, false);
            return false;
        }

        time_t now = Now();
        if (!event.attacking)
        {
            bool gathered = true;
            for (uint32 entry : event.bots)
                if (Creature* bot = GetBot(entry); bot && bot->IsAlive() && bot->isMoving())
                    gathered = false;

            if (now < event.attackAt && !(gathered && now + EVENT_GATHER_TIME - 30 >= event.attackAt))
                return true;

            event.attacking = true;
            event.endAt = now + EVENT_FIGHT_TIME;
            leader->GetBotAI()->GetChatter().Announce(BOT_TEXT_EVENT_ATTACK, { { "%enemy", event.enemyName } },
                CHAT_MSG_MONSTER_YELL);
            for (uint32 entry : event.bots)
            {
                Creature* bot = GetBot(entry);
                bot->GetBotAI()->GetActivity().StartEventAttack(event.target);
                if (bot != leader && roll_chance_i(30))
                    bot->GetBotAI()->GetChatter().Announce(BOT_TEXT_EVENT_JOIN, {}, CHAT_MSG_MONSTER_SAY);
            }
            return true;
        }

        std::vector<Creature*> enemies = GetEventEnemies(event, leader);
        if (enemies.empty())
        {
            EndEvent(event, true);
            return false;
        }
        if (now >= event.endAt)
        {
            EndEvent(event, false);
            return false;
        }

        // bots arriving at the target pick the nearest enemy
        for (uint32 entry : event.bots)
        {
            Creature* bot = GetBot(entry);
            if (!bot->IsAlive() || bot->GetVictim() || bot->GetExactDist2d(event.target) > EVENT_ATTACK_RANGE)
                continue;

            Creature* nearest = nullptr;
            for (Creature* enemy : enemies)
                if (bot->IsValidAttackTarget(enemy) &&
                    (!nearest || bot->GetExactDist2d(enemy) < bot->GetExactDist2d(nearest)))
                    nearest = enemy;
            if (nearest)
                bot->Attack(nearest, !bot->GetBotAI()->HasRole(BOT_ROLE_RANGED));
        }
        return true;
    }

    // returns false when the contract is over
    bool UpdateContract(uint32 entry, Contract& contract)
    {
        Creature* bot = GetBot(entry);
        Player* player = ObjectAccessor::FindConnectedPlayer(contract.player);
        if (!bot || !bot->GetBotAI() || !player || bot->GetBotAI()->IAmFree() ||
            bot->GetBotAI()->GetBotOwner() != player)
            return false;

        time_t now = Now();
        if (contract.leaveAt)
        {
            if (now < contract.leaveAt || player->IsBeingTeleported() || bot->GetBotAI()->IsDuringTeleport())
                return true;
            if (player->HaveBot() && player->GetBotMgr()->GetBot(bot->GetGUID()))
                player->GetBotMgr()->RemoveBot(bot->GetGUID(), BOT_REMOVE_DISMISS);
            return false;
        }

        if (player->IsBeingTeleported() || !player->IsInWorld())
            return true;

        bool done = false;
        switch (contract.type)
        {
            case OFFER_GROUP_QUEST:
            case OFFER_QUEST_HELP:
                done = player->GetQuestStatus(contract.contentId) != QUEST_STATUS_INCOMPLETE;
                break;
            case OFFER_DUNGEON:
            case OFFER_RAID:
                if (player->GetMapId() == contract.contentId)
                    contract.visited = true;
                else if (contract.visited && !player->GetMap()->Instanceable())
                    done = true;
                break;
            case OFFER_WORLD_BOSS:
            {
                WorldBossSpawn const* spawn = FindWorldBossSpawn(contract.contentId);
                if (!spawn)
                {
                    done = true;
                    break;
                }
                bool near = player->GetMapId() == spawn->mapId &&
                    player->GetExactDist2d(spawn->pos) <= WORLD_BOSS_VISIT_RANGE;
                contract.visited = contract.visited || near;
                if (contract.visited)
                {
                    Creature const* boss = FindWorldBoss(*spawn);
                    done = (boss && !boss->IsAlive()) || player->GetMapId() != spawn->mapId ||
                        player->GetExactDist2d(spawn->pos) > WORLD_BOSS_LEAVE_RANGE;
                }
                break;
            }
        }

        if (!done && now < contract.until)
            return true;

        // not in the middle of a fight
        if (!done && (player->IsInCombat() || bot->IsInCombat()))
            return true;

        if (contract.speaker)
        {
            bot_ai* ai = bot->GetBotAI();
            ai->GetChatter().Announce(done ? BOT_TEXT_EVENT_TASK_DONE : BOT_TEXT_EVENT_TASK_TIMEOUT, {},
                CHAT_MSG_PARTY, player);
            if (done)
                ai->GetChatter().NoteRelation(player, BOT_RELATION_FIGHT_TOGETHER);
        }
        if (bot->IsAlive() && !bot->IsInCombat())
            bot->HandleEmoteCommand(EMOTE_ONESHOT_WAVE);
        contract.leaveAt = now + FAREWELL_DELAY + (contract.speaker ? 0 : urand(0, 3));
        return true;
    }
}

void BotWorldEvents::Update(uint32 diff)
{
    if (!BotCfg::IsNpcBotModEnabled() || !BotDataMgr::AllBotsLoaded())
        return;

    if (UpdateTimer > diff)
    {
        UpdateTimer -= diff;
        return;
    }
    UpdateTimer = WORLD_EVENTS_UPDATE_INTERVAL;

    time_t now = Now();

    // unanswered offers
    for (auto itr = Offers.begin(); itr != Offers.end();)
    {
        if (itr->second.expires <= now)
            itr = Offers.erase(itr);
        else
            ++itr;
    }

    for (auto itr = Contracts.begin(); itr != Contracts.end();)
    {
        if (!UpdateContract(itr->first, itr->second))
            itr = Contracts.erase(itr);
        else
            ++itr;
    }

    std::erase_if(Events, [](WorldEvent& event) { return !UpdateEvent(event); });

    std::unordered_set<ObjectGuid> online;
    for (auto const& [_, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || player->IsBeingTeleported())
            continue;

        online.insert(player->GetGUID());
        auto [itr, inserted] = Timers.try_emplace(player->GetGUID());
        PlayerTimers& timers = itr->second;
        if (inserted)
        {
            // no offers right after logging in
            timers.nextGroupOffer = NextTime(BotCfg::GetBotGroupFinderInterval());
            timers.nextEvent = NextTime(BotCfg::GetBotWorldEventsInterval());
            timers.nextRaidOffer = NextTime(BotCfg::GetBotRaidOffersInterval());
        }

        if (!BotCfg::IsBotGenerationEnabledWorldMapId(player->GetMapId()) && !player->GetMap()->Instanceable())
            continue;

        TryQuestHelp(player, timers);
        TryGroupFinder(player, timers);
        TryRaidOffer(player, timers);
        TryWorldEvent(player, timers);
    }

    std::erase_if(Timers, [&online](auto const& entry) { return !online.contains(entry.first); });
}

bool BotWorldEvents::OnGroupInviteAnswer(Player* player, bool accept)
{
    auto itr = Offers.find(player->GetGUID());
    if (itr == Offers.end())
        return false;

    Offer offer = itr->second;
    Offers.erase(itr);

    if (accept)
        AcceptOffer(player, offer);
    else
        DeclineOffer(player, offer);
    return true;
}

bool BotWorldEvents::OnPlayerWhisper(Player* player, std::string const& botName, std::string_view message)
{
    auto itr = Offers.find(player->GetGUID());
    if (itr == Offers.end())
        return false;

    Creature const* organizer = BotDataMgr::FindBot(itr->second.organizer);
    if (!organizer)
        return false;

    std::wstring wname, wbotName;
    if (!Utf8toWStr(organizer->GetName(), wname) || !Utf8toWStr(botName, wbotName))
        return false;
    wstrToLower(wname);
    wstrToLower(wbotName);
    if (wname != wbotName)
        return false;

    bool yes = BotChatter::MatchesKeywordText(message, BOT_TEXT_EVENT_KEYWORDS_YES);
    bool no = !yes && BotChatter::MatchesKeywordText(message, BOT_TEXT_EVENT_KEYWORDS_NO);
    if (!yes && !no)
        return false;

    // a click in the invite window still open on the client does nothing afterwards
    return OnGroupInviteAnswer(player, yes);
}
