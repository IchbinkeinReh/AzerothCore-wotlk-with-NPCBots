#ifndef BOTWORLDEVENTS_H
#define BOTWORLDEVENTS_H

#include "Common.h"

#include <string>
#include <string_view>

/*
NpcBot World Events: free wandering bots that make the world feel inhabited (NpcBot.WanderingBots.*):
- group finder: a bot asks a player to do an elite (group) quest of the player's quest log or a dungeon fitting
                the player's level together, with more bots joining on accept
- quest help:   a bot near a player fighting a creature needed for a quest joins the fight ("I need those too!")
                and offers to team up until the quest is done
- events:       several bots gather near a player, call for help and then raid a camp of hostile creatures or
                hunt a rare creature, players are welcome to join
- raids:        a bot organizes a world boss or raid run and invites the player, a whole raid of bots joins

Offers come as a regular group invite from the bot (or as a whisper answered with yes or no). Bots hired through
an offer stay until the task is done (quest finished, dungeon or raid left, world boss defeated) or the time is
up, then say goodbye and leave the group.

Everything runs in the world thread while maps are not updated (BotDataMgr::Update and session handlers).
*/

class Player;

class BotWorldEvents
{
public:
    static void Update(uint32 diff);

    // answer to a bot's group invite, returns true if the invite came from a bot
    static bool OnGroupInviteAnswer(Player* player, bool accept);
    // a whispered yes or no to a bot with a pending offer, returns true if the whisper answered an offer
    static bool OnPlayerWhisper(Player* player, std::string const& botName, std::string_view message);
};

#endif //BOTWORLDEVENTS_H
