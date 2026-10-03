#ifndef BOTMEMORY_H
#define BOTMEMORY_H

#include "Common.h"

#include <array>
#include <optional>
#include <string>

/*
NpcBot Memory: what bots remember across server restarts (NpcBot.Chatter.Memory.*).

Generated wandering bots get new entries every start. To make the same bots appear again, each random look
(name, race, gender, model, appearance) becomes a persona; on the next start generated bots of a class take
the stored personas of that class first. Moods and relationships to players (see BotChatter) are kept per
persona, or per entry for bots stored in the database. Everything is written to a JSON file in the data
directory periodically and on shutdown.

World thread only, except GetSocialKey() and GetGuildSeed().
*/

struct BotPersona
{
    uint32 id = 0;
    uint8 botClass = 0;
    uint32 protoEntry = 0;
    uint8 race = 0;
    uint8 gender = 0;
    uint32 displayId = 0;
    bool hasAppearance = false;
    std::array<uint8, 5> appearance{}; // skin, face, hair, haircolor, features
    std::string name;
};

class BotMemory
{
public:
    static void Load();
    static void Save();
    static void Update(uint32 diff);

    // a stored persona of the class not used by another bot yet
    static std::optional<BotPersona> TakePersona(uint8 botClass);
    // stores a new persona, returns its id
    static uint32 AddPersona(BotPersona persona);
    static void BindPersona(uint32 entry, uint32 personaId);
    // generated bot copied from a bot stored in the database: remembers as that bot
    static void BindOriginalEntry(uint32 entry, uint32 originalEntry);

    // key of the bot's mood and relationships: its persona or its entry
    static uint32 GetSocialKey(uint32 entry);
    // stable value to derive the bot's guild from
    static uint32 GetGuildSeed(uint32 entry);
    // social keys worth saving: personas and bots stored in the database
    static bool IsPersistentSocialKey(uint32 key);
};

#endif //BOTMEMORY_H
