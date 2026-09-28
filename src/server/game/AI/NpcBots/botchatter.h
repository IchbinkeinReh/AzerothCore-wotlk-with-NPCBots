#ifndef BOTCHATTER_H
#define BOTCHATTER_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <unordered_map>
#include <vector>

/*
NpcBot Chatter: bots greet nearby players, answer greetings, comment on kills and level-ups
and occasionally talk in /say or in the zone's General channel.
Inspired by cmangos playerbots BroadcastHelper.

Texts are stored in `npc_text` (BOT_TEXT_CHATTER_* in bottext.h), one variant per `textN_0` column,
and translated through `npc_text_locale`. Supported placeholders:
    %name           - bot name
    %level          - bot level
    %class          - bot class
    %race           - bot race
    %zone           - current zone name
    %target         - player (greetings) or victim (kills) name
    %target_level   - player or victim level
*/

class bot_ai;
class Channel;
class Creature;
class Group;
class Player;
class Unit;
class WorldObject;
class WorldPacket;

enum BotChatterCategory : uint8
{
    BOT_CHATTER_GREET           = 0,
    BOT_CHATTER_GREET_REPLY,
    BOT_CHATTER_BYE_REPLY,
    BOT_CHATTER_IDLE,
    BOT_CHATTER_KILL_NORMAL,
    BOT_CHATTER_KILL_ELITE,
    BOT_CHATTER_KILL_RARE,
    BOT_CHATTER_KILL_BOSS,
    BOT_CHATTER_KILL_PLAYER,
    BOT_CHATTER_LEVEL_UP,
    BOT_CHATTER_WHISPER_REPLY,
    BOT_CHATTER_WHISPER_BUSY,

    BOT_CHATTER_CATEGORY_END
};

// where a reply to a player goes: the chat the player used
enum BotChatterReplyMode : uint8
{
    BOT_CHATTER_REPLY_SAY       = 0,
    BOT_CHATTER_REPLY_PARTY,
    BOT_CHATTER_REPLY_RAID,
    BOT_CHATTER_REPLY_CHANNEL
};

class BotChatter
{
public:
    BotChatter(bot_ai* ai, Creature* bot);

    void Update(uint32 diff);

    void OnKilledUnit(Unit const* victim);
    void OnLevelUp();

    // player chat hooks, called from the world thread
    static void OnPlayerSay(Player const* player, std::string_view message);
    static void OnPlayerGroupChat(Player const* player, Group* group, ChatMsg msgType, std::string_view message);
    static void OnPlayerChannelChat(Player const* player, Channel const* channel, std::string_view message);
    // returns true if the whisper was delivered to a bot
    static bool OnPlayerWhisper(Player* player, std::string const& botName, std::string_view message);

    // builds one chat packet for a specific locale, used by Acore::LocalizedPacketDo
    class TextBuilder
    {
    public:
        TextBuilder(BotChatter const& chatter, ChatMsg msgType, uint32 textId, uint8 slot, WorldObject const* subject,
            std::string const& channelName = "")
            : _chatter(chatter), _msgType(msgType), _textId(textId), _slot(slot), _subject(subject),
            _channelName(channelName) { }

        void operator()(WorldPacket& data, LocaleConstant locale) const;

    private:
        BotChatter const& _chatter;
        ChatMsg _msgType;
        uint32 _textId;
        uint8 _slot;
        WorldObject const* _subject;
        std::string _channelName;
    };

private:
    static void HandlePlayerMessage(Player const* player, std::string_view message, BotChatterReplyMode mode,
        std::vector<Creature*> const& bots);

    void ReplyToWhisper(Player* player, std::string_view message);

    bool CanChat(bool unprompted, bool inCombat = false) const;
    bool IsOwnedBy(Player const* player) const;
    bool CanReplyTo(Player const* player, bool addressed) const;
    void QueueReply(Player const* player, BotChatterCategory category, BotChatterReplyMode mode, uint32 delay);
    void SendPendingReply();

    bool TryGreetNearbyPlayer();
    void Chatter(BotChatterCategory category, WorldObject const* subject, bool allowChannel);

    bool HasPlayersInRange(float range) const;
    bool SayNearby(BotChatterCategory category, WorldObject const* subject, ChatMsg msgType);
    bool SayToGroup(BotChatterCategory category, WorldObject const* subject, ChatMsg msgType);
    bool SayToZoneChannel(BotChatterCategory category, WorldObject const* subject, bool ignoreCooldown = false);

    std::string FormatText(uint32 textId, uint8 slot, LocaleConstant locale, WorldObject const* subject) const;

    bot_ai* _ai;
    Creature* _me;

    uint32 _greetTimer;
    uint32 _idleTimer;

    // players this bot already greeted (or was greeted by) -> game time of that greeting
    std::unordered_map<ObjectGuid, time_t> _greeted;

    ObjectGuid _replyTarget;
    BotChatterCategory _replyCategory;
    BotChatterReplyMode _replyMode;
    uint32 _replyTimer;
};

#endif //BOTCHATTER_H
