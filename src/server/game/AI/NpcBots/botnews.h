#ifndef BOTNEWS_H
#define BOTNEWS_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <string>
#include <vector>

/*
NpcBot News: bots spread news of what players did (NpcBot.Chatter.News.Enable). Deeds come from world events,
invasions, world pvp objectives, world bosses, dungeons and story arcs the bots took part in. Bots talk about
recent deeds of their faction in the General channel, mention them in OpenAI conversations and greet famous
players they never met. Every deed makes the player more famous. Kept in the bot memory file.

Deeds are recorded from the world thread, news is read from map threads too.
*/

class Creature;
class Player;

enum BotDeedType : uint8
{
    BOT_DEED_TOWN_DEFENDED = 0,     // subject: invaders, place: town
    BOT_DEED_CAMP_RAIDED,           // subject: camp creature, place: zone
    BOT_DEED_RARE_SLAIN,            // subject: rare creature, place: zone
    BOT_DEED_OBJECTIVE_TAKEN,       // subject: capture point, place: zone
    BOT_DEED_WORLD_BOSS,            // subject: world boss, place: zone
    BOT_DEED_DUNGEON,               // subject: dungeon or raid
    BOT_DEED_STORY_ARC,             // subject: story title, place: zone
    BOT_DEED_VILLAIN,               // subject: villain, place: lair
    BOT_DEED_CONTEST,               // subject: "a race", "a drinking contest"..., place: where

    BOT_DEED_TYPE_END
};

struct BotDeed
{
    uint32 player = 0;              // guid counter
    std::string playerName;
    uint8 team = TEAM_NEUTRAL;
    uint8 type = BOT_DEED_TOWN_DEFENDED;
    std::string subject;
    std::string place;
    uint32 zoneId = 0;
    time_t time = 0;
    uint32 told = 0;                // how often bots talked about it
};

class BotNews
{
public:
    static void RecordDeed(Player const* player, BotDeedType type, std::string const& subject,
        std::string const& place, uint32 zoneId);

    // fame of a player: grows with every deed
    static uint32 GetFame(ObjectGuid player);
    static bool IsFamous(ObjectGuid player);

    // a deed of the faction worth talking about, the zone's first, empty if none
    static std::string TakeNews(TeamId team, uint32 zoneId);
    // what a bot heard about the player and the latest news, for OpenAI
    static std::string GetNewsContext(TeamId team, ObjectGuid player);
    // the latest deed of a player as a sentence
    static std::string GetLatestDeedText(ObjectGuid player);

    // a true rumor: a rare creature alive near the bot ("They say ... near ..."), empty if none, map thread
    static std::string GetRareRumor(Creature const* bot);

    // bot memory
    static std::vector<BotDeed> ExportDeeds();
    static void ImportDeeds(std::vector<BotDeed> const& deeds);
};

#endif //BOTNEWS_H
