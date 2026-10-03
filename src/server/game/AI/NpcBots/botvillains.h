#ifndef BOTVILLAINS_H
#define BOTVILLAINS_H

#include "botopenai.h"
#include "Common.h"

#include <string>
#include <vector>

/*
NpcBot Villains: recurring antagonists OpenAI makes up (NpcBot.Chatter.OpenAI.Villains.*).

A villain is a named elite creature of the Scourge, the Burning Legion or a band of raiders, living in a lair in
a zone, with a title, a cult of followers and a backstory made up by OpenAI. Its followers show up in invasions of
its kind and level and stand behind the story arcs around it. Every defeat of its followers (invasions won, story
arcs finished) weakens it, bots talk about it in General. Once weak enough it shows itself at its lair: bots call
everyone in the zone, the villain and waves of followers fight the players and bots there. Defeated, the deed is
told around; escaped, it gathers new followers.

OpenAI is asked once per villain, only while a player is online. Villains are kept in the bot memory file. World
thread only.
*/

struct BotVillainRef
{
    uint32 id = 0;
    std::string name;
    std::string title;
    std::string cult;
    std::string backstory;
};

class BotVillains
{
public:
    static void Update(uint32 diff);
    static void OnVillainGenerated(BotAIResult const& result);

    // the active villain behind enemies of an invader theme (0 Scourge, 1 Legion, 2 raiders, ANY_THEME) around a
    // level on a map
    static constexpr uint8 ANY_THEME = 0xFF;
    static bool GetVillainFor(uint8 theme, uint8 level, uint32 mapId, BotVillainRef& out);
    // followers of the villain lost a fight against players
    static void OnFollowersDefeated(uint32 villainId);

    // bot memory
    struct VillainRecord
    {
        uint32 id = 0;
        uint32 entry = 0;           // the creature it is
        std::string name;
        std::string title;
        std::string cult;
        std::string backstory;
        std::string taunt;
        std::string defeatLine;
        uint8 theme = 0;
        uint8 minLevel = 0;
        uint8 maxLevel = 0;
        uint32 mapId = 0;
        uint32 zoneId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::string lairPlace;
        uint32 progress = 0;
        uint8 state = 0;
        time_t created = 0;
        time_t defeated = 0;
    };
    static std::vector<VillainRecord> ExportVillains();
    static void ImportVillains(std::vector<VillainRecord> const& villains);
};

#endif //BOTVILLAINS_H
