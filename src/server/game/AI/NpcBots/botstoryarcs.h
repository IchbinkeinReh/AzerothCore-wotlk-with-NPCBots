#ifndef BOTSTORYARCS_H
#define BOTSTORYARCS_H

#include "botopenai.h"
#include "Common.h"

#include <string>
#include <string_view>
#include <vector>

/*
NpcBot Story Arcs: small quest lines OpenAI makes up for a player (NpcBot.Chatter.OpenAI.StoryArcs.*).

A free wandering bot near the player asks for help (the giver). The player has to find another bot somewhere
else in the zone (the helper), by walking up to it or by whispering it. The helper's clue leads to a camp of
hostile creatures near it, where waves of them attack the player, bots around join the fight. After the
victory the giver thanks the player by whisper and pays a reward, the news spread (botnews.h).

OpenAI is asked once per arc, while the player is online, everything later uses the texts it made up. Arcs
last up to three days and are kept in the bot memory file. World thread only.
*/

class Player;

class BotStoryArcs
{
public:
    static void Update(uint32 diff);
    static void OnArcGenerated(BotAIResult const& result);
    // a whisper to the giver or the helper of the player's arc (in the world or not), returns true if handled
    static bool OnPlayerWhisper(Player* player, std::string const& botName, std::string_view message);

    // bot memory
    struct ArcRecord
    {
        uint32 player = 0;
        uint32 giverKey = 0;
        std::string giverName;
        uint32 helperKey = 0;
        std::string helperName;
        std::string helperPlace;
        std::string title;
        std::string intro;
        std::string helperLine;
        std::string battleCry;
        std::string finale;
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 zoneId = 0;
        uint32 enemyEntry = 0;
        std::string enemyName;
        std::string campPlace;
        uint8 step = 0;
        time_t expires = 0;
    };
    static std::vector<ArcRecord> ExportArcs();
    static void ImportArcs(std::vector<ArcRecord> const& arcs);
};

#endif //BOTSTORYARCS_H
