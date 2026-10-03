#ifndef BOTCHATTER_H
#define BOTCHATTER_H

#include "botopenai.h"
#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <deque>
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
    BOT_CHATTER_ZONE_ENTER,
    BOT_CHATTER_PLAYER_DIED,
    BOT_CHATTER_PLAYER_LEVELUP,
    BOT_CHATTER_TIME_MORNING,
    BOT_CHATTER_TIME_EVENING,
    BOT_CHATTER_TIME_NIGHT,
    BOT_CHATTER_WEATHER_RAIN,
    BOT_CHATTER_WEATHER_SNOW,
    BOT_CHATTER_WEATHER_STORM,
    BOT_CHATTER_MOOD_CHEERFUL,
    BOT_CHATTER_MOOD_GRUMPY,
    BOT_CHATTER_MOOD_TIRED,

    BOT_CHATTER_CATEGORY_END
};

enum BotMood : uint8
{
    BOT_MOOD_NORMAL             = 0,
    BOT_MOOD_CHEERFUL,
    BOT_MOOD_GRUMPY,
    BOT_MOOD_TIRED
};

// what a player did to a bot, see BotChatter::NoteRelation()
enum BotRelationEvent : uint8
{
    BOT_RELATION_TALK           = 0,
    BOT_RELATION_FIGHT_TOGETHER,
    BOT_RELATION_ATTACKED_ME,
    BOT_RELATION_KILLED_ME,
    BOT_RELATION_I_KILLED,
    BOT_RELATION_STORY,
    BOT_RELATION_HIRED,
    BOT_RELATION_FRIENDLY_EMOTE,
    BOT_RELATION_HOSTILE_EMOTE
};

// where a reply to a player goes: the chat the player used
enum BotChatterReplyMode : uint8
{
    BOT_CHATTER_REPLY_SAY       = 0,
    BOT_CHATTER_REPLY_PARTY,
    BOT_CHATTER_REPLY_RAID,
    BOT_CHATTER_REPLY_CHANNEL,
    BOT_CHATTER_REPLY_WHISPER,
    BOT_CHATTER_REPLY_STORY     // the next sentence of a story told to the player
};

class BotChatter
{
public:
    BotChatter(bot_ai* ai, Creature* bot);

    void Update(uint32 diff);

    void OnKilledUnit(Unit const* victim);
    void OnLevelUp();
    void OnZoneChanged();
    void OnDied(Unit const* killer);
    void OnHired(Player const* owner);

    // mood and relationships to players, kept per bot entry or persona (survive despawns and restarts, see
    // botmemory.h)
    BotMood GetMood() const;
    bool IsTired() const { return GetMood() == BOT_MOOD_TIRED; }
    void NoteRelation(Player const* player, BotRelationEvent event);
    int32 GetAffinity(ObjectGuid player) const;

    // relationships to other bots (friends and rivals), symmetric, kept like the ones to players
    static void NoteBondBetween(Creature const* a, Creature const* b, int32 delta);
    int32 GetBondAffinity(Creature const* other) const;
    bool HasBond(Creature const* other) const;

    // a line of a text id in /say (campfire stories), localized for each listener
    bool SayText(uint32 textId, uint8 slot);
    void DoTextEmote(uint32 textEmote);
    // asks OpenAI for the next sentence of the story told to the player, see BotActivity
    bool RequestStoryStep(Player const* player);
    // dancing, sleeping... for a few seconds
    bool IsPerformingStateEmote() const { return _stateEmoteTimer > 0; }

    // player chat hooks, called from the world thread
    static void OnPlayerSay(Player const* player, std::string_view message);
    static void OnPlayerGroupChat(Player const* player, Group* group, ChatMsg msgType, std::string_view message);
    static void OnPlayerChannelChat(Player const* player, Channel const* channel, std::string_view message);
    // returns true if the whisper was delivered to a bot
    static bool OnPlayerWhisper(Player* player, std::string const& botName, std::string_view message);
    // delivers the OpenAI answers, called from the world thread
    static void ProcessAIReplies();
    // a player's /emote, called from the player's map thread
    static void OnPlayerTextEmote(Player* player, uint32 textEmote, Unit const* target);

    // moods and relationships of all bots, saved and loaded by BotMemory
    struct RelationRecord
    {
        uint32 player = 0; // player guid counter
        std::string name;
        int32 affinity = 0;
        uint32 talks = 0;
        uint32 fights = 0;
        uint32 attacks = 0;
        uint32 killedMe = 0;
        uint32 killedThem = 0;
        uint32 stories = 0;
        uint32 friendly = 0;
        uint32 hostile = 0;
        bool hired = false;
        time_t lastSeen = 0;
    };
    struct BondRecord
    {
        uint32 key = 0; // social key of the other bot
        std::string name;
        int32 affinity = 0;
        uint32 meetings = 0;
        time_t lastSeen = 0;
    };
    struct SocialRecord
    {
        uint32 key = 0;
        int32 mood = 0;
        uint32 activeMinutes = 0;
        std::vector<RelationRecord> relations;
        std::vector<BondRecord> bonds;
    };
    static std::vector<SocialRecord> ExportSocial();
    static void ImportSocial(std::vector<SocialRecord> const& records);

    // extra placeholders of a text and their values, e.g. { "%quest", "The Missing Diplomat" }
    using TextVars = std::vector<std::pair<std::string, std::string>>;

    // a text id variant (localized for each listener) or a raw text
    struct ChatterText
    {
        uint32 textId = 0;
        uint8 slot = 0;
        std::string raw;
        TextVars vars;
    };

    // world events (botworldevents.h), world thread: a random variant of a text id in /say, /yell (nearby
    // players), the zone's General channel, a whisper to target or the bot's group chat. %target is target
    static bool HasText(uint32 textId);
    // whole words or phrases of a keyword text (any locale) in a message
    static bool MatchesKeywordText(std::string_view message, uint32 textId);
    // the language bots talk in (NpcBot.Chatter.Locale)
    static LocaleConstant GetServerLocale();
    bool Announce(uint32 textId, TextVars vars, ChatMsg msgType, Player* target = nullptr);
    // to every player of the bot's faction who joined the channel (ChatChannels.dbc id), e.g. Trade
    bool AnnounceToChannel(uint32 channelId, uint32 textId, TextVars vars);
    // the first variant of a text id in the server language
    static std::string GetServerText(uint32 textId);
    // a raw text (e.g. made up by OpenAI): /say, /yell or a whisper to target
    bool SayRaw(std::string const& text, ChatMsg msgType, Player* target = nullptr);
    // a single chat line out of an OpenAI answer
    static std::string SanitizeText(std::string const& text);
    // English name of the server language, for OpenAI
    static char const* GetServerLanguageName();

    // builds one chat packet for a specific locale, used by Acore::LocalizedPacketDo
    class TextBuilder
    {
    public:
        TextBuilder(BotChatter const& chatter, ChatMsg msgType, ChatterText const& text, WorldObject const* subject,
            std::string const& channelName = "")
            : _chatter(chatter), _msgType(msgType), _text(text), _subject(subject), _channelName(channelName) { }

        void operator()(WorldPacket& data, LocaleConstant locale) const;

    private:
        BotChatter const& _chatter;
        ChatMsg _msgType;
        ChatterText const& _text;
        WorldObject const* _subject;
        std::string _channelName;
    };

private:
    static void HandlePlayerMessage(Player const* player, std::string_view message, BotChatterReplyMode mode,
        std::vector<Creature*> const& bots, ObjectGuid preferredBot = ObjectGuid::Empty);

    void ReplyToWhisper(Player* player, std::string_view message);

    bool CanChat(bool unprompted, bool inCombat = false) const;
    bool IsOwnedBy(Player const* player) const;
    bool CanReplyTo(Player const* player, bool addressed) const;
    void QueueReply(Player const* player, BotChatterCategory category, BotChatterReplyMode mode, uint32 delay);
    void SendPendingReply();
    void DeliverReply(ChatterText const& text, ObjectGuid targetGuid, BotChatterReplyMode mode);

    bool RequestAIReply(Player const* player, std::string_view message, BotChatterReplyMode mode);
    void OnAIReply(BotAIResult const& result);
    std::string BuildAIInstructions(Player const* player, BotChatterReplyMode mode) const;

    bool TryGreetNearbyPlayer();
    bool GreetBotNearby();
    void NoteBond(uint32 otherKey, std::string const& otherName, int32 delta);
    std::string GetBondsText() const;
    bool TalkAboutRival();
    bool TalkAboutNews();
    // a text id in /say a moment later, e.g. answering another bot
    void SayLater(uint32 textId, ObjectGuid subject, uint32 delay);
    bool SayTextNearby(uint32 textId, WorldObject const* subject, TextVars vars = {});
    void ObservePlayers();
    void UpdateSocial();
    void ChangeMood(int32 delta);
    BotChatterCategory SelectIdleCategory();
    std::string GetRelationshipText(Player const* player) const;
    void Chatter(BotChatterCategory category, WorldObject const* subject, bool allowChannel);

    bool HasPlayersInRange(float range) const;
    bool SayNearby(BotChatterCategory category, WorldObject const* subject, ChatMsg msgType);
    bool SayNearby(ChatterText const& text, WorldObject const* subject, ChatMsg msgType);
    bool SayToGroup(ChatterText const& text, WorldObject const* subject, ChatMsg msgType);
    bool SayToZoneChannel(BotChatterCategory category, WorldObject const* subject, bool ignoreCooldown = false);
    bool SayToZoneChannel(ChatterText const& text, WorldObject const* subject, bool ignoreCooldown);
    bool WhisperTo(ChatterText const& text, Player* player);
    void PerformEmote(uint32 textEmote, Unit* target);
    void EndStateEmote();
    void ApplyAIActivity(std::string const& activity, Player const* player);
    // stand still for about as long as typing the message would take
    void PauseToType(ChatterText const& text, WorldObject const* subject);
    void ReactToEmote(Player* player, uint32 textEmote, bool atMe);

    std::string FormatText(ChatterText const& text, LocaleConstant locale, WorldObject const* subject) const;
    std::string GetDefaultText(ChatterText const& text, WorldObject const* subject) const;

    bot_ai* _ai;
    Creature* _me;

    uint32 _greetTimer;
    uint32 _idleTimer;
    uint32 _socialTimer;

    // players around: dying, leveling, fighting with or against the bot
    struct ObservedPlayer
    {
        uint8 level;
        bool alive;
        bool foughtTogether;
        bool attackedMe;
    };
    std::unordered_map<ObjectGuid, ObservedPlayer> _observed;

    // players this bot already greeted (or was greeted by) -> game time of that greeting
    std::unordered_map<ObjectGuid, time_t> _greeted;

    ObjectGuid _replyTarget;
    BotChatterCategory _replyCategory;
    BotChatterReplyMode _replyMode;
    uint32 _replyTimer;

    // what the bot said and heard in /say and whispers, context for OpenAI answers (party/raid and General are
    // shared logs)
    struct ChatMemoryLine
    {
        std::string speaker;
        std::string text;
        time_t time;
    };
    std::deque<ChatMemoryLine> _sayMemory;
    std::deque<ChatMemoryLine> _whisperMemory;

    static void Remember(std::deque<ChatMemoryLine>& memory, std::string_view speaker, std::string_view text);
    static std::string GetMemoryText(std::deque<ChatMemoryLine> const& memory);
    std::string BuildAIContext(Player const* player) const;
    // waiting for an answer until then
    time_t _aiPendingUntil;

    // lasting emote (dance, sleep, sit, kneel) until the timer runs out
    uint32 _stateEmote;
    uint32 _stateEmoteTimer;

    // line said a moment later
    uint32 _delayedTextId;
    ObjectGuid _delayedSubject;
    uint32 _delayedTimer;

    // emote answering a player's emote
    uint32 _emoteReaction;
    ObjectGuid _emoteTarget;
    uint32 _emoteTimer;

    bool IsAIPending() const;
};

#endif //BOTCHATTER_H
