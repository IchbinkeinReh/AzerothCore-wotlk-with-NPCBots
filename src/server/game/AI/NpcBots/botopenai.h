#ifndef BOTOPENAI_H
#define BOTOPENAI_H

#include "Common.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

/*
NpcBot OpenAI: bot answers generated through the OpenAI Responses API (NpcBot.Chatter.OpenAI.*).
Requests are sent by worker threads, results are polled and delivered from the world thread.
Every request belongs to a player and is only sent while that player is online: no tokens are spent on bots
talking to nobody.
*/

enum BotAIRequestKind : uint8
{
    BOT_AI_KIND_CHAT = 0,   // a chat answer or a story sentence (BotChatter)
    BOT_AI_KIND_STORY_ARC,  // a story arc made up for a player (BotStoryArcs), raw JSON in BotAIResult::text
    BOT_AI_KIND_VILLAIN     // a villain made up while a player is online (BotVillains), raw JSON
};

struct BotAIMessage
{
    bool fromBot;
    std::string text;
};

struct BotAIRequest
{
    uint32 botEntry;
    ObjectGuid playerGuid;
    uint8 replyMode;
    std::string instructions;
    std::vector<BotAIMessage> input;
    // emotes the bot may answer with
    std::vector<std::string> emotes;
    // the answer may change what the bot is doing (activity field)
    bool allowActivity = false;
    // a sentence of a story told to the player (story_end field)
    bool storyStep = false;
    BotAIRequestKind kind = BOT_AI_KIND_CHAT;
    // own structured output (JSON schema object as text), the answer is returned as is
    std::string schemaName;
    std::string schema;
    // 0: NpcBot.Chatter.OpenAI.MaxOutputTokens
    uint32 maxOutputTokens = 0;
};

struct BotAIResult
{
    uint32 botEntry;
    ObjectGuid playerGuid;
    uint8 replyMode;
    std::string text; // empty on failure or emote only
    std::string emote; // empty or "none" if no emote
    std::string activity; // empty or "keep" if unchanged, "active", "rest", "story"
    bool storyEnd = false;
    BotAIRequestKind kind = BOT_AI_KIND_CHAT;
};

class BotOpenAI
{
public:
    // enabled in config and supported by this build
    static bool IsEnabled();
    // false if disabled, the queue is full or the player of the request is not online
    static bool Enqueue(BotAIRequest&& request);
    static bool PollResult(BotAIResult& result);
    static void Shutdown();
};

#endif //BOTOPENAI_H
