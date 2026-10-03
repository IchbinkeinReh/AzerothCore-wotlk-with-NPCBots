#include "bot_ai.h"
#include "botactivity.h"
#include "botchatter.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botdefine.h"
#include "botmgr.h"
#include "botnews.h"
#include "botvillains.h"
#include "botwanderful.h"
#include "bottext.h"
#include "botworldevents.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Channel.h"
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
#include "OutdoorPvP.h"
#include "OutdoorPvPMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <mutex>
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
    constexpr uint8 MIN_INVASION_LEVEL = 10;
    constexpr float INVASION_TOWN_RANGE = 60.0f;
    constexpr float INVASION_RECRUIT_RANGE = 400.0f;
    constexpr float INVASION_SPAWN_MIN_DIST = 55.0f;
    constexpr float INVASION_SPAWN_MAX_DIST = 75.0f;
    constexpr uint32 INVASION_INVADER_LIFETIME = 15 * MINUTE * IN_MILLISECONDS;
    constexpr time_t INVASION_FIRST_WAVE_DELAY = 30;
    constexpr time_t INVASION_WAVE_INTERVAL = 75;
    constexpr time_t INVASION_MAX_TIME = 12 * MINUTE;
    constexpr uint32 INVASION_WAVE_BASE = 3;
    constexpr uint32 INVASION_WAVE_MAX = 7;
    constexpr uint32 INVADER_MIN_SPAWNS = 3;
    constexpr time_t OBJECTIVE_MAX_TIME = 12 * MINUTE;
    constexpr time_t OBJECTIVE_HOLD_TIME = 60;
    constexpr float OBJECTIVE_RECRUIT_RANGE = 1500.0f;
    constexpr time_t TRADE_OFFER_TIME = 15 * MINUTE;
    constexpr uint32 CHATTER_CHANNEL_TRADE = 2; // ChatChannels.dbc: Trade - City
    constexpr uint32 TRADE_MIN_GOODS = 5;
    constexpr uint32 MAX_GATHERED_GOODS = 40;
    constexpr time_t FINISHED_EVENT_KEEP_TIME = 30 * MINUTE;
    constexpr float STORY_BATTLE_RECRUIT_RANGE = 300.0f;
    constexpr float STORY_CAMP_RANGE = 200.0f;
    constexpr float CONTEST_SPACING = 300.0f;
    constexpr float CONTEST_INN_RANGE = 30.0f;
    constexpr float CONTEST_RECRUIT_RANGE = 60.0f;
    constexpr float CONTEST_ORGANIZER_RANGE = 40.0f;
    constexpr float CONTEST_RACE_MIN_DISTANCE = 120.0f;
    constexpr float CONTEST_RACE_MAX_DISTANCE = 300.0f;
    constexpr float CONTEST_START_RANGE = 15.0f;
    constexpr float CONTEST_FINISH_RANGE = 6.0f;
    constexpr time_t CONTEST_SIGN_UP_TIME = 30;
    constexpr time_t CONTEST_RACE_TIME = 3 * MINUTE;
    constexpr time_t CONTEST_INN_TIME = 3 * MINUTE;
    constexpr time_t CONTEST_ROUND_TIME = 6;
    constexpr time_t CONTEST_ARM_ROUND_TIME = 3;
    constexpr uint32 CONTEST_ARM_ROUNDS = 3;
    constexpr uint32 CONTEST_MAX_ROUNDS = 8;
    constexpr uint32 CONTEST_DROP_OUT_CHANCE = 25;
    constexpr time_t CHEER_KEEP_TIME = 2 * MINUTE;
    constexpr uint32 VILLAIN_LIFETIME = 30 * MINUTE * IN_MILLISECONDS;
    constexpr time_t VILLAIN_BATTLE_TIME = 15 * MINUTE;

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

    enum WorldEventType : uint8
    {
        EVENT_CAMP = 0,         // raiding a camp of hostile creatures
        EVENT_HUNT,             // hunting a rare creature
        EVENT_INVASION,         // defending a town against waves of invaders
        EVENT_OBJECTIVE         // taking a world pvp capture point
    };

    struct WorldEvent
    {
        uint32 id = 0;
        WorldEventType type = EVENT_CAMP;
        uint32 mapId = 0;
        Position target;            // camp, rare, town or capture point
        ObjectGuid rare;
        std::string enemyName;      // enemy, invaders or capture point
        std::string townName;
        std::vector<uint32> bots;   // the first one leads
        time_t attackAt = 0;
        time_t endAt = 0;
        bool attacking = false;

        // invasion
        std::vector<uint32> invaderEntries;
        std::vector<ObjectGuid> invaders;
        uint32 wavesLeft = 0;
        time_t nextWaveAt = 0;

        // a story arc's battle (botstoryarcs.h): no announcements, no deeds
        bool story = false;
        // invaders following a villain (botvillains.h), or the villain's own last stand
        uint32 villainFollowers = 0;
        uint32 villainId = 0;

        // objective
        uint32 zoneId = 0;
        ObjectGuid::LowType capturePoint = 0;
        TeamId team = TEAM_NEUTRAL;
        time_t heldSince = 0;
    };

    struct PlayerTimers
    {
        time_t nextGroupOffer = 0;
        time_t nextQuestHelp = 0;
        time_t nextEvent = 0;
        time_t nextRaidOffer = 0;
        time_t nextInvasion = 0;
        time_t nextContest = 0;
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
    std::unordered_map<uint32 /*zoneId*/, time_t> NextObjective;
    // outcome of finished events: victory, end time
    std::unordered_map<uint32, std::pair<bool, time_t>> FinishedEvents;

    // what gathering bots found, sold in the Trade channel; written from map threads
    std::mutex GoodsLock;
    std::unordered_map<uint32 /*bot entry*/, std::unordered_map<uint32 /*item*/, uint32 /*count*/>> GatheredGoods;
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
        EventTargetCheck(Player const* player, WorldObject const* center, float range) :
            _player(player), _center(center), _range(range) { }

        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsNPCBot() && !creature->IsCritter() && !creature->IsCivilian() &&
                !creature->IsSummon() && creature->GetSpawnId() && !creature->IsInCombat() &&
                !creature->isWorldBoss() && !creature->IsDungeonBoss() && creature->IsHostileTo(_player) &&
                _center->IsWithinDistInMap(creature, _range);
        }

    private:
        Player const* _player;
        WorldObject const* _center;
        float _range;
    };

    bool FitsEventLevel(Creature const* creature, Player const* player)
    {
        int32 diff = int32(creature->GetLevel()) - int32(player->GetLevel());
        return diff >= -6 && diff <= 3;
    }

    bool IsEventNear(uint32 mapId, Position const& pos)
    {
        for (WorldEvent const& other : Events)
            if (other.mapId == mapId && other.target.GetExactDist2d(pos) < EVENT_RECRUIT_RANGE)
                return true;
        return false;
    }

    // a rare creature around a spot (the player), or the center of a camp of hostile creatures
    bool FindEventTarget(Player* player, WorldObject const* center, float range, bool allowRare, WorldEvent& event)
    {
        std::list<Creature*> creatures;
        EventTargetCheck check(player, center, range);
        Acore::CreatureListSearcher<EventTargetCheck> searcher(center, creatures, check);
        Cell::VisitObjects(center, searcher, range);

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

        if (allowRare && !rares.empty())
        {
            Creature* rare = Acore::Containers::SelectRandomContainerElement(rares);
            event.type = EVENT_HUNT;
            event.rare = rare->GetGUID();
            event.target.Relocate(rare);
            event.enemyName = CreatureName(rare->GetEntry());
            return true;
        }

        Creature* campCenter = nullptr;
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
                campCenter = creature;
            }
        }

        if (!campCenter || bestCount < CAMP_MIN_CREATURES)
            return false;

        event.type = EVENT_CAMP;
        event.target.Relocate(campCenter);
        event.enemyName = CreatureName(campCenter->GetEntry());
        event.invaderEntries = { campCenter->GetEntry() };
        return true;
    }

    // free wanderers of the player's faction around a spot join the event, returns false if too few did
    bool RecruitEventBots(WorldEvent& event, Player const* player, float range, int32 maxLevelDiff,
        uint32 maxBots, uint32 minBots, Position const& rally)
    {
        for (Creature* bot : FindWanderers(player, range, false))
        {
            if (bot->IsInCombat() || std::abs(int32(bot->GetLevel()) - int32(player->GetLevel())) > maxLevelDiff)
                continue;
            if (bot->GetBotAI()->GetActivity().JoinEvent(event.id, rally))
                event.bots.push_back(bot->GetEntry());
            if (event.bots.size() >= maxBots)
                break;
        }

        if (event.bots.size() >= minBots)
            return true;

        for (uint32 entry : event.bots)
            if (Creature* bot = GetBot(entry))
                bot->GetBotAI()->GetActivity().Stop();
        event.bots.clear();
        return false;
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
        if (IsEventNear(player->GetMapId(), player->GetPosition()))
            return;

        WorldEvent event;
        if (!FindEventTarget(player, player, EVENT_SEARCH_RANGE, true, event))
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

        uint32 maxBots = std::max<uint32>(BotCfg::GetBotWorldEventsMaxBots(), EVENT_MIN_BOTS);
        if (!RecruitEventBots(event, player, EVENT_RECRUIT_RANGE, 8, maxBots, EVENT_MIN_BOTS, rally))
            return;

        Creature* leader = GetBot(event.bots.front());
        BotChatter::TextVars vars{ { "%enemy", event.enemyName } };
        BotChatter& chatter = leader->GetBotAI()->GetChatter();
        bool hunt = event.type == EVENT_HUNT;
        chatter.Announce(hunt ? BOT_TEXT_EVENT_HUNT_CALL : BOT_TEXT_EVENT_CAMP_CALL, vars,
            CHAT_MSG_MONSTER_YELL, player);
        chatter.Announce(hunt ? BOT_TEXT_EVENT_HUNT_CHANNEL : BOT_TEXT_EVENT_CAMP_CHANNEL, vars,
            CHAT_MSG_CHANNEL, player);

        BOT_LOG_DEBUG("npcbots", "World event {}: {} bots {} {} near player {} map {} {}", event.id,
            uint32(event.bots.size()), hunt ? "hunt" : "raid camp of", event.enemyName, player->GetName(),
            event.mapId, event.target.ToString());

        Events.push_back(std::move(event));
    }

    // invaders: creatures hostile to everyone with a real spawn somewhere, sorted by theme
    struct InvaderTheme
    {
        uint32 nameTextId;
        std::vector<CreatureTemplate const*> creatures;
    };

    std::array<InvaderTheme, 3>& GetInvaderThemes()
    {
        static std::array<InvaderTheme, 3> themes{ { { BOT_TEXT_INVADERS_SCOURGE, {} },
            { BOT_TEXT_INVADERS_LEGION, {} }, { BOT_TEXT_INVADERS_RAIDERS, {} } } };
        static bool initialized = false;
        if (initialized)
            return themes;
        initialized = true;

        // common creatures only: spawned several times on continents and nowhere in an instance, which rules
        // out bosses, named quest creatures and dungeon inhabitants
        std::unordered_map<uint32, uint32> continentSpawns;
        std::unordered_set<uint32> instanceCreatures;
        for (auto const& [_, data] : sObjectMgr->GetAllCreatureData())
        {
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            if (!map)
                continue;
            if (map->IsContinent())
                ++continentSpawns[data.id];
            else
                instanceCreatures.insert(data.id);
        }

        for (auto const& [entry, proto] : *sObjectMgr->GetCreatureTemplates())
        {
            auto spawns = continentSpawns.find(entry);
            if (spawns == continentSpawns.end() || spawns->second < INVADER_MIN_SPAWNS ||
                instanceCreatures.contains(entry))
                continue;
            // no bosses of any kind
            if (proto.rank != CREATURE_ELITE_NORMAL || (proto.type_flags & CREATURE_TYPE_FLAG_BOSS_MOB) ||
                (proto.flags_extra & CREATURE_FLAG_EXTRA_DUNGEON_BOSS) ||
                std::ranges::find(WorldBosses, entry, &WorldBoss::entry) != WorldBosses.end())
                continue;
            if (proto.ScriptID || (!proto.AIName.empty() && proto.AIName != "SmartAI") || proto.VehicleId ||
                proto.Models.empty())
                continue;
            if (proto.unit_flags & (UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC))
                continue;
            if (proto.flags_extra & (CREATURE_FLAG_EXTRA_CIVILIAN | CREATURE_FLAG_EXTRA_TRIGGER))
                continue;
            if (proto.npcflag || proto.maxlevel > DEFAULT_MAX_LEVEL)
                continue;

            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(proto.faction);
            if (!faction || !faction->IsHostileToPlayers() || (faction->friendlyMask & FACTION_MASK_PLAYER))
                continue;

            switch (proto.type)
            {
                case CREATURE_TYPE_UNDEAD:   themes[0].creatures.push_back(&proto); break;
                case CREATURE_TYPE_DEMON:    themes[1].creatures.push_back(&proto); break;
                case CREATURE_TYPE_HUMANOID: themes[2].creatures.push_back(&proto); break;
                default:                                                            break;
            }
        }
        return themes;
    }

    // a town: a friendly innkeeper or flight master near the player, not in a capital
    Creature* FindTownAnchor(Player* player)
    {
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId());
            !zone || (zone->flags & AREA_FLAG_CAPITAL))
            return nullptr;

        struct TownNpcCheck
        {
            TownNpcCheck(Player const* player) : _player(player) { }

            bool operator()(Creature* creature) const
            {
                return creature->IsAlive() && !creature->IsNPCBot() && creature->IsFriendlyTo(_player) &&
                    (creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER) ||
                    creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER)) &&
                    _player->IsWithinDistInMap(creature, INVASION_TOWN_RANGE);
            }

        private:
            Player const* _player;
        };

        Creature* anchor = nullptr;
        TownNpcCheck check(player);
        Acore::CreatureSearcher<TownNpcCheck> searcher(player, anchor, check);
        Cell::VisitObjects(player, searcher, INVASION_TOWN_RANGE);
        return anchor;
    }

    // a spot on the ground around the town the invaders come from
    bool GetInvaderSpawnPos(Map* map, Position const& center, float baseAngle, Position& pos)
    {
        for (uint32 tries = 0; tries < 12; ++tries)
        {
            float angle = baseAngle + frand(-0.5f, 0.5f);
            float dist = frand(INVASION_SPAWN_MIN_DIST, INVASION_SPAWN_MAX_DIST);
            float x = center.GetPositionX() + dist * std::cos(angle);
            float y = center.GetPositionY() + dist * std::sin(angle);
            float z = map->GetHeight(PHASEMASK_NORMAL, x, y, center.GetPositionZ() + 30.0f, true, 60.0f);
            if (z <= INVALID_HEIGHT || std::abs(z - center.GetPositionZ()) > 25.0f ||
                map->IsInWater(PHASEMASK_NORMAL, x, y, z, 2.0f))
                continue;
            pos.Relocate(x, y, z + 0.5f, Position::NormalizeOrientation(angle + float(M_PI)));
            return true;
        }
        return false;
    }

    void SpawnInvaderWave(WorldEvent& event, Map* map, uint32 count)
    {
        float baseAngle = rand_norm() * 2.0f * float(M_PI);
        for (uint32 i = 0; i < count; ++i)
        {
            Position pos;
            if (!GetInvaderSpawnPos(map, event.target, baseAngle, pos))
                continue;

            uint32 entry = Acore::Containers::SelectRandomContainerElement(event.invaderEntries);
            TempSummon* invader = map->SummonCreature(entry, pos, nullptr, INVASION_INVADER_LIFETIME);
            if (!invader)
                continue;

            invader->SetHomePosition(event.target);
            invader->GetMotionMaster()->MovePoint(0, event.target);
            event.invaders.push_back(invader->GetGUID());
        }
    }

    // invasions: waves of the Scourge, the Legion or raiders attack a town near the player, bots defend it
    void TryInvasion(Player* player, PlayerTimers& timers)
    {
        if (!BotCfg::IsBotInvasionsEnabled() || timers.nextInvasion > Now())
            return;
        if (player->GetLevel() < MIN_INVASION_LEVEL || IsInCombatWithCreatures(player) ||
            !player->GetMap()->GetEntry()->IsContinent() || !player->IsAlive() || player->IsGameMaster() ||
            player->IsInFlight())
            return;

        timers.nextInvasion = NextTime(BotCfg::GetBotInvasionsInterval());

        Creature* anchor = FindTownAnchor(player);
        if (!anchor || IsEventNear(player->GetMapId(), anchor->GetPosition()))
            return;

        // invaders around the player's level
        uint8 level = player->GetLevel();
        std::vector<std::pair<uint8, std::vector<uint32>>> themes;
        for (uint8 index = 0; index < GetInvaderThemes().size(); ++index)
        {
            std::vector<uint32> entries = BotWorldEvents::GetInvaderEntries(index, level);
            if (entries.size() >= 2)
                themes.emplace_back(index, std::move(entries));
        }
        if (themes.empty())
            return;

        // a villain around sends its followers more often than not
        auto chosen = &themes[urand(0, uint32(themes.size()) - 1)];
        BotVillainRef villain;
        for (auto& theme : themes)
            if (BotVillains::GetVillainFor(theme.first, level, player->GetMapId(), villain) && roll_chance_i(70))
            {
                chosen = &theme;
                break;
            }
        if (villain.id && !BotVillains::GetVillainFor(chosen->first, level, player->GetMapId(), villain))
            villain = {};
        uint32 nameTextId = GetInvaderThemes()[chosen->first].nameTextId;
        std::vector<uint32>& entries = chosen->second;

        WorldEvent event;
        event.id = NextEventId++;
        event.type = EVENT_INVASION;
        event.mapId = player->GetMapId();
        event.target.Relocate(anchor);
        event.enemyName = villain.id ? villain.cult : BotChatter::GetServerText(nameTextId);
        event.villainFollowers = villain.id;
        event.invaderEntries = std::move(entries);
        event.wavesLeft = std::max<uint32>(BotCfg::GetBotInvasionsWaves(), 1);
        event.attackAt = Now() + INVASION_FIRST_WAVE_DELAY;
        event.nextWaveAt = event.attackAt;
        event.endAt = event.attackAt + INVASION_MAX_TIME;

        uint32 areaId = player->GetMap()->GetAreaId(PHASEMASK_NORMAL, anchor->GetPositionX(), anchor->GetPositionY(),
            anchor->GetPositionZ());
        event.townName = ZoneName(areaId);
        if (event.townName.empty())
            event.townName = ZoneName(player->GetZoneId());

        uint32 maxBots = std::max<uint32>(BotCfg::GetBotWorldEventsMaxBots(), EVENT_MIN_BOTS) + 1;
        if (!RecruitEventBots(event, player, INVASION_RECRUIT_RANGE, 10, maxBots, 1, event.target))
            return;

        Creature* leader = GetBot(event.bots.front());
        BotChatter::TextVars vars{ { "%enemy", event.enemyName }, { "%town", event.townName } };
        leader->GetBotAI()->GetChatter().Announce(BOT_TEXT_INVASION_CALL, vars, CHAT_MSG_MONSTER_YELL, player);
        leader->GetBotAI()->GetChatter().Announce(BOT_TEXT_INVASION_CHANNEL, vars, CHAT_MSG_CHANNEL, player);

        BOT_LOG_DEBUG("npcbots", "Invasion {}: {} ({} invader kinds) attack {} near player {}, {} defenders", event.id,
            event.enemyName, uint32(event.invaderEntries.size()), event.townName, player->GetName(),
            uint32(event.bots.size()));

        Events.push_back(std::move(event));
    }

    OPvPCapturePoint* GetCapturePoint(WorldEvent const& event)
    {
        OutdoorPvP* pvp = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(event.zoneId);
        return pvp ? pvp->GetCapturePoint(event.capturePoint) : nullptr;
    }

    // the capture point is fully held by the team
    bool IsHeldBy(OPvPCapturePoint const* point, TeamId team)
    {
        float max = point->GetMaxValue();
        return team == TEAM_ALLIANCE ? point->GetSlider() >= max * 0.99f : point->GetSlider() <= -max * 0.99f;
    }

    // free wanderers not busy with anything else, any faction
    bool IsFreeWanderer(Creature const* bot)
    {
        bot_ai const* ai = bot->GetBotAI();
        return ai && bot->IsInWorld() && bot->IsAlive() && ai->IsWanderer() && ai->IAmFree() &&
            !ai->IsDuringTeleport() && !ai->GetBG() && !ai->GetActivity().IsInEvent() &&
            !ai->GetActivity().IsTellingStory() && !bot->IsInCombat() && !IsReserved(bot->GetEntry());
    }

    // world pvp: bots of a faction head to a capture point the enemy holds and take it
    void TryObjective(uint32 zoneId, Map* map)
    {
        OutdoorPvP* pvp = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
        if (!pvp)
            return;

        for (WorldEvent const& other : Events)
            if (other.type == EVENT_OBJECTIVE && other.zoneId == zoneId)
                return;

        // free wanderers in the zone per faction
        std::array<std::vector<Creature*>, 2> bots;
        for (Creature const* cbot : BotDataMgr::GetExistingNPCBots())
        {
            if (cbot->GetMap() != map || !IsFreeWanderer(cbot) || cbot->GetZoneId() != zoneId)
                continue;
            TeamId team = BotDataMgr::GetTeamIdForFaction(cbot->GetFaction());
            if (team == TEAM_ALLIANCE || team == TEAM_HORDE)
                bots[team].push_back(const_cast<Creature*>(cbot));
        }

        std::vector<TeamId> teams;
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            if (bots[team].size() >= EVENT_MIN_BOTS)
                teams.push_back(team);
        if (teams.empty())
            return;
        TeamId team = Acore::Containers::SelectRandomContainerElement(teams);

        std::vector<OPvPCapturePoint*> points;
        for (auto const& [_, point] : pvp->GetCapturePoints())
            if (point && point->_capturePoint && point->_capturePoint->IsInWorld() &&
                point->_capturePoint->GetMap() == map && !IsHeldBy(point, team))
                points.push_back(point);
        if (points.empty())
            return;

        OPvPCapturePoint* point = Acore::Containers::SelectRandomContainerElement(points);
        GameObject const* go = point->_capturePoint;

        WorldEvent event;
        event.id = NextEventId++;
        event.type = EVENT_OBJECTIVE;
        event.mapId = map->GetId();
        event.zoneId = zoneId;
        event.capturePoint = point->m_capturePointSpawnId;
        event.team = team;
        event.target.Relocate(go);
        event.attackAt = Now();
        event.endAt = Now() + OBJECTIVE_MAX_TIME;
        event.attacking = true;

        std::string name = go->GetGOInfo()->name;
        if (GameObjectLocale const* locale = sObjectMgr->GetGameObjectLocale(go->GetEntry()))
            ObjectMgr::GetLocaleString(locale->Name, BotChatter::GetServerLocale(), name);
        event.enemyName = name;

        std::ranges::sort(bots[team], {}, [go](Creature const* bot) { return bot->GetExactDist2d(go); });
        uint32 maxBots = std::max<uint32>(BotCfg::GetBotWorldEventsMaxBots(), EVENT_MIN_BOTS);
        for (Creature* bot : bots[team])
        {
            if (bot->GetExactDist2d(go) > OBJECTIVE_RECRUIT_RANGE)
                break;
            if (bot->GetBotAI()->GetActivity().JoinEvent(event.id, event.target))
            {
                bot->GetBotAI()->GetActivity().StartEventAttack(event.target);
                event.bots.push_back(bot->GetEntry());
            }
            if (event.bots.size() >= maxBots)
                break;
        }
        if (event.bots.size() < EVENT_MIN_BOTS)
        {
            for (uint32 entry : event.bots)
                if (Creature* bot = GetBot(entry))
                    bot->GetBotAI()->GetActivity().Stop();
            return;
        }

        Creature* leader = GetBot(event.bots.front());
        BotChatter::TextVars vars{ { "%objective", event.enemyName } };
        leader->GetBotAI()->GetChatter().Announce(BOT_TEXT_OBJECTIVE_CALL, vars, CHAT_MSG_MONSTER_SAY);
        leader->GetBotAI()->GetChatter().Announce(BOT_TEXT_OBJECTIVE_CHANNEL, vars, CHAT_MSG_CHANNEL);

        BOT_LOG_DEBUG("npcbots", "Objective {}: {} {} bots head to {} in zone {}", event.id, uint32(event.bots.size()),
            team == TEAM_ALLIANCE ? "alliance" : "horde", event.enemyName, zoneId);

        Events.push_back(std::move(event));
    }

    // camp creatures, the rare or the invaders still standing
    std::vector<Creature*> GetEventEnemies(WorldEvent const& event, Creature* leader)
    {
        std::vector<Creature*> enemies;
        switch (event.type)
        {
            case EVENT_HUNT:
                if (Creature* rare = leader->GetMap()->GetCreature(event.rare); rare && rare->IsAlive())
                    enemies.push_back(rare);
                return enemies;
            case EVENT_INVASION:
                for (ObjectGuid guid : event.invaders)
                    if (Creature* invader = leader->GetMap()->GetCreature(guid); invader && invader->IsAlive())
                        enemies.push_back(invader);
                return enemies;
            case EVENT_OBJECTIVE:
                return enemies;
            default:
                break;
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

    // the players who fought along get the deed told around
    void RecordEventDeeds(WorldEvent const& event)
    {
        Map* map = sMapMgr->FindBaseMap(event.mapId);
        if (!map || event.story)
            return;

        BotDeedType type;
        std::string subject = event.enemyName;
        switch (event.type)
        {
            case EVENT_INVASION:  type = event.villainId ? BOT_DEED_VILLAIN : BOT_DEED_TOWN_DEFENDED; break;
            case EVENT_HUNT:      type = BOT_DEED_RARE_SLAIN;      break;
            case EVENT_OBJECTIVE: type = BOT_DEED_OBJECTIVE_TAKEN; break;
            default:              type = BOT_DEED_CAMP_RAIDED;     break;
        }

        uint32 zoneId = map->GetZoneId(PHASEMASK_NORMAL, event.target.GetPositionX(), event.target.GetPositionY(),
            event.target.GetPositionZ());
        uint32 areaId = map->GetAreaId(PHASEMASK_NORMAL, event.target.GetPositionX(), event.target.GetPositionY(),
            event.target.GetPositionZ());
        std::string place = event.type == EVENT_INVASION ? event.townName : ZoneName(areaId);
        if (place.empty())
            place = ZoneName(zoneId);

        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* player = itr->GetSource();
            if (!player || !player->IsAlive() || player->GetExactDist2d(event.target) > EVENT_THANKS_RANGE)
                continue;
            if (event.type == EVENT_OBJECTIVE && player->GetTeamId() != event.team)
                continue;
            BotNews::RecordDeed(player, type, subject, place, zoneId);
        }
    }

    void EndEvent(WorldEvent const& event, bool victory)
    {
        FinishedEvents[event.id] = { victory, Now() };
        if (victory)
            RecordEventDeeds(event);
        if (victory && event.villainFollowers)
            BotVillains::OnFollowersDefeated(event.villainFollowers);

        Creature* leader = nullptr;
        std::vector<Creature*> survivors;
        for (uint32 entry : event.bots)
        {
            Creature* bot = GetBot(entry);
            if (!bot || !bot->GetBotAI() || bot->GetBotAI()->GetActivity().GetEventId() != event.id)
                continue;
            if (bot->IsAlive())
            {
                leader = leader ? leader : bot;
                survivors.push_back(bot);
            }

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

        // fighting side by side makes friends
        if (victory && BotCfg::IsBotBondsEnabled())
            for (std::size_t i = 0; i < survivors.size(); ++i)
                for (std::size_t j = i + 1; j < survivors.size(); ++j)
                    BotChatter::NoteBondBetween(survivors[i], survivors[j], 2);

        // invaders left over withdraw
        if (event.type == EVENT_INVASION)
            if (Map* map = sMapMgr->FindBaseMap(event.mapId))
                for (ObjectGuid guid : event.invaders)
                    if (Creature* invader = map->GetCreature(guid); invader && invader->IsAlive())
                        invader->DespawnOrUnsummon();

        // the villain's fate is told by BotVillains
        if (leader && !leader->IsInCombat() && !event.villainId)
        {
            uint32 textId;
            ChatMsg msgType = CHAT_MSG_MONSTER_SAY;
            switch (event.story ? EVENT_CAMP : event.type)
            {
                case EVENT_INVASION:
                    textId = victory ? BOT_TEXT_INVASION_VICTORY : BOT_TEXT_INVASION_FAILED;
                    msgType = CHAT_MSG_MONSTER_YELL;
                    break;
                case EVENT_OBJECTIVE:
                    textId = victory ? BOT_TEXT_OBJECTIVE_TAKEN : BOT_TEXT_OBJECTIVE_FAILED;
                    msgType = victory ? CHAT_MSG_MONSTER_YELL : CHAT_MSG_MONSTER_SAY;
                    break;
                default:
                    textId = victory ? BOT_TEXT_EVENT_VICTORY : BOT_TEXT_EVENT_FAILED;
                    break;
            }
            leader->GetBotAI()->GetChatter().Announce(textId, { { "%enemy", event.enemyName },
                { "%town", event.townName }, { "%objective", event.enemyName } }, msgType);
        }

        BOT_LOG_DEBUG("npcbots", "World event {} ended, {}", event.id, victory ? "victory" : "failed");
    }

    // bots near the target pick the nearest enemy
    void AttackEventEnemies(WorldEvent const& event, std::vector<Creature*> const& enemies)
    {
        for (uint32 entry : event.bots)
        {
            Creature* bot = GetBot(entry);
            if (!bot->IsAlive() || bot->GetVictim() || bot->GetExactDist2d(event.target) > EVENT_ATTACK_RANGE)
                continue;

            Creature* nearest = nullptr;
            for (Creature* enemy : enemies)
                if (bot->IsValidAttackTarget(enemy) && bot->GetExactDist2d(enemy) <= EVENT_ATTACK_RANGE &&
                    (!nearest || bot->GetExactDist2d(enemy) < bot->GetExactDist2d(nearest)))
                    nearest = enemy;
            if (nearest)
                bot->Attack(nearest, !bot->GetBotAI()->HasRole(BOT_ROLE_RANGED));
        }
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

        time_t now = Now();

        // invasions go on without bots, players and guards may still win
        if (!leader && event.type != EVENT_INVASION)
        {
            EndEvent(event, false);
            return false;
        }

        switch (event.type)
        {
            case EVENT_INVASION:
            {
                Map* map = sMapMgr->FindBaseMap(event.mapId);
                if (!map)
                {
                    EndEvent(event, false);
                    return false;
                }

                if (event.wavesLeft && now >= event.nextWaveAt)
                {
                    bool firstWave = !event.attacking;
                    uint32 count = std::min<uint32>(INVASION_WAVE_BASE + uint32(event.bots.size()) / 2 +
                        (event.invaders.empty() ? 0 : 1), INVASION_WAVE_MAX);
                    SpawnInvaderWave(event, map, count);
                    --event.wavesLeft;
                    event.nextWaveAt = now + INVASION_WAVE_INTERVAL;
                    event.attacking = true;

                    for (uint32 entry : event.bots)
                        if (Creature* bot = GetBot(entry))
                            bot->GetBotAI()->GetActivity().StartEventAttack(event.target);
                    if (!firstWave && leader)
                        leader->GetBotAI()->GetChatter().Announce(BOT_TEXT_INVASION_WAVE, {}, CHAT_MSG_MONSTER_YELL);
                }

                if (!event.attacking)
                    return true;

                std::vector<Creature*> enemies;
                for (ObjectGuid guid : event.invaders)
                    if (Creature* invader = map->GetCreature(guid); invader && invader->IsAlive())
                        enemies.push_back(invader);

                // the villain's last stand ends with the villain
                if (event.villainId)
                {
                    Creature const* villain = map->GetCreature(event.rare);
                    if (!villain || !villain->IsAlive())
                    {
                        EndEvent(event, villain != nullptr);
                        return false;
                    }
                }
                else if (enemies.empty() && !event.wavesLeft)
                {
                    EndEvent(event, true);
                    return false;
                }
                if (now >= event.endAt)
                {
                    EndEvent(event, false);
                    return false;
                }

                // invaders keep pushing into the town after their fights
                for (Creature* invader : enemies)
                    if (!invader->IsInCombat() && !invader->isMoving() &&
                        invader->GetExactDist2d(event.target) > 10.0f)
                        invader->GetMotionMaster()->MovePoint(0, event.target);

                AttackEventEnemies(event, enemies);
                return true;
            }
            case EVENT_OBJECTIVE:
            {
                OPvPCapturePoint* point = GetCapturePoint(event);
                if (!point || now >= event.endAt)
                {
                    EndEvent(event, false);
                    return false;
                }

                if (IsHeldBy(point, event.team))
                {
                    if (!event.heldSince)
                        event.heldSince = now;
                    else if (now >= event.heldSince + OBJECTIVE_HOLD_TIME)
                    {
                        EndEvent(event, true);
                        return false;
                    }
                }
                else
                    event.heldSince = 0;
                return true;
            }
            default:
                break;
        }

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

        AttackEventEnemies(event, enemies);
        return true;
    }

    // trade channel: bots selling items they found and buying trade goods
    struct TradeOffer
    {
        uint32 bot;
        TeamId team;
        bool selling;
        uint32 itemId;
        uint32 count;
        uint32 price;       // per item
        time_t expires;
    };

    std::vector<TradeOffer> TradeOffers;
    std::array<time_t, 2> NextTradePost{};

    // goods bots buy, by level range
    struct WantedGoods
    {
        uint32 itemId;
        uint8 minLevel;
        uint8 maxLevel;
    };

    constexpr std::array WantedGoodsList =
    {
        WantedGoods{ 2589, 1, 15 },  WantedGoods{ 2592, 15, 30 },  WantedGoods{ 4306, 25, 40 },  // cloth
        WantedGoods{ 4338, 35, 50 }, WantedGoods{ 14047, 45, 60 }, WantedGoods{ 21877, 58, 70 },
        WantedGoods{ 33470, 68, 80 },
        WantedGoods{ 2447, 1, 15 },  WantedGoods{ 765, 1, 15 },    WantedGoods{ 785, 10, 25 },   // herbs
        WantedGoods{ 2450, 15, 30 }, WantedGoods{ 3820, 15, 30 },  WantedGoods{ 3355, 25, 40 },
        WantedGoods{ 3818, 30, 45 }, WantedGoods{ 8831, 40, 55 },  WantedGoods{ 8838, 40, 55 },
        WantedGoods{ 13464, 50, 60 }, WantedGoods{ 22785, 58, 70 }, WantedGoods{ 22786, 60, 70 },
        WantedGoods{ 36901, 68, 80 }, WantedGoods{ 36907, 70, 80 },
        WantedGoods{ 2770, 1, 15 },  WantedGoods{ 2771, 15, 30 },  WantedGoods{ 2772, 25, 45 },  // ore
        WantedGoods{ 3858, 40, 55 }, WantedGoods{ 10620, 50, 60 }, WantedGoods{ 23424, 58, 70 },
        WantedGoods{ 23425, 65, 70 }, WantedGoods{ 36909, 68, 80 }, WantedGoods{ 36912, 72, 80 },
        WantedGoods{ 2318, 1, 20 },  WantedGoods{ 2319, 15, 30 },  WantedGoods{ 4234, 25, 40 },  // leather
        WantedGoods{ 4304, 35, 50 }, WantedGoods{ 8170, 45, 60 },  WantedGoods{ 21887, 58, 70 },
        WantedGoods{ 33568, 68, 80 },
    };

    constexpr std::array OreGoods =
    {
        WantedGoods{ 2770, 1, 15 },  WantedGoods{ 2771, 15, 30 },  WantedGoods{ 2772, 25, 45 },
        WantedGoods{ 3858, 40, 55 }, WantedGoods{ 10620, 50, 60 }, WantedGoods{ 23424, 58, 70 },
        WantedGoods{ 23425, 65, 70 }, WantedGoods{ 36909, 68, 80 }, WantedGoods{ 36912, 72, 80 },
    };

    constexpr std::array HerbGoods =
    {
        WantedGoods{ 2447, 1, 15 },  WantedGoods{ 765, 1, 15 },    WantedGoods{ 785, 10, 25 },
        WantedGoods{ 2450, 15, 30 }, WantedGoods{ 3820, 15, 30 },  WantedGoods{ 3355, 25, 40 },
        WantedGoods{ 3818, 30, 45 }, WantedGoods{ 8831, 40, 55 },  WantedGoods{ 8838, 40, 55 },
        WantedGoods{ 13464, 50, 60 }, WantedGoods{ 22785, 58, 70 }, WantedGoods{ 22786, 60, 70 },
        WantedGoods{ 36901, 68, 80 }, WantedGoods{ 36907, 70, 80 },
    };

    constexpr std::array FishGoods =
    {
        WantedGoods{ 6291, 1, 15 },  WantedGoods{ 6289, 10, 25 },  WantedGoods{ 6308, 20, 35 },
        WantedGoods{ 8365, 30, 45 }, WantedGoods{ 13754, 40, 55 }, WantedGoods{ 13759, 45, 60 },
        WantedGoods{ 27422, 58, 70 }, WantedGoods{ 41809, 68, 80 },
    };

    // a stack of something a bot of the faction gathered, taken out of the bot's stock
    bool TakeGatheredGoods(TeamId team, Creature*& seller, uint32& itemId, uint32& count)
    {
        std::lock_guard<std::mutex> lock(GoodsLock);
        for (auto& [entry, goods] : GatheredGoods)
        {
            Creature const* bot = BotDataMgr::FindBot(entry);
            if (!bot || !bot->GetBotAI() || BotDataMgr::GetTeamIdForFaction(bot->GetFaction()) != team)
                continue;
            for (auto itr = goods.begin(); itr != goods.end(); ++itr)
            {
                if (itr->second < TRADE_MIN_GOODS)
                    continue;
                seller = const_cast<Creature*>(bot);
                itemId = itr->first;
                count = itr->second;
                goods.erase(itr);
                return true;
            }
        }
        return false;
    }

    // uncommon and rare equipment that drops in the world, by required level
    std::vector<uint32> const& GetSaleItems(uint8 level)
    {
        static std::array<std::vector<uint32>, DEFAULT_MAX_LEVEL + 1> items;
        static bool initialized = false;
        if (!initialized)
        {
            initialized = true;
            static constexpr std::array badNames{ "Test", "TEST", "Monster", "Deprecated", "DEPRECATED", "[PH]",
                "OLD", "Unused", "zz", "QA" };
            for (auto const& [id, proto] : *sObjectMgr->GetItemTemplateStore())
            {
                if ((proto.Quality != ITEM_QUALITY_UNCOMMON && proto.Quality != ITEM_QUALITY_RARE) ||
                    (proto.Class != ITEM_CLASS_WEAPON && proto.Class != ITEM_CLASS_ARMOR) ||
                    proto.InventoryType == INVTYPE_NON_EQUIP || !proto.SellPrice || !proto.RequiredLevel ||
                    proto.RequiredLevel > DEFAULT_MAX_LEVEL || proto.Bonding == BIND_WHEN_PICKED_UP ||
                    proto.Bonding == BIND_QUEST_ITEM || proto.MaxCount > 0 || proto.RequiredReputationFaction ||
                    proto.RequiredSkill)
                    continue;
                auto badName = [&proto](char const* bad) { return proto.Name1.find(bad) != std::string::npos; };
                if (std::ranges::any_of(badNames, badName))
                    continue;
                items[proto.RequiredLevel].push_back(id);
            }
        }
        return items[std::min<uint8>(level, DEFAULT_MAX_LEVEL)];
    }

    std::string ItemLink(uint32 itemId, uint32 count = 1)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            return "";

        std::string name = proto->Name1;
        if (ItemLocale const* locale = sObjectMgr->GetItemLocale(itemId))
            ObjectMgr::GetLocaleString(locale->Name, BotChatter::GetServerLocale(), name);

        std::string link = Acore::StringFormat("|c{:08x}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r",
            ItemQualityColors[proto->Quality], itemId, name);
        return count > 1 ? std::to_string(count) + "x " + link : link;
    }

    // coin icons, the same in every language
    std::string MoneyString(uint32 copper)
    {
        uint32 gold = copper / GOLD, silver = (copper % GOLD) / SILVER, rest = copper % SILVER;
        std::string text;
        auto add = [&text](uint32 amount, char const* icon) {
            if (!text.empty())
                text += ' ';
            text += std::to_string(amount) + "|TInterface\\MoneyFrame\\UI-" + icon + "Icon:0:0:2:0|t";
        };
        if (gold)
            add(gold, "Gold");
        if (silver)
            add(silver, "Silver");
        if (rest || text.empty())
            add(rest, "Copper");
        return text;
    }

    // whole silver above a gold, whole copper below
    uint32 RoundPrice(uint32 copper)
    {
        if (copper >= GOLD)
            return copper / SILVER * SILVER;
        return std::max<uint32>(copper, 1);
    }

    // players of a faction with the Trade channel joined (in a city)
    std::vector<Player*> GetTradeListeners(TeamId team)
    {
        std::vector<Player*> listeners;
        for (auto const& [_, session] : sWorldSessionMgr->GetAllSessions())
        {
            Player* player = session ? session->GetPlayer() : nullptr;
            if (!player || !player->IsInWorld() || player->GetTeamId() != team)
                continue;
            for (Channel const* channel : player->GetJoinedChannelsForBots())
                if (channel && channel->GetChannelId() == CHATTER_CHANNEL_TRADE)
                {
                    listeners.push_back(player);
                    break;
                }
        }
        return listeners;
    }

    void UpdateTrade()
    {
        time_t now = Now();
        std::erase_if(TradeOffers, [now](TradeOffer const& offer) { return offer.expires <= now; });

        if (!BotCfg::IsBotTradeEnabled())
            return;

        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            if (NextTradePost[team] > now)
                continue;
            NextTradePost[team] = NextTime(BotCfg::GetBotTradeInterval());

            std::vector<Player*> listeners = GetTradeListeners(team);
            if (listeners.empty())
                continue;

            // the offer fits one of the listeners
            Player* customer = Acore::Containers::SelectRandomContainerElement(listeners);
            std::vector<Creature*> sellers;
            for (Creature const* cbot : BotDataMgr::GetExistingNPCBots())
                if (IsFreeWanderer(cbot) && BotDataMgr::GetTeamIdForFaction(cbot->GetFaction()) == team)
                    sellers.push_back(const_cast<Creature*>(cbot));
            if (sellers.empty())
                continue;
            Creature* seller = Acore::Containers::SelectRandomContainerElement(sellers);

            TradeOffer offer{ seller->GetEntry(), team, roll_chance_i(65), 0, 1, 0, now + TRADE_OFFER_TIME };
            uint8 level = customer->GetLevel();
            if (offer.selling && roll_chance_i(50) && TakeGatheredGoods(team, seller, offer.itemId, offer.count))
            {
                // fish, ore and herbs the bot gathered itself
                offer.bot = seller->GetEntry();
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(offer.itemId);
                offer.count = std::min<uint32>(offer.count, std::max<uint32>(proto->GetMaxStackSize(), 1));
                offer.price = std::max<uint32>(proto->SellPrice * urand(2, 4), 1);
            }
            else if (offer.selling)
            {
                std::vector<uint32> items;
                for (uint8 l = level > 4 ? level - 4 : 1; l <= level; ++l)
                    std::ranges::copy(GetSaleItems(l), std::back_inserter(items));
                if (items.empty())
                    continue;
                offer.itemId = Acore::Containers::SelectRandomContainerElement(items);
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(offer.itemId);
                offer.price = RoundPrice(proto->SellPrice * urand(4, 8));
            }
            else
            {
                std::vector<WantedGoods> goods;
                for (WantedGoods const& wanted : WantedGoodsList)
                    if (level + 5 >= wanted.minLevel && level <= wanted.maxLevel + 10 &&
                        sObjectMgr->GetItemTemplate(wanted.itemId))
                        goods.push_back(wanted);
                if (goods.empty())
                    continue;
                offer.itemId = Acore::Containers::SelectRandomContainerElement(goods).itemId;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(offer.itemId);
                offer.count = std::min<uint32>(urand(1, 4) * 5, std::max<uint32>(proto->GetMaxStackSize(), 1));
                offer.price = std::max<uint32>(proto->SellPrice * urand(2, 4), 1);
            }

            // selling: the whole stack for a price, buying: a price for each
            BotChatter::TextVars vars{ { "%item", ItemLink(offer.itemId, offer.selling ? offer.count : 1) },
                { "%price", MoneyString(offer.selling ? offer.price * offer.count : offer.price) },
                { "%count", std::to_string(offer.count) } };
            if (!seller->GetBotAI()->GetChatter().AnnounceToChannel(CHATTER_CHANNEL_TRADE,
                offer.selling ? BOT_TEXT_TRADE_WTS : BOT_TEXT_TRADE_WTB, std::move(vars)))
                continue;

            // one offer per bot
            std::erase_if(TradeOffers, [&offer](TradeOffer const& other) { return other.bot == offer.bot; });
            TradeOffers.push_back(offer);
        }
    }

    // a whispered deal: buying the item or selling the goods the bot asked for, returns true if handled
    bool HandleTradeWhisper(Player* player, Creature* bot, std::string_view message)
    {
        auto itr = std::ranges::find(TradeOffers, bot->GetEntry(), &TradeOffer::bot);
        if (itr == TradeOffers.end() || itr->team != player->GetTeamId())
            return false;

        if (!BotChatter::MatchesKeywordText(message, BOT_TEXT_TRADE_KEYWORDS) &&
            !BotChatter::MatchesKeywordText(message, BOT_TEXT_EVENT_KEYWORDS_YES))
            return false;

        TradeOffer offer = *itr;
        BotChatter& chatter = bot->GetBotAI()->GetChatter();
        uint32 total = offer.price * offer.count;
        BotChatter::TextVars vars{ { "%item", ItemLink(offer.itemId, offer.selling ? offer.count : 1) },
            { "%count", std::to_string(offer.count) }, { "%price", MoneyString(total) } };

        if (offer.selling)
        {
            if (!player->HasEnoughMoney(total))
            {
                chatter.Announce(BOT_TEXT_TRADE_NO_MONEY, std::move(vars), CHAT_MSG_WHISPER, player);
                return true;
            }

            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, offer.itemId, offer.count) != EQUIP_ERR_OK)
            {
                chatter.Announce(BOT_TEXT_TRADE_BAGS_FULL, std::move(vars), CHAT_MSG_WHISPER, player);
                return true;
            }

            Item* item = player->StoreNewItem(dest, offer.itemId, true,
                Item::GenerateItemRandomPropertyId(offer.itemId));
            if (!item)
                return true;
            player->SendNewItem(item, offer.count, true, false);
            player->ModifyMoney(-int32(total));
            chatter.Announce(BOT_TEXT_TRADE_SOLD, std::move(vars), CHAT_MSG_WHISPER, player);
        }
        else
        {
            if (!player->HasItemCount(offer.itemId, offer.count))
            {
                chatter.Announce(BOT_TEXT_TRADE_NO_ITEMS, std::move(vars), CHAT_MSG_WHISPER, player);
                return true;
            }

            player->DestroyItemCount(offer.itemId, offer.count, true);
            player->ModifyMoney(int32(total));
            chatter.Announce(BOT_TEXT_TRADE_BOUGHT, std::move(vars), CHAT_MSG_WHISPER, player);
        }

        chatter.NoteRelation(player, BOT_RELATION_TALK);
        TradeOffers.erase(std::ranges::find(TradeOffers, offer.bot, &TradeOffer::bot));
        return true;
    }

    // races, drinking contests and arm wrestling, players take part and win small prizes
    enum ContestType : uint8
    {
        CONTEST_RACE = 0,
        CONTEST_DRINKING,
        CONTEST_ARM_WRESTLING
    };

    struct Contest
    {
        uint32 id = 0;              // also the event id of the bots taking part (BotActivity::JoinEvent)
        ContestType type = CONTEST_RACE;
        uint32 mapId = 0;
        Position start;
        Position finish;
        std::string place;
        uint32 organizer = 0;
        std::vector<uint32> bots;
        std::vector<ObjectGuid> players;
        std::vector<uint32> botsOut;
        std::vector<ObjectGuid> playersOut;
        time_t signUpUntil = 0;
        time_t endAt = 0;
        time_t nextRound = 0;
        uint32 round = 0;
        bool running = false;
    };

    struct Cheer
    {
        ObjectGuid player;
        uint32 mapId;
        float x;
        float y;
        time_t time;
    };

    std::vector<Contest> Contests;
    std::mutex CheersLock;
    std::vector<Cheer> Cheers;

    uint32 ContestNameTextId(ContestType type)
    {
        switch (type)
        {
            case CONTEST_RACE:     return BOT_TEXT_CONTEST_NAME_RACE;
            case CONTEST_DRINKING: return BOT_TEXT_CONTEST_NAME_DRINKING;
            default:               return BOT_TEXT_CONTEST_NAME_ARM;
        }
    }

    bool IsContestNear(uint32 mapId, Position const& pos)
    {
        for (Contest const& contest : Contests)
            if (contest.mapId == mapId && contest.start.GetExactDist2d(pos) < CONTEST_SPACING)
                return true;
        return false;
    }

    // a friendly innkeeper next to the player
    Creature* FindInnkeeper(Player* player)
    {
        struct InnkeeperCheck
        {
            InnkeeperCheck(Player const* player) : _player(player) { }

            bool operator()(Creature* creature) const
            {
                return creature->IsAlive() && !creature->IsNPCBot() && creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER) &&
                    creature->IsFriendlyTo(_player) && _player->IsWithinDistInMap(creature, CONTEST_INN_RANGE);
            }

        private:
            Player const* _player;
        };

        Creature* innkeeper = nullptr;
        InnkeeperCheck check(player);
        Acore::CreatureSearcher<InnkeeperCheck> searcher(player, innkeeper, check);
        Cell::VisitObjects(player, searcher, CONTEST_INN_RANGE);
        return innkeeper;
    }

    std::string AreaName(Map* map, Position const& pos)
    {
        uint32 areaId = map->GetAreaId(PHASEMASK_NORMAL, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());
        std::string name = ZoneName(areaId);
        if (name.empty())
            name = ZoneName(map->GetZoneId(PHASEMASK_NORMAL, pos.GetPositionX(), pos.GetPositionY(),
                pos.GetPositionZ()));
        return name;
    }

    void TryContest(Player* player, PlayerTimers& timers)
    {
        if (!BotCfg::IsBotContestsEnabled() || timers.nextContest > Now())
            return;
        if (player->GetLevel() < MIN_WORLD_EVENT_LEVEL || IsInCombatWithCreatures(player) ||
            !player->GetMap()->GetEntry()->IsContinent() || !player->IsAlive() || player->IsGameMaster() ||
            player->IsInFlight() || IsContestNear(player->GetMapId(), player->GetPosition()))
            return;

        timers.nextContest = NextTime(BotCfg::GetBotContestsInterval());

        std::vector<Creature*> bots = FindWanderers(player, CONTEST_RECRUIT_RANGE, false);
        std::erase_if(bots, [](Creature const* bot) { return bot->IsInCombat(); });
        if (bots.empty() || player->GetExactDist2d(bots.front()) > CONTEST_ORGANIZER_RANGE)
            return;

        Contest contest;
        contest.id = NextEventId++;
        contest.mapId = player->GetMapId();
        Map* map = player->GetMap();

        uint32 maxBots;
        if (Creature const* innkeeper = FindInnkeeper(player))
        {
            contest.type = roll_chance_i(60) ? CONTEST_DRINKING : CONTEST_ARM_WRESTLING;
            contest.start.Relocate(bots.front());
            contest.place = AreaName(map, innkeeper->GetPosition());
            maxBots = contest.type == CONTEST_DRINKING ? 3 : 2;
        }
        else
        {
            // the finish line: a waypoint of the wandering bots a bit away
            std::vector<WanderNode const*> finishes;
            WanderNode::DoForAllMapWPs(player->GetMapId(), [player, &finishes](WanderNode const* node) {
                float dist = player->GetExactDist2d(node);
                if (dist >= CONTEST_RACE_MIN_DISTANCE && dist <= CONTEST_RACE_MAX_DISTANCE &&
                    std::abs(node->GetPositionZ() - player->GetPositionZ()) < 40.0f)
                    finishes.push_back(node);
            });
            if (finishes.empty())
                return;

            contest.type = CONTEST_RACE;
            contest.start.Relocate(bots.front());
            contest.finish.Relocate(*Acore::Containers::SelectRandomContainerElement(finishes));
            contest.place = AreaName(map, contest.finish);
            maxBots = 4;
        }
        if (contest.place.empty())
            return;

        for (Creature* bot : bots)
        {
            if (bot->GetBotAI()->GetActivity().JoinEvent(contest.id, contest.start))
                contest.bots.push_back(bot->GetEntry());
            if (contest.bots.size() >= maxBots)
                break;
        }
        if (contest.bots.empty())
            return;

        contest.organizer = contest.bots.front();
        contest.signUpUntil = Now() + CONTEST_SIGN_UP_TIME;

        Creature* organizer = GetBot(contest.organizer);
        BotChatter::TextVars vars{ { "%place", contest.place } };
        switch (contest.type)
        {
            case CONTEST_RACE:
                organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_RACE_CALL, vars, CHAT_MSG_MONSTER_YELL,
                    player);
                break;
            case CONTEST_DRINKING:
                organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_DRINK_CALL, vars, CHAT_MSG_MONSTER_YELL,
                    player);
                break;
            default:
                organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_ARM_CALL, vars, CHAT_MSG_MONSTER_YELL,
                    player);
                break;
        }

        BOT_LOG_DEBUG("npcbots", "Contest {} type {} near {} at {}", contest.id, uint32(contest.type),
            player->GetName(), contest.place);
        Contests.push_back(std::move(contest));
    }

    void EndContest(Contest& contest, Creature* organizer, WorldObject const* winner)
    {
        Map* map = sMapMgr->FindBaseMap(contest.mapId);
        if (organizer && organizer->IsInWorld())
        {
            uint32 textId = !winner ? BOT_TEXT_CONTEST_NO_WINNER : contest.type == CONTEST_RACE ?
                BOT_TEXT_CONTEST_RACE_WINNER : contest.type == CONTEST_DRINKING ? BOT_TEXT_CONTEST_DRINK_WINNER :
                BOT_TEXT_CONTEST_ARM_WINNER;
            BotChatter::TextVars vars{ { "%winner", winner ? winner->GetName() : "" }, { "%place", contest.place } };
            BotChatter& chatter = organizer->GetBotAI()->GetChatter();
            // a race ends far from the start: the zone hears about it
            if (!(contest.type == CONTEST_RACE && winner && chatter.Announce(textId, vars, CHAT_MSG_CHANNEL)))
                chatter.Announce(textId, std::move(vars), CHAT_MSG_MONSTER_YELL);
        }

        if (Player* player = winner ? const_cast<Player*>(winner->ToPlayer()) : nullptr)
        {
            uint32 prize = std::max<uint32>(player->GetLevel() * player->GetLevel() * 5, 50);
            player->ModifyMoney(int32(prize));
            if (organizer && organizer->IsInWorld())
                organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_PRIZE,
                    { { "%price", MoneyString(prize) } }, CHAT_MSG_WHISPER, player);
            BotNews::RecordDeed(player, BOT_DEED_CONTEST, BotChatter::GetServerText(ContestNameTextId(contest.type)),
                contest.place, map ? map->GetZoneId(PHASEMASK_NORMAL, contest.start.GetPositionX(),
                contest.start.GetPositionY(), contest.start.GetPositionZ()) : 0);
        }

        for (uint32 entry : contest.bots)
            if (Creature* bot = GetBot(entry); bot && bot->GetBotAI() &&
                bot->GetBotAI()->GetActivity().GetEventId() == contest.id)
                bot->GetBotAI()->GetActivity().Stop();
    }

    // returns false when the contest is over
    bool UpdateContest(Contest& contest)
    {
        std::erase_if(contest.bots, [&contest](uint32 entry) {
            Creature* bot = GetBot(entry);
            return !bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetMapId() != contest.mapId ||
                !bot->GetBotAI() || bot->GetBotAI()->GetActivity().GetEventId() != contest.id;
        });
        std::erase_if(contest.players, [](ObjectGuid guid) {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            return !player || !player->IsInWorld() || !player->IsAlive();
        });

        Creature* organizer = GetBot(contest.organizer);
        if (organizer && (!organizer->IsInWorld() || organizer->GetMapId() != contest.mapId))
            organizer = nullptr;

        time_t now = Now();
        if (!contest.running)
        {
            if (!organizer)
            {
                EndContest(contest, nullptr, nullptr);
                return false;
            }
            if (now < contest.signUpUntil)
                return true;

            // players next to the start (race) or who cheered to sign up (inn contests)
            Map* map = organizer->GetMap();
            Map::PlayerList const& players = map->GetPlayers();
            {
                std::lock_guard<std::mutex> lock(CheersLock);
                for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
                {
                    Player* player = itr->GetSource();
                    if (!player || !player->IsAlive() || player->IsGameMaster() || organizer->IsHostileTo(player))
                        continue;
                    bool joined = contest.type == CONTEST_RACE ?
                        player->GetExactDist2d(contest.start) <= CONTEST_START_RANGE :
                        std::ranges::any_of(Cheers, [&contest, player](Cheer const& cheer) {
                            return cheer.player == player->GetGUID() && cheer.mapId == contest.mapId &&
                                contest.start.GetExactDist2d(cheer.x, cheer.y) <= CONTEST_START_RANGE &&
                                cheer.time + CONTEST_SIGN_UP_TIME + 5 >= contest.signUpUntil;
                        });
                    if (joined)
                        contest.players.push_back(player->GetGUID());
                }
            }

            // arm wrestling: the organizer against one challenger
            if (contest.type == CONTEST_ARM_WRESTLING)
            {
                if (!contest.players.empty())
                {
                    contest.players.resize(1);
                    contest.bots = { contest.organizer };
                }
                else if (contest.bots.size() > 2)
                    contest.bots.resize(2);
            }

            if (contest.bots.size() + contest.players.size() < 2)
            {
                EndContest(contest, organizer, nullptr);
                return false;
            }

            contest.running = true;
            contest.nextRound = now;
            contest.endAt = now + (contest.type == CONTEST_RACE ? CONTEST_RACE_TIME : CONTEST_INN_TIME);
            if (contest.type == CONTEST_RACE)
            {
                organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_RACE_GO, {}, CHAT_MSG_MONSTER_YELL);
                for (uint32 entry : contest.bots)
                    GetBot(entry)->GetBotAI()->GetActivity().StartEventAttack(contest.finish);
            }
            return true;
        }

        if (now >= contest.endAt)
        {
            EndContest(contest, organizer, nullptr);
            return false;
        }

        switch (contest.type)
        {
            case CONTEST_RACE:
            {
                // mounts are cheating, the first one at the finish line wins
                WorldObject const* winner = nullptr;
                float best = CONTEST_FINISH_RANGE;
                for (auto itr = contest.players.begin(); itr != contest.players.end();)
                {
                    Player* player = ObjectAccessor::FindConnectedPlayer(*itr);
                    if (player->IsMounted() || player->IsInFlight())
                    {
                        if (organizer)
                            organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_DISQUALIFIED, {},
                                CHAT_MSG_WHISPER, player);
                        itr = contest.players.erase(itr);
                        continue;
                    }
                    if (player->GetMapId() == contest.mapId && player->GetExactDist2d(contest.finish) < best)
                    {
                        best = player->GetExactDist2d(contest.finish);
                        winner = player;
                    }
                    ++itr;
                }
                for (uint32 entry : contest.bots)
                    if (Creature const* bot = GetBot(entry); bot->GetExactDist2d(contest.finish) < best)
                    {
                        best = bot->GetExactDist2d(contest.finish);
                        winner = bot;
                    }
                if (!winner)
                    return true;
                EndContest(contest, organizer, winner);
                return false;
            }
            case CONTEST_DRINKING:
            {
                if (now < contest.nextRound)
                    return true;
                contest.nextRound = now + CONTEST_ROUND_TIME;
                ++contest.round;

                if (organizer)
                    organizer->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_DRINK_ROUND, {},
                        CHAT_MSG_MONSTER_SAY);

                // everyone drinks, some give up
                std::vector<WorldObject*> left;
                for (uint32 entry : contest.bots)
                    if (std::ranges::find(contest.botsOut, entry) == contest.botsOut.end())
                    {
                        Creature* bot = GetBot(entry);
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_EAT_NO_SHEATHE);
                        left.push_back(bot);
                    }
                for (ObjectGuid guid : contest.players)
                    if (std::ranges::find(contest.playersOut, guid) == contest.playersOut.end())
                    {
                        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
                        player->SetDrunkValue(std::min<uint32>(player->GetDrunkValue() + 15, 100));
                        left.push_back(player);
                    }

                if (left.size() <= 1 || contest.round >= CONTEST_MAX_ROUNDS)
                {
                    EndContest(contest, organizer, left.empty() ? nullptr :
                        Acore::Containers::SelectRandomContainerElement(left));
                    return false;
                }

                Acore::Containers::RandomShuffle(left);
                for (std::size_t i = 1; i < left.size(); ++i)
                {
                    if (!roll_chance_i(CONTEST_DROP_OUT_CHANCE))
                        continue;
                    if (Creature* bot = left[i]->ToCreature())
                    {
                        contest.botsOut.push_back(bot->GetEntry());
                        bot->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_DRINK_OUT, {}, CHAT_MSG_MONSTER_SAY);
                        bot->SetStandState(UNIT_STAND_STATE_SIT);
                    }
                    else if (Player* player = left[i]->ToPlayer())
                    {
                        contest.playersOut.push_back(player->GetGUID());
                        ChatHandler(player->GetSession()).SendSysMessage(
                            BotChatter::GetServerText(BOT_TEXT_CONTEST_PLAYER_OUT));
                    }
                }
                return true;
            }
            default:
            {
                if (now < contest.nextRound)
                    return true;
                contest.nextRound = now + CONTEST_ARM_ROUND_TIME;
                ++contest.round;

                std::vector<WorldObject*> rivals;
                for (uint32 entry : contest.bots)
                    rivals.push_back(GetBot(entry));
                for (ObjectGuid guid : contest.players)
                    rivals.push_back(ObjectAccessor::FindConnectedPlayer(guid));
                if (rivals.size() < 2)
                {
                    EndContest(contest, organizer, rivals.empty() ? nullptr : rivals.front());
                    return false;
                }

                if (contest.round <= CONTEST_ARM_ROUNDS)
                {
                    for (WorldObject* rival : rivals)
                        if (Creature* bot = rival->ToCreature())
                        {
                            bot->HandleEmoteCommand(contest.round % 2 ? EMOTE_ONESHOT_ATTACK1H : EMOTE_ONESHOT_ROAR);
                            if (roll_chance_i(40))
                                bot->GetBotAI()->GetChatter().Announce(BOT_TEXT_CONTEST_ARM_ROUND, {},
                                    CHAT_MSG_MONSTER_SAY);
                        }
                    return true;
                }

                EndContest(contest, organizer, Acore::Containers::SelectRandomContainerElement(rivals));
                return false;
            }
        }
    }

    // dungeons and raids cleared, world bosses defeated
    void RecordContractDeed(Player const* player, Contract const& contract)
    {
        if (contract.type == OFFER_DUNGEON || contract.type == OFFER_RAID)
            BotNews::RecordDeed(player, BOT_DEED_DUNGEON, MapName(contract.contentId), "", 0);
        else if (contract.type == OFFER_WORLD_BOSS)
        {
            WorldBossSpawn const* spawn = FindWorldBossSpawn(contract.contentId);
            Creature const* boss = spawn ? FindWorldBoss(*spawn) : nullptr;
            if (!boss || boss->IsAlive())
                return;
            uint32 zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, spawn->mapId, spawn->pos);
            BotNews::RecordDeed(player, BOT_DEED_WORLD_BOSS, CreatureName(spawn->entry), ZoneName(zoneId), zoneId);
        }
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
            {
                ai->GetChatter().NoteRelation(player, BOT_RELATION_FIGHT_TOGETHER);
                RecordContractDeed(player, contract);
            }
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
    std::erase_if(FinishedEvents, [now](auto const& entry) {
        return entry.second.second + FINISHED_EVENT_KEEP_TIME <= now;
    });
    UpdateTrade();
    std::erase_if(Contests, [](Contest& contest) { return !UpdateContest(contest); });
    {
        std::lock_guard<std::mutex> lock(CheersLock);
        std::erase_if(Cheers, [now](Cheer const& cheer) { return cheer.time + CHEER_KEEP_TIME <= now; });
    }

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
            timers.nextInvasion = NextTime(BotCfg::GetBotInvasionsInterval());
            timers.nextContest = NextTime(BotCfg::GetBotContestsInterval());
        }

        // world pvp zones with players get their objectives fought over
        if (BotCfg::IsBotObjectivesEnabled() && !player->GetMap()->Instanceable() &&
            sOutdoorPvPMgr->GetOutdoorPvPToZoneId(player->GetZoneId()))
        {
            time_t& next = NextObjective[player->GetZoneId()];
            if (!next)
                next = Now() + urand(60, 300);
            else if (next <= Now())
            {
                next = NextTime(BotCfg::GetBotObjectivesInterval());
                TryObjective(player->GetZoneId(), player->GetMap());
            }
        }

        if (!BotCfg::IsBotGenerationEnabledWorldMapId(player->GetMapId()) && !player->GetMap()->Instanceable())
            continue;

        TryQuestHelp(player, timers);
        TryGroupFinder(player, timers);
        TryRaidOffer(player, timers);
        TryWorldEvent(player, timers);
        TryInvasion(player, timers);
        TryContest(player, timers);
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
    if (Creature const* bot = BotDataMgr::FindBotByNameFor(botName, player); bot && bot->GetBotAI() &&
        HandleTradeWhisper(player, const_cast<Creature*>(bot), message))
        return true;

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

float BotWorldEvents::GetCapturePointBotBalance(GameObject const* capturePoint, float radius)
{
    if (!BotCfg::IsNpcBotModEnabled() || !BotCfg::IsBotObjectivesEnabled())
        return 0.0f;

    struct FreeBotCheck
    {
        FreeBotCheck(GameObject const* point, float radius) : _point(point), _radius(radius) { }

        bool operator()(Creature* creature) const
        {
            bot_ai const* ai = creature->GetBotAI();
            return ai && creature->IsAlive() && ai->IsWanderer() && ai->IAmFree() &&
                _point->IsWithinDistInMap(creature, _radius);
        }

    private:
        GameObject const* _point;
        float _radius;
    };

    std::list<Creature*> bots;
    FreeBotCheck check(capturePoint, radius);
    Acore::CreatureListSearcher<FreeBotCheck> searcher(capturePoint, bots, check);
    Cell::VisitObjects(capturePoint, searcher, radius);

    float balance = 0.0f;
    for (Creature const* bot : bots)
    {
        TeamId team = BotDataMgr::GetTeamIdForFaction(bot->GetFaction());
        if (team == TEAM_ALLIANCE)
            balance += 1.0f;
        else if (team == TEAM_HORDE)
            balance -= 1.0f;
    }
    return balance;
}

void BotWorldEvents::AddGatheredGoods(Creature const* bot, uint8 gatherKind)
{
    auto pick = [level = bot->GetLevel()](auto const& goods) -> uint32 {
        std::vector<uint32> fitting;
        for (WantedGoods const& wanted : goods)
            if (level + 3 >= wanted.minLevel && level <= wanted.maxLevel + 5)
                fitting.push_back(wanted.itemId);
        return fitting.empty() ? 0 : Acore::Containers::SelectRandomContainerElement(fitting);
    };

    uint32 itemId, count;
    switch (gatherKind)
    {
        case BOT_GATHER_FISHING: itemId = pick(FishGoods); count = 1;           break;
        case BOT_GATHER_MINING:  itemId = pick(OreGoods);  count = urand(1, 3); break;
        default:                 itemId = pick(HerbGoods); count = urand(1, 3); break;
    }
    if (!itemId || !sObjectMgr->GetItemTemplate(itemId))
        return;

    std::lock_guard<std::mutex> lock(GoodsLock);
    uint32& stock = GatheredGoods[bot->GetEntry()][itemId];
    stock = std::min(stock + count, MAX_GATHERED_GOODS);
}

bool BotWorldEvents::FindStoryCamp(Player* player, WorldObject const* center, Position& pos, uint32& enemyEntry,
    std::string& enemyName)
{
    WorldEvent event;
    if (!FindEventTarget(player, center, STORY_CAMP_RANGE, false, event) || event.invaderEntries.empty())
        return false;

    pos.Relocate(event.target);
    enemyEntry = event.invaderEntries.front();
    enemyName = event.enemyName;
    return true;
}

uint32 BotWorldEvents::StartStoryBattle(Player* player, Position const& pos, uint32 enemyEntry,
    std::string const& battleCry)
{
    if (!sObjectMgr->GetCreatureTemplate(enemyEntry))
        return 0;

    WorldEvent event;
    event.id = NextEventId++;
    event.type = EVENT_INVASION;
    event.story = true;
    event.mapId = player->GetMapId();
    event.target.Relocate(pos);
    event.enemyName = CreatureName(enemyEntry);
    event.townName = ZoneName(player->GetAreaId());
    event.invaderEntries = { enemyEntry };
    event.wavesLeft = 2;
    event.attackAt = Now() + 3;
    event.nextWaveAt = event.attackAt;
    event.endAt = event.attackAt + INVASION_MAX_TIME;

    // bots around lend a hand, the player may have to fight alone
    uint32 maxBots = std::max<uint32>(BotCfg::GetBotWorldEventsMaxBots(), EVENT_MIN_BOTS);
    RecruitEventBots(event, player, STORY_BATTLE_RECRUIT_RANGE, 10, maxBots, 0, pos);
    if (!event.bots.empty() && !battleCry.empty())
        if (Creature* leader = GetBot(event.bots.front()))
            leader->GetBotAI()->GetChatter().SayRaw(battleCry, CHAT_MSG_MONSTER_YELL, player);

    uint32 id = event.id;
    Events.push_back(std::move(event));
    return id;
}

BotEventOutcome BotWorldEvents::GetEventOutcome(uint32 eventId)
{
    for (WorldEvent const& event : Events)
        if (event.id == eventId)
            return BOT_EVENT_RUNNING;

    auto itr = FinishedEvents.find(eventId);
    if (itr == FinishedEvents.end())
        return BOT_EVENT_UNKNOWN;
    return itr->second.first ? BOT_EVENT_VICTORY : BOT_EVENT_FAILED;
}

std::string BotWorldEvents::FormatMoney(uint32 copper)
{
    return MoneyString(copper);
}

void BotWorldEvents::OnPlayerEmote(Player const* player, uint32 textEmote)
{
    if (textEmote != TEXT_EMOTE_CHEER)
        return;

    std::lock_guard<std::mutex> lock(CheersLock);
    Cheers.push_back({ player->GetGUID(), player->GetMapId(), player->GetPositionX(), player->GetPositionY(), Now() });
}

std::string BotWorldEvents::FormatItemLink(uint32 itemId, uint32 count)
{
    return ItemLink(itemId, count);
}

std::vector<uint32> BotWorldEvents::GetInvaderEntries(uint8 theme, uint8 level)
{
    std::vector<uint32> entries;
    if (theme >= GetInvaderThemes().size())
        return entries;
    for (CreatureTemplate const* proto : GetInvaderThemes()[theme].creatures)
        if (proto->minlevel <= uint32(level) + 1 && proto->maxlevel + 2 >= uint32(level))
            entries.push_back(proto->Entry);
    return entries;
}

uint32 BotWorldEvents::StartVillainBattle(Player* player, uint32 villainId, uint32 villainEntry, Position const& lair,
    std::string const& villainName, std::string const& place, std::vector<uint32> const& followers,
    std::string const& taunt)
{
    Map* map = player->GetMap();
    TempSummon* villain = map->SummonCreature(villainEntry, lair, nullptr, VILLAIN_LIFETIME);
    if (!villain)
        return 0;
    villain->SetHomePosition(lair);

    WorldEvent event;
    event.id = NextEventId++;
    event.type = EVENT_INVASION;
    event.villainId = villainId;
    event.mapId = map->GetId();
    event.target.Relocate(lair);
    event.rare = villain->GetGUID();
    event.invaders.push_back(villain->GetGUID());
    event.enemyName = villainName;
    event.townName = place;
    event.invaderEntries = followers;
    event.wavesLeft = followers.empty() ? 0 : 2;
    event.attacking = true;
    event.attackAt = Now() + 20;
    event.nextWaveAt = event.attackAt;
    event.endAt = Now() + VILLAIN_BATTLE_TIME;

    uint32 maxBots = std::max<uint32>(BotCfg::GetBotWorldEventsMaxBots(), EVENT_MIN_BOTS) + 2;
    RecruitEventBots(event, player, INVASION_RECRUIT_RANGE, 15, maxBots, 0, lair);
    for (uint32 entry : event.bots)
        if (Creature* bot = GetBot(entry))
            bot->GetBotAI()->GetActivity().StartEventAttack(lair);

    if (!taunt.empty())
        villain->Yell(taunt, LANG_UNIVERSAL);

    uint32 id = event.id;
    Events.push_back(std::move(event));
    return id;
}
