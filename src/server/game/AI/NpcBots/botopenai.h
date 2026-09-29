#ifndef BOTOPENAI_H
#define BOTOPENAI_H

#include "Common.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

/*
NpcBot OpenAI: bot answers generated through the OpenAI Responses API (NpcBot.Chatter.OpenAI.*).
Requests are sent by worker threads, results are polled and delivered from the world thread.
*/

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
};

class BotOpenAI
{
public:
    // enabled in config and supported by this build
    static bool IsEnabled();
    // false if disabled or the queue is full
    static bool Enqueue(BotAIRequest&& request);
    static bool PollResult(BotAIResult& result);
    static void Shutdown();
};

#endif //BOTOPENAI_H
