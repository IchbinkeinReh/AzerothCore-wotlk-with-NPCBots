#include "botchatter.h"
#include "botconfig.h"
#include "botnews.h"
#include "bottext.h"
#include "Containers.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "World.h"

#include <mutex>
#include <sstream>
#include <unordered_map>

/*
NpcBot News, see botnews.h
*/

namespace
{
    constexpr std::size_t MAX_DEEDS = 200;
    constexpr time_t NEWS_MAX_AGE = 7 * DAY;
    constexpr uint32 NEWS_MAX_TOLD = 4;
    constexpr uint32 FAME_FAMOUS = 6;

    constexpr std::array<uint32, BOT_DEED_TYPE_END> DeedFame = { 3, 1, 1, 1, 4, 2, 3, 5, 1 };
    constexpr std::array<uint32, BOT_DEED_TYPE_END> DeedTexts =
    {
        BOT_TEXT_DEED_TOWN_DEFENDED, BOT_TEXT_DEED_CAMP_RAIDED, BOT_TEXT_DEED_RARE_SLAIN, BOT_TEXT_DEED_OBJECTIVE_TAKEN,
        BOT_TEXT_DEED_WORLD_BOSS, BOT_TEXT_DEED_DUNGEON, BOT_TEXT_DEED_STORY_ARC, BOT_TEXT_DEED_VILLAIN,
        BOT_TEXT_DEED_CONTEST
    };
    constexpr float RUMOR_RANGE = 600.0f;

    // rare creatures spawned on the continents, per map
    struct RareSpawn
    {
        ObjectGuid::LowType spawnId;
        float x;
        float y;
    };

    std::mutex GossipLock;
    std::vector<BotDeed> Deeds;
    std::unordered_map<uint32 /*player*/, uint32> Fame;

    void ReplaceAll(std::string& text, std::string_view token, std::string_view value)
    {
        for (std::size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size()))
            text.replace(pos, token.size(), value);
    }

    // a sentence in the server language: "Patrick defended Goldshire against the Scourge."
    std::string FormatDeed(BotDeed const& deed)
    {
        if (deed.type >= BOT_DEED_TYPE_END)
            return "";

        std::string text = BotChatter::GetServerText(DeedTexts[deed.type]);
        ReplaceAll(text, "%player", deed.playerName);
        ReplaceAll(text, "%subject", deed.subject);
        ReplaceAll(text, "%place", deed.place);
        return text;
    }
}

void BotNews::RecordDeed(Player const* player, BotDeedType type, std::string const& subject,
    std::string const& place, uint32 zoneId)
{
    if (!BotCfg::IsBotNewsEnabled() || !player || type >= BOT_DEED_TYPE_END)
        return;

    uint32 counter = player->GetGUID().GetCounter();
    time_t now = GameTime::GetGameTime().count();

    std::lock_guard<std::mutex> lock(GossipLock);

    // the same deed again in a short time (several bots, several waves) is not new
    for (BotDeed const& deed : Deeds)
        if (deed.player == counter && deed.type == type && deed.subject == subject && deed.time + HOUR > now)
            return;

    BotDeed& deed = Deeds.emplace_back();
    deed.player = counter;
    deed.playerName = player->GetName();
    deed.team = player->GetTeamId();
    deed.type = type;
    deed.subject = subject;
    deed.place = place;
    deed.zoneId = zoneId;
    deed.time = now;

    Fame[counter] += DeedFame[type];

    if (Deeds.size() > MAX_DEEDS)
        Deeds.erase(Deeds.begin());
}

uint32 BotNews::GetFame(ObjectGuid player)
{
    std::lock_guard<std::mutex> lock(GossipLock);
    auto itr = Fame.find(player.GetCounter());
    return itr != Fame.end() ? itr->second : 0;
}

bool BotNews::IsFamous(ObjectGuid player)
{
    return BotCfg::IsBotNewsEnabled() && GetFame(player) >= FAME_FAMOUS;
}

std::string BotNews::TakeNews(TeamId team, uint32 zoneId)
{
    if (!BotCfg::IsBotNewsEnabled())
        return "";

    time_t now = GameTime::GetGameTime().count();

    std::lock_guard<std::mutex> lock(GossipLock);
    std::vector<BotDeed*> local, other;
    for (BotDeed& deed : Deeds)
    {
        if (deed.team != team || deed.told >= NEWS_MAX_TOLD || deed.time + NEWS_MAX_AGE <= now)
            continue;
        (deed.zoneId == zoneId ? local : other).push_back(&deed);
    }

    // news travel: the own zone first, the rest of the world sometimes
    std::vector<BotDeed*> const& pool = (!local.empty() && (other.empty() || roll_chance_i(70))) ? local : other;
    if (pool.empty())
        return "";

    BotDeed* deed = Acore::Containers::SelectRandomContainerElement(pool);
    ++deed->told;
    return FormatDeed(*deed);
}

std::string BotNews::GetNewsContext(TeamId team, ObjectGuid player)
{
    if (!BotCfg::IsBotNewsEnabled())
        return "";

    time_t now = GameTime::GetGameTime().count();
    std::vector<std::string> aboutPlayer, latest;
    {
        std::lock_guard<std::mutex> lock(GossipLock);
        for (auto itr = Deeds.rbegin(); itr != Deeds.rend(); ++itr)
        {
            if (itr->time + NEWS_MAX_AGE <= now)
                continue;
            if (itr->player == player.GetCounter())
            {
                if (aboutPlayer.size() < 3)
                    aboutPlayer.push_back(FormatDeed(*itr));
            }
            else if (itr->team == team && latest.size() < 2)
                latest.push_back(FormatDeed(*itr));
        }
    }

    std::ostringstream ss;
    if (!aboutPlayer.empty())
    {
        ss << "What people say about the one talking to you:";
        for (std::string const& text : aboutPlayer)
            ss << ' ' << text;
        ss << ' ';
    }
    if (!latest.empty())
    {
        ss << "Other news going around:";
        for (std::string const& text : latest)
            ss << ' ' << text;
        ss << ' ';
    }
    return ss.str();
}

std::string BotNews::GetLatestDeedText(ObjectGuid player)
{
    std::lock_guard<std::mutex> lock(GossipLock);
    for (auto itr = Deeds.rbegin(); itr != Deeds.rend(); ++itr)
        if (itr->player == player.GetCounter())
            return FormatDeed(*itr);
    return "";
}

std::string BotNews::GetRareRumor(Creature const* bot)
{
    static std::unordered_map<uint32 /*map*/, std::vector<RareSpawn>> spawns;
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(data.id);
            if (map && map->IsContinent() && proto &&
                (proto->rank == CREATURE_ELITE_RARE || proto->rank == CREATURE_ELITE_RAREELITE))
                spawns[data.mapid].push_back({ spawnId, data.posX, data.posY });
        }
    });

    if (!BotCfg::IsBotNewsEnabled() || !bot->IsInWorld())
        return "";

    auto itr = spawns.find(bot->GetMapId());
    if (itr == spawns.end())
        return "";

    // only rares that are really there: spawned, alive, of a level the bot cares about
    Map* map = bot->GetMap();
    std::vector<Creature*> rares;
    for (RareSpawn const& spawn : itr->second)
    {
        if (bot->GetExactDist2d(spawn.x, spawn.y) > RUMOR_RANGE)
            continue;
        auto bounds = map->GetCreatureBySpawnIdStore().equal_range(spawn.spawnId);
        for (auto creature = bounds.first; creature != bounds.second; ++creature)
            if (creature->second->IsAlive() && creature->second->IsInWorld() &&
                std::abs(int32(creature->second->GetLevel()) - int32(bot->GetLevel())) <= 10)
                rares.push_back(creature->second);
    }
    if (rares.empty())
        return "";

    Creature const* rare = Acore::Containers::SelectRandomContainerElement(rares);
    LocaleConstant locale = BotChatter::GetServerLocale();
    LocaleConstant dbcLocale = sWorld->GetAvailableDbcLocale(locale);

    std::string name = rare->GetCreatureTemplate()->Name;
    if (CreatureLocale const* creatureLocale = sObjectMgr->GetCreatureLocale(rare->GetEntry()))
        ObjectMgr::GetLocaleString(creatureLocale->Name, locale, name);

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(rare->GetAreaId());
    std::string place = area ? area->area_name[dbcLocale] : "";
    if (place.empty())
        return "";

    std::string text = BotChatter::GetServerText(BOT_TEXT_RUMOR_RARE);
    ReplaceAll(text, "%enemy", name);
    ReplaceAll(text, "%place", place);
    return text;
}

std::vector<BotDeed> BotNews::ExportDeeds()
{
    std::lock_guard<std::mutex> lock(GossipLock);
    return Deeds;
}

void BotNews::ImportDeeds(std::vector<BotDeed> const& deeds)
{
    std::lock_guard<std::mutex> lock(GossipLock);
    for (BotDeed const& deed : deeds)
    {
        if (deed.type >= BOT_DEED_TYPE_END || !deed.player)
            continue;
        Deeds.push_back(deed);
        Fame[deed.player] += DeedFame[deed.type];
    }
    while (Deeds.size() > MAX_DEEDS)
        Deeds.erase(Deeds.begin());
}
