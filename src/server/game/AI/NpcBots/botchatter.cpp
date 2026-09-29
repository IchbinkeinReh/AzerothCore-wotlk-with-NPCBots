#include "bot_ai.h"
#include "botchatter.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botdefine.h"
#include "botmgr.h"
#include "botopenai.h"
#include "bottext.h"
#include "CellImpl.h"
#include "Channel.h"
#include "Chat.h"
#include "Containers.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Tokenize.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <sstream>

/*
NpcBot Chatter, see botchatter.h
*/

namespace
{
    constexpr float CHATTER_GREET_RANGE = 15.0f;
    constexpr float CHATTER_CHANNEL_REPLY_RANGE = 100.0f;
    constexpr uint32 CHATTER_CHANNEL_GENERAL = 1; // ChatChannels.dbc
    constexpr std::size_t CHATTER_MAX_REPLIES = 2;
    constexpr uint8 CHATTER_TRIVIAL_KILL_LEVEL_DIFF = 10;
    constexpr int32 CHATTER_CHANNEL_OVER_SAY_CHANCE = 40;

    struct ChatterTextRange
    {
        uint32 first;
        uint32 last;
    };

    constexpr std::array<ChatterTextRange, BOT_CHATTER_CATEGORY_END> ChatterTexts =
    {{
        { BOT_TEXT_CHATTER_GREET_1,         BOT_TEXT_CHATTER_GREET_2        }, // BOT_CHATTER_GREET
        { BOT_TEXT_CHATTER_GREET_REPLY,     BOT_TEXT_CHATTER_GREET_REPLY    }, // BOT_CHATTER_GREET_REPLY
        { BOT_TEXT_CHATTER_BYE_REPLY,       BOT_TEXT_CHATTER_BYE_REPLY      }, // BOT_CHATTER_BYE_REPLY
        { BOT_TEXT_CHATTER_IDLE_1,          BOT_TEXT_CHATTER_IDLE_3         }, // BOT_CHATTER_IDLE
        { BOT_TEXT_CHATTER_KILL_NORMAL,     BOT_TEXT_CHATTER_KILL_NORMAL    }, // BOT_CHATTER_KILL_NORMAL
        { BOT_TEXT_CHATTER_KILL_ELITE,      BOT_TEXT_CHATTER_KILL_ELITE     }, // BOT_CHATTER_KILL_ELITE
        { BOT_TEXT_CHATTER_KILL_RARE,       BOT_TEXT_CHATTER_KILL_RARE      }, // BOT_CHATTER_KILL_RARE
        { BOT_TEXT_CHATTER_KILL_BOSS,       BOT_TEXT_CHATTER_KILL_BOSS      }, // BOT_CHATTER_KILL_BOSS
        { BOT_TEXT_CHATTER_KILL_PLAYER,     BOT_TEXT_CHATTER_KILL_PLAYER    }, // BOT_CHATTER_KILL_PLAYER
        { BOT_TEXT_CHATTER_LEVEL_UP,        BOT_TEXT_CHATTER_LEVEL_UP       }, // BOT_CHATTER_LEVEL_UP
        { BOT_TEXT_CHATTER_WHISPER_REPLY,   BOT_TEXT_CHATTER_WHISPER_REPLY  }, // BOT_CHATTER_WHISPER_REPLY
        { BOT_TEXT_CHATTER_WHISPER_BUSY,    BOT_TEXT_CHATTER_WHISPER_BUSY   }, // BOT_CHATTER_WHISPER_BUSY
    }};

    std::array<std::atomic<bool>, BOT_CHATTER_CATEGORY_END> MissingTextsReported{};

    std::mutex ChannelCooldownsLock;
    std::unordered_map<uint32 /*zoneId*/, time_t> ChannelCooldowns;

    std::string const& GetTextVariant(uint32 textId, uint8 slot, LocaleConstant locale)
    {
        static std::string const empty;

        GossipText const* text = sObjectMgr->GetGossipText(textId);
        if (!text || slot >= MAX_GOSSIP_TEXT_OPTIONS)
            return empty;

        if (locale != DEFAULT_LOCALE)
        {
            if (NpcTextLocale const* textLocale = sObjectMgr->GetNpcTextLocale(textId))
            {
                if (textLocale->Text_0[slot].size() > std::size_t(locale) && !textLocale->Text_0[slot][locale].empty())
                    return textLocale->Text_0[slot][locale];
            }
        }

        return text->Options[slot].Text_0;
    }

    // picks a random non-empty variant among all texts of the category
    bool SelectText(BotChatterCategory category, BotChatter::ChatterText& text)
    {
        ChatterTextRange const& range = ChatterTexts[category];

        std::vector<std::pair<uint32, uint8>> variants;
        for (uint32 id = range.first; id <= range.last; ++id)
        {
            if (GossipText const* text = sObjectMgr->GetGossipText(id))
            {
                for (uint8 i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
                    if (!text->Options[i].Text_0.empty())
                        variants.emplace_back(id, i);
            }
        }

        if (variants.empty())
        {
            if (!MissingTextsReported[category].exchange(true))
                BOT_LOG_ERROR("npcbots", "BotChatter: no texts found for category {} (npc_text {}-{}), "
                    "NPCBots SQL is not up to date", uint32(category), range.first, range.last);
            return false;
        }

        std::tie(text.textId, text.slot) = Acore::Containers::SelectRandomContainerElement(variants);
        return true;
    }

    uint32 GetClassTextId(uint8 botClass)
    {
        switch (botClass)
        {
            case BOT_CLASS_WARRIOR:     return BOT_TEXT_CLASS_WARRIOR;
            case BOT_CLASS_PALADIN:     return BOT_TEXT_CLASS_PALADIN;
            case BOT_CLASS_MAGE:        return BOT_TEXT_CLASS_MAGE;
            case BOT_CLASS_PRIEST:      return BOT_TEXT_CLASS_PRIEST;
            case BOT_CLASS_WARLOCK:     return BOT_TEXT_CLASS_WARLOCK;
            case BOT_CLASS_DRUID:       return BOT_TEXT_CLASS_DRUID;
            case BOT_CLASS_DEATH_KNIGHT:return BOT_TEXT_CLASS_DEATH_KNIGHT;
            case BOT_CLASS_ROGUE:       return BOT_TEXT_CLASS_ROGUE;
            case BOT_CLASS_SHAMAN:      return BOT_TEXT_CLASS_SHAMAN;
            case BOT_CLASS_HUNTER:      return BOT_TEXT_CLASS_HUNTER;
            case BOT_CLASS_BM:          return BOT_TEXT_CLASS_BM;
            case BOT_CLASS_SPHYNX:      return BOT_TEXT_CLASS_SPHYNX;
            case BOT_CLASS_ARCHMAGE:    return BOT_TEXT_CLASS_ARCHMAGE;
            case BOT_CLASS_DREADLORD:   return BOT_TEXT_CLASS_DREADLORD;
            case BOT_CLASS_SPELLBREAKER:return BOT_TEXT_CLASS_SPELLBREAKER;
            case BOT_CLASS_DARK_RANGER: return BOT_TEXT_CLASS_DARK_RANGER;
            case BOT_CLASS_NECROMANCER: return BOT_TEXT_CLASS_NECROMANCER;
            case BOT_CLASS_SEA_WITCH:   return BOT_TEXT_CLASS_SEAWITCH;
            case BOT_CLASS_CRYPT_LORD:  return BOT_TEXT_CLASS_CRYPT_LORD;
            default:                    return 0;
        }
    }

    void ReplaceAll(std::string& text, std::string_view token, std::string_view value)
    {
        for (std::size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size()))
            text.replace(pos, token.size(), value);
    }

    // lowercase, punctuation replaced by spaces, padded with spaces for whole word matching
    std::wstring NormalizeForMatching(std::string_view text)
    {
        std::wstring wtext;
        if (!Utf8toWStr(text, wtext))
            return {};

        wstrToLower(wtext);

        static constexpr std::wstring_view separators = L".,!?;:\"()[]<>{}~*/\\-_+=#\t";
        for (wchar_t& c : wtext)
            if (separators.find(c) != std::wstring_view::npos)
                c = L' ';

        return L' ' + wtext + L' ';
    }

    bool ContainsPhrase(std::wstring const& normalizedText, std::string_view phrase)
    {
        std::wstring wphrase = NormalizeForMatching(phrase);
        if (wphrase.find_first_not_of(L' ') == std::wstring::npos)
            return false;

        return normalizedText.find(wphrase) != std::wstring::npos;
    }

    // keywords of every locale: players don't always type in their client's language
    bool MatchesKeywords(std::wstring const& normalizedText, uint32 textId)
    {
        for (uint8 loc = LOCALE_enUS; loc < TOTAL_LOCALES; ++loc)
        {
            std::string const& keywords = GetTextVariant(textId, 0, LocaleConstant(loc));
            if (loc != DEFAULT_LOCALE && keywords == GetTextVariant(textId, 0, DEFAULT_LOCALE))
                continue;

            for (std::string_view keyword : Acore::Tokenize(keywords, '|', false))
                if (ContainsPhrase(normalizedText, keyword))
                    return true;
        }

        return false;
    }

    // greeting / farewell said by a player, and the fitting answer
    bool GetReplyCategory(std::wstring const& normalizedText, BotChatterCategory& category)
    {
        if (MatchesKeywords(normalizedText, BOT_TEXT_CHATTER_KEYWORDS_GREET))
            category = BOT_CHATTER_GREET_REPLY;
        else if (MatchesKeywords(normalizedText, BOT_TEXT_CHATTER_KEYWORDS_BYE))
            category = BOT_CHATTER_BYE_REPLY;
        else
            return false;

        return true;
    }

    constexpr std::size_t CHATTER_AI_HISTORY_SIZE = 8;
    constexpr time_t CHATTER_AI_HISTORY_EXPIRE = 15 * MINUTE;
    constexpr std::size_t CHATTER_AI_MAX_TEXT_LENGTH = 240;

    // world and map threads (emotes)
    std::mutex AIPlayerCooldownsLock;
    std::unordered_map<ObjectGuid, time_t> AIPlayerCooldowns;

    // emotes an OpenAI answer may contain, as typed by players
    struct ChatterEmote
    {
        std::string_view name;
        uint32 textEmote;
    };

    constexpr std::array ChatterEmotes =
    {
        ChatterEmote{ "agree", TEXT_EMOTE_AGREE },          ChatterEmote{ "angry", TEXT_EMOTE_ANGRY },
        ChatterEmote{ "applaud", TEXT_EMOTE_APPLAUD },      ChatterEmote{ "beg", TEXT_EMOTE_BEG },
        ChatterEmote{ "blush", TEXT_EMOTE_BLUSH },          ChatterEmote{ "bored", TEXT_EMOTE_BORED },
        ChatterEmote{ "bow", TEXT_EMOTE_BOW },              ChatterEmote{ "bye", TEXT_EMOTE_BYE },
        ChatterEmote{ "cheer", TEXT_EMOTE_CHEER },          ChatterEmote{ "chicken", TEXT_EMOTE_CHICKEN },
        ChatterEmote{ "chuckle", TEXT_EMOTE_CHUCKLE },      ChatterEmote{ "confused", TEXT_EMOTE_CONFUSED },
        ChatterEmote{ "congratulate", TEXT_EMOTE_CONGRATULATE }, ChatterEmote{ "cower", TEXT_EMOTE_COWER },
        ChatterEmote{ "cry", TEXT_EMOTE_CRY },              ChatterEmote{ "dance", TEXT_EMOTE_DANCE },
        ChatterEmote{ "facepalm", TEXT_EMOTE_FACEPALM },    ChatterEmote{ "flex", TEXT_EMOTE_FLEX },
        ChatterEmote{ "flirt", TEXT_EMOTE_FLIRT },          ChatterEmote{ "gasp", TEXT_EMOTE_GASP },
        ChatterEmote{ "giggle", TEXT_EMOTE_GIGGLE },        ChatterEmote{ "greet", TEXT_EMOTE_GREET },
        ChatterEmote{ "grin", TEXT_EMOTE_GRIN },            ChatterEmote{ "hello", TEXT_EMOTE_HELLO },
        ChatterEmote{ "hug", TEXT_EMOTE_HUG },              ChatterEmote{ "kiss", TEXT_EMOTE_KISS },
        ChatterEmote{ "laugh", TEXT_EMOTE_LAUGH },          ChatterEmote{ "love", TEXT_EMOTE_LOVE },
        ChatterEmote{ "no", TEXT_EMOTE_NO },                ChatterEmote{ "nod", TEXT_EMOTE_NOD },
        ChatterEmote{ "point", TEXT_EMOTE_POINT },          ChatterEmote{ "pray", TEXT_EMOTE_PRAY },
        ChatterEmote{ "roar", TEXT_EMOTE_ROAR },            ChatterEmote{ "rofl", TEXT_EMOTE_ROFL },
        ChatterEmote{ "rude", TEXT_EMOTE_RUDE },            ChatterEmote{ "salute", TEXT_EMOTE_SALUTE },
        ChatterEmote{ "shrug", TEXT_EMOTE_SHRUG },          ChatterEmote{ "shy", TEXT_EMOTE_SHY },
        ChatterEmote{ "sigh", TEXT_EMOTE_SIGH },            ChatterEmote{ "sleep", TEXT_EMOTE_SLEEP },
        ChatterEmote{ "smile", TEXT_EMOTE_SMILE },          ChatterEmote{ "surprised", TEXT_EMOTE_SURPRISED },
        ChatterEmote{ "talk", TEXT_EMOTE_TALK },            ChatterEmote{ "thank", TEXT_EMOTE_THANK },
        ChatterEmote{ "threaten", TEXT_EMOTE_THREATEN },    ChatterEmote{ "victory", TEXT_EMOTE_VICTORY },
        ChatterEmote{ "wave", TEXT_EMOTE_WAVE },            ChatterEmote{ "wink", TEXT_EMOTE_WINK }
    };

    uint32 FindChatterEmote(std::string_view name)
    {
        auto itr = std::ranges::find(ChatterEmotes, name, &ChatterEmote::name);
        if (itr == ChatterEmotes.end() || !sEmotesTextStore.LookupEntry(itr->textEmote))
            return 0;
        return itr->textEmote;
    }

    std::string_view GetChatterEmoteName(uint32 textEmote)
    {
        auto itr = std::ranges::find(ChatterEmotes, textEmote, &ChatterEmote::textEmote);
        return itr != ChatterEmotes.end() ? itr->name : std::string_view{};
    }

    // emotes the bots handle as commands when aimed at them, see bot_ai::ReceiveEmote()
    bool IsBotCommandEmote(uint32 textEmote)
    {
        switch (textEmote)
        {
            case TEXT_EMOTE_BONK:
            case TEXT_EMOTE_SALUTE:
            case TEXT_EMOTE_STAND:
            case TEXT_EMOTE_WAVE:
            case TEXT_EMOTE_TICKLE:
                return true;
            default:
                return false;
        }
    }

    // fitting answer to a player's emote without OpenAI, 0 for none
    uint32 SelectEmoteReaction(uint32 textEmote, bool atMe)
    {
        std::vector<uint32> reactions;
        switch (textEmote)
        {
            case TEXT_EMOTE_WAVE:
            case TEXT_EMOTE_HELLO:      reactions = { TEXT_EMOTE_WAVE, TEXT_EMOTE_HELLO };          break;
            case TEXT_EMOTE_GREET:
            case TEXT_EMOTE_BOW:        reactions = { TEXT_EMOTE_BOW, TEXT_EMOTE_GREET };           break;
            case TEXT_EMOTE_BYE:        reactions = { TEXT_EMOTE_BYE, TEXT_EMOTE_WAVE };            break;
            case TEXT_EMOTE_CHEER:
            case TEXT_EMOTE_VICTORY:    reactions = { TEXT_EMOTE_CHEER, TEXT_EMOTE_APPLAUD };       break;
            case TEXT_EMOTE_DANCE:      reactions = { TEXT_EMOTE_DANCE, TEXT_EMOTE_APPLAUD };       break;
            case TEXT_EMOTE_FLEX:       reactions = { TEXT_EMOTE_APPLAUD, TEXT_EMOTE_LAUGH };       break;
            case TEXT_EMOTE_LAUGH:
            case TEXT_EMOTE_ROFL:
            case TEXT_EMOTE_CHUCKLE:
            case TEXT_EMOTE_GIGGLE:     reactions = { TEXT_EMOTE_LAUGH, TEXT_EMOTE_GIGGLE };        break;
            case TEXT_EMOTE_KISS:
            case TEXT_EMOTE_LOVE:
            case TEXT_EMOTE_FLIRT:      reactions = { TEXT_EMOTE_BLUSH, TEXT_EMOTE_GIGGLE };        break;
            case TEXT_EMOTE_HUG:        reactions = { TEXT_EMOTE_HUG, TEXT_EMOTE_SMILE };           break;
            case TEXT_EMOTE_THANK:
            case TEXT_EMOTE_APPLAUD:
            case TEXT_EMOTE_CONGRATULATE: reactions = { TEXT_EMOTE_BOW, TEXT_EMOTE_SMILE };         break;
            case TEXT_EMOTE_SALUTE:     reactions = { TEXT_EMOTE_SALUTE };                          break;
            case TEXT_EMOTE_CRY:
            case TEXT_EMOTE_SIGH:       reactions = { TEXT_EMOTE_HUG, TEXT_EMOTE_SIGH };            break;
            case TEXT_EMOTE_RUDE:
            case TEXT_EMOTE_CHICKEN:    reactions = { TEXT_EMOTE_RUDE, TEXT_EMOTE_ANGRY };          break;
            case TEXT_EMOTE_THREATEN:
            case TEXT_EMOTE_ROAR:
            case TEXT_EMOTE_ANGRY:      reactions = { TEXT_EMOTE_ROAR, TEXT_EMOTE_COWER };          break;
            case TEXT_EMOTE_POINT:
            case TEXT_EMOTE_CONFUSED:   reactions = { TEXT_EMOTE_SHRUG, TEXT_EMOTE_CONFUSED };      break;
            case TEXT_EMOTE_SMILE:
            case TEXT_EMOTE_GRIN:
            case TEXT_EMOTE_WINK:       reactions = { TEXT_EMOTE_SMILE, TEXT_EMOTE_WINK };          break;
            default:
                if (atMe)
                    reactions = { TEXT_EMOTE_SHRUG, TEXT_EMOTE_CONFUSED, TEXT_EMOTE_SMILE };
                break;
        }

        std::erase_if(reactions, [](uint32 id) { return !sEmotesTextStore.LookupEntry(id); });
        return reactions.empty() ? 0 : Acore::Containers::SelectRandomContainerElement(reactions);
    }

    // SMSG_TEXT_EMOTE, the client builds the localized emote text itself
    class EmoteBuilder
    {
    public:
        EmoteBuilder(Creature const* source, uint32 textEmote, Unit const* target)
            : _source(source), _textEmote(textEmote), _target(target) { }

        void operator()(WorldPacket& data, LocaleConstant locale) const
        {
            std::string const name(_target ? _target->GetNameForLocaleIdx(locale) : "");

            data.Initialize(SMSG_TEXT_EMOTE, 20 + name.size());
            data << _source->GetGUID();
            data << uint32(_textEmote);
            data << uint32(0);
            data << uint32(name.size());
            if (name.size() > 1)
                data << name;
            else
                data << uint8(0);
        }

    private:
        Creature const* _source;
        uint32 _textEmote;
        Unit const* _target;
    };

    // recent party/raid and General channel messages, context for OpenAI answers
    struct ChatLogLine
    {
        std::string speaker;
        std::string text;
        time_t time;
    };

    constexpr time_t CHATTER_CHAT_LOG_EXPIRE = 30 * MINUTE;

    std::mutex ChatLogsLock;
    std::unordered_map<uint64, std::deque<ChatLogLine>> ChatLogs;

    uint64 GroupChatLogKey(Group const* group)
    {
        return (uint64(1) << 32) | group->GetGUID().GetCounter();
    }

    uint64 ChannelChatLogKey(uint32 zoneId, TeamId team)
    {
        return (uint64(2) << 32) | (uint64(zoneId) << 2) | uint64(team);
    }

    void LogChatLine(uint64 key, std::string_view speaker, std::string_view text)
    {
        const uint32 size = BotCfg::GetBotOpenAIChatContextSize();
        if (!size || text.empty() || !BotOpenAI::IsEnabled())
            return;

        time_t now = GameTime::GetGameTime().count();

        std::lock_guard<std::mutex> lock(ChatLogsLock);
        std::erase_if(ChatLogs, [now](auto const& log) {
            return log.second.empty() || log.second.back().time + CHATTER_CHAT_LOG_EXPIRE <= now;
        });

        std::deque<ChatLogLine>& log = ChatLogs[key];
        log.push_back({ std::string(speaker), std::string(text), now });
        while (log.size() > size)
            log.pop_front();
    }

    std::string GetChatLogContext(uint64 key)
    {
        time_t now = GameTime::GetGameTime().count();
        std::ostringstream ss;

        std::lock_guard<std::mutex> lock(ChatLogsLock);
        auto itr = ChatLogs.find(key);
        if (itr != ChatLogs.end())
            for (ChatLogLine const& line : itr->second)
                if (line.time + CHATTER_CHAT_LOG_EXPIRE > now)
                    ss << '[' << line.speaker << "]: " << line.text << '\n';

        return ss.str();
    }

    // one chat line: no line breaks, no chat escape sequences, cut at a character boundary
    std::string SanitizeAIText(std::string const& text)
    {
        std::wstring wtext;
        if (!Utf8toWStr(text, wtext))
            return {};

        for (wchar_t& c : wtext)
        {
            if (c == L'\n' || c == L'\r' || c == L'\t')
                c = L' ';
            else if (c == L'|')
                c = L'/';
        }

        std::size_t first = wtext.find_first_not_of(L" \"");
        std::size_t last = wtext.find_last_not_of(L" \"");
        if (first == std::wstring::npos)
            return {};
        wtext = wtext.substr(first, last - first + 1);

        std::string result;
        while (!wtext.empty())
        {
            if (!WStrToUtf8(wtext, result))
                return {};
            if (result.size() <= CHATTER_AI_MAX_TEXT_LENGTH)
                break;
            wtext.resize(wtext.size() - 1);
        }
        return result;
    }

    std::string GetEnglishClassName(uint8 botClass)
    {
        uint32 textId = GetClassTextId(botClass);
        return textId ? GetTextVariant(textId, 0, DEFAULT_LOCALE) : "adventurer";
    }

    std::string GetEnglishRaceName(uint8 race)
    {
        ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race);
        return entry ? entry->name[sWorld->GetDefaultDbcLocale()] : "";
    }

    std::string GetEnglishZoneName(uint32 zoneId)
    {
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
        return zone ? zone->area_name[sWorld->GetDefaultDbcLocale()] : "";
    }

    struct NearbyBotCheck
    {
        NearbyBotCheck(WorldObject const* obj, float range) : _obj(obj), _range(range) { }

        bool operator()(Creature* creature) const
        {
            return creature->IsNPCBot() && creature->IsAlive() && _obj->IsWithinDistInMap(creature, _range);
        }

    private:
        WorldObject const* _obj;
        float _range;
    };
}

BotChatter::BotChatter(bot_ai* ai, Creature* bot) : _ai(ai), _me(bot),
    _greetTimer(urand(2000, 5000)), _idleTimer(urand(30000, 90000)),
    _replyCategory(BOT_CHATTER_GREET_REPLY), _replyMode(BOT_CHATTER_REPLY_SAY), _replyTimer(0), _aiPendingUntil(0),
    _stateEmote(0), _stateEmoteTimer(0), _emoteReaction(0), _emoteTimer(0)
{
}

void BotChatter::TextBuilder::operator()(WorldPacket& data, LocaleConstant locale) const
{
    std::string text = _text.textId ? _chatter.FormatText(_text.textId, _text.slot, locale, _subject) : _text.raw;

    switch (_msgType)
    {
        case CHAT_MSG_CHANNEL:
        case CHAT_MSG_PARTY:
        case CHAT_MSG_RAID:
        case CHAT_MSG_WHISPER:
        {
            // player chat types carry no sender name and the client does not query names of creature guids,
            // the gm variant of the packet carries the name (no gm tag is set)
            std::string const& name = _chatter._me->GetNameForLocaleIdx(locale);
            bool whisper = _msgType == CHAT_MSG_WHISPER && _subject;
            ObjectGuid receiver = whisper ? _subject->GetGUID() : ObjectGuid::Empty;
            ChatHandler::BuildChatPacket(data, _msgType, LANG_UNIVERSAL, _chatter._me->GetGUID(), receiver,
                text, 0, name, "", 0, true, _channelName);
            break;
        }
        default:
        {
            WorldObject const* receiver = (_subject && _subject->IsPlayer()) ? _subject : nullptr;
            ChatHandler::BuildChatPacket(data, _msgType, LANG_UNIVERSAL, _chatter._me, receiver, text, 0, "", locale);
            break;
        }
    }
}

void BotChatter::Update(uint32 diff)
{
    if (_stateEmoteTimer)
    {
        if (_stateEmoteTimer <= diff || _me->IsInCombat() || !_me->IsAlive())
            EndStateEmote();
        else
            _stateEmoteTimer -= diff;
    }

    if (_emoteTimer)
    {
        if (_emoteTimer <= diff)
        {
            _emoteTimer = 0;
            if (CanChat(false, true))
                PerformEmote(_emoteReaction, ObjectAccessor::GetPlayer(*_me, _emoteTarget));
            _emoteTarget.Clear();
        }
        else
            _emoteTimer -= diff;
    }

    if (_replyTimer)
    {
        if (_replyTimer <= diff)
        {
            _replyTimer = 0;
            SendPendingReply();
        }
        else
            _replyTimer -= diff;
    }

    if (_greetTimer > diff)
        _greetTimer -= diff;
    else
    {
        _greetTimer = urand(2000, 4000);
        if (CanChat(true))
            TryGreetNearbyPlayer();
    }

    if (_idleTimer > diff)
        _idleTimer -= diff;
    else
    {
        uint32 interval = urand(BotCfg::GetBotChatterIdleIntervalMin(), BotCfg::GetBotChatterIdleIntervalMax());
        _idleTimer = interval * IN_MILLISECONDS;
        if (CanChat(true) && roll_chance_i(BotCfg::GetBotChatterIdleChance()))
            Chatter(BOT_CHATTER_IDLE, nullptr, true);
    }
}

void BotChatter::OnKilledUnit(Unit const* victim)
{
    if (!CanChat(true, true))
        return;

    BotChatterCategory category;
    int32 chance = BotCfg::GetBotChatterEventChance();

    if (victim->IsPlayer() || victim->IsNPCBot())
        category = BOT_CHATTER_KILL_PLAYER;
    else if (Creature const* creature = victim->ToCreature())
    {
        if (creature->IsPet() || creature->IsTotem() || creature->IsCritter() || creature->IsNPCBotPet())
            return;

        if (creature->isWorldBoss() || creature->IsDungeonBoss())
            category = BOT_CHATTER_KILL_BOSS;
        else
        {
            switch (creature->GetCreatureTemplate()->rank)
            {
                case CREATURE_ELITE_RARE:
                case CREATURE_ELITE_RAREELITE:
                    category = BOT_CHATTER_KILL_RARE;
                    break;
                case CREATURE_ELITE_ELITE:
                    // every trash mob in a dungeon is elite
                    category = creature->GetMap()->Instanceable() ? BOT_CHATTER_KILL_NORMAL : BOT_CHATTER_KILL_ELITE;
                    break;
                default:
                    category = BOT_CHATTER_KILL_NORMAL;
                    break;
            }
        }

        if (category == BOT_CHATTER_KILL_NORMAL)
        {
            if (creature->GetLevel() + CHATTER_TRIVIAL_KILL_LEVEL_DIFF < _me->GetLevel())
                return;
            chance /= 5;
        }
    }
    else
        return;

    if (!roll_chance_i(chance))
        return;

    // routine kills are only worth mentioning to those around
    Chatter(category, victim, category != BOT_CHATTER_KILL_NORMAL);
}

bool BotChatter::SayText(uint32 textId, uint8 slot)
{
    ChatterText text;
    text.textId = textId;
    text.slot = slot;
    return SayNearby(text, nullptr, CHAT_MSG_MONSTER_SAY);
}

void BotChatter::DoTextEmote(uint32 textEmote)
{
    PerformEmote(textEmote, nullptr);
}

void BotChatter::OnLevelUp()
{
    if (CanChat(true, true) && roll_chance_i(BotCfg::GetBotChatterEventChance()))
        Chatter(BOT_CHATTER_LEVEL_UP, nullptr, true);
}

void BotChatter::OnPlayerSay(Player const* player, std::string_view message)
{
    if (!BotCfg::IsBotChatterEnabled() || !player->IsInWorld())
        return;

    float range = sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY);
    std::list<Creature*> found;
    NearbyBotCheck check(player, range);
    Acore::CreatureListSearcher<NearbyBotCheck> searcher(player, found, check);
    Cell::VisitObjects(player, searcher, range);

    HandlePlayerMessage(player, message, BOT_CHATTER_REPLY_SAY, { found.begin(), found.end() });
}

void BotChatter::OnPlayerGroupChat(Player const* player, Group* group, ChatMsg msgType, std::string_view message)
{
    if (!BotCfg::IsBotChatterEnabled() || !player->IsInWorld() || !group)
        return;

    // bots of every group member who are in this group
    std::vector<Creature*> bots;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player const* member = itr->GetSource();
        if (!member || !member->GetBotMgr())
            continue;

        for (auto const& [guid, bot] : *member->GetBotMgr()->GetBotMap())
            if (bot && bot->IsInWorld() && bot->IsAlive() && bot->GetBotAI() && bot->GetBotAI()->GetGroup() == group)
                bots.push_back(bot);
    }

    bool raid = msgType == CHAT_MSG_RAID || msgType == CHAT_MSG_RAID_LEADER;
    HandlePlayerMessage(player, message, raid ? BOT_CHATTER_REPLY_RAID : BOT_CHATTER_REPLY_PARTY, bots);

    LogChatLine(GroupChatLogKey(group), player->GetName(), message);
}

void BotChatter::OnPlayerChannelChat(Player const* player, Channel const* channel, std::string_view message)
{
    if (!BotCfg::IsBotChatterEnabled() || !BotCfg::IsBotChatterChannelEnabled() || !player->IsInWorld())
        return;
    if (channel->GetChannelId() != CHATTER_CHANNEL_GENERAL || player->GetMap()->Instanceable())
        return;

    // bots around the player, plus the player's own bots anywhere in the zone
    std::list<Creature*> found;
    NearbyBotCheck check(player, CHATTER_CHANNEL_REPLY_RANGE);
    Acore::CreatureListSearcher<NearbyBotCheck> searcher(player, found, check);
    Cell::VisitObjects(player, searcher, CHATTER_CHANNEL_REPLY_RANGE);

    std::vector<Creature*> bots(found.begin(), found.end());
    if (BotMgr const* mgr = player->GetBotMgr())
    {
        for (auto const& [guid, bot] : *mgr->GetBotMap())
        {
            if (bot && bot->IsInWorld() && bot->IsAlive() && bot->GetMap() == player->GetMap() &&
                bot->GetZoneId() == player->GetZoneId() && std::ranges::find(bots, bot) == bots.end())
                bots.push_back(bot);
        }
    }

    HandlePlayerMessage(player, message, BOT_CHATTER_REPLY_CHANNEL, bots);

    LogChatLine(ChannelChatLogKey(player->GetZoneId(), player->GetTeamId()), player->GetName(), message);
}

bool BotChatter::OnPlayerWhisper(Player* player, std::string const& botName, std::string_view message)
{
    if (!BotCfg::IsBotChatterEnabled() || !BotDataMgr::AllBotsLoaded())
        return false;

    LocaleConstant locale = player->GetSession()->GetSessionDbLocaleIndex();
    Creature const* bot = BotDataMgr::FindBotByNameFor(botName, player);

    // hidden or despawned bots are "offline"
    if (!bot || !bot->IsInWorld() || !bot->GetBotAI())
        return false;

    if (bot->IsHostileTo(player))
    {
        player->GetSession()->SendWrongFactionNotice();
        return true;
    }

    // echo "To [bot]: ..." to the sender
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_INFORM, LANG_UNIVERSAL, bot->GetGUID(), player->GetGUID(),
        message, 0, bot->GetNameForLocaleIdx(locale), "", 0, true);
    player->SendDirectMessage(&data);

    bot->GetBotAI()->GetChatter().ReplyToWhisper(player, message);
    return true;
}

void BotChatter::ReplyToWhisper(Player* player, std::string_view message)
{
    // whispers work across maps: the bot answers right away, without waiting for its (possibly idle) map update
    if (!_me->IsAlive() || _ai->IsDuringTeleport())
        return;

    if (_ai->GetActivity().IsTellingStoryTo(player->GetGUID()))
    {
        _ai->GetActivity().OnStoryComment(message);
        return;
    }

    if (!_me->IsInCombat() && !IsAIPending() && RequestAIReply(player, message, BOT_CHATTER_REPLY_WHISPER))
        return;

    BotChatterCategory category = BOT_CHATTER_WHISPER_REPLY;
    if (_me->IsInCombat())
        category = BOT_CHATTER_WHISPER_BUSY;
    else
    {
        std::wstring normalized = NormalizeForMatching(message);
        if (GetReplyCategory(normalized, category) && category == BOT_CHATTER_GREET_REPLY)
            _greeted[player->GetGUID()] = GameTime::GetGameTime().count();
    }

    ChatterText text;
    if (SelectText(category, text))
        WhisperTo(text, player);
}

void BotChatter::OnPlayerTextEmote(Player* player, uint32 textEmote, Unit const* target)
{
    if (!BotCfg::IsBotChatterEnabled() || !player->IsInWorld())
        return;

    Creature const* targetBot = (target && target->IsNPCBot()) ? target->ToCreature() : nullptr;
    // commands for the bot, see bot_ai::ReceiveEmote()
    if (targetBot && IsBotCommandEmote(textEmote))
        return;

    float range = sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE);
    std::list<Creature*> found;
    NearbyBotCheck check(player, range);
    Acore::CreatureListSearcher<NearbyBotCheck> searcher(player, found, check);
    Cell::VisitObjects(player, searcher, range);

    // the bot the emote is aimed at, otherwise one of the player's own bots or with a chance a random one
    BotChatter* responder = nullptr;
    std::vector<BotChatter*> own;
    std::vector<BotChatter*> others;
    for (Creature* bot : found)
    {
        bot_ai* ai = bot->GetBotAI();
        if (!ai)
            continue;

        BotChatter& chatter = ai->GetChatter();
        bool atMe = bot == targetBot;
        if (chatter._emoteTimer || !chatter.CanReplyTo(player, atMe))
            continue;

        if (atMe)
        {
            responder = &chatter;
            break;
        }
        (chatter.IsOwnedBy(player) ? own : others).push_back(&chatter);
    }

    if (targetBot && !responder)
        return;

    if (!responder && !own.empty())
        responder = Acore::Containers::SelectRandomContainerElement(own);
    if (!responder && !others.empty() && roll_chance_i(BotCfg::GetBotChatterReplyChance()))
        responder = Acore::Containers::SelectRandomContainerElement(others);

    if (responder)
        responder->ReactToEmote(player, textEmote, targetBot != nullptr);
}

void BotChatter::ReactToEmote(Player* player, uint32 textEmote, bool atMe)
{
    // OpenAI gets the emote as a message and may answer with words, an emote or both
    if (BotOpenAI::IsEnabled())
    {
        std::string_view name = GetChatterEmoteName(textEmote);
        std::string action = "*" + player->GetName() + (name.empty() ? " does an emote" : " /" + std::string(name)) +
            (atMe ? " at you*" : "*");
        if (RequestAIReply(player, action, BOT_CHATTER_REPLY_SAY))
            return;
    }

    if (uint32 reaction = SelectEmoteReaction(textEmote, atMe))
    {
        _emoteReaction = reaction;
        _emoteTarget = player->GetGUID();
        _emoteTimer = urand(800, 2000);
    }
}

void BotChatter::HandlePlayerMessage(Player const* player, std::string_view message, BotChatterReplyMode mode,
    std::vector<Creature*> const& bots)
{
    if (bots.empty() || message.empty())
        return;

    std::wstring normalized = NormalizeForMatching(message);
    if (normalized.find_first_not_of(L' ') == std::wstring::npos)
        return;

    // with OpenAI any message can be answered, otherwise only greetings and farewells
    const bool ai = BotOpenAI::IsEnabled();
    BotChatterCategory category;
    const bool canned = GetReplyCategory(normalized, category);
    if (!ai && !canned)
        return;

    // a bot telling this player a story takes the remark into it
    for (Creature* bot : bots)
    {
        if (bot->GetBotAI() && bot->GetBotAI()->GetActivity().IsTellingStoryTo(player->GetGUID()))
        {
            bot->GetBotAI()->GetActivity().OnStoryComment(message);
            return;
        }
    }

    LocaleConstant locale = player->GetSession()->GetSessionDbLocaleIndex();

    std::vector<BotChatter*> addressed;
    std::vector<BotChatter*> own;
    std::vector<BotChatter*> others;
    for (Creature* bot : bots)
    {
        bot_ai* ai = bot->GetBotAI();
        if (!ai)
            continue;

        bool isAddressed = player->GetTarget() == bot->GetGUID() ||
            ContainsPhrase(normalized, bot->GetName()) || ContainsPhrase(normalized, bot->GetNameForLocaleIdx(locale));

        BotChatter& chatter = ai->GetChatter();
        if (!chatter.CanReplyTo(player, isAddressed))
            continue;

        if (isAddressed)
            addressed.push_back(&chatter);
        else if (chatter.IsOwnedBy(player))
            own.push_back(&chatter);
        else
            others.push_back(&chatter);
    }

    // one bot answers through OpenAI: the addressed one, one of the player's own bots or,
    // with a chance, a random one. Canned answers if that fails
    if (ai)
    {
        BotChatter* responder = nullptr;
        if (!addressed.empty())
            responder = Acore::Containers::SelectRandomContainerElement(addressed);
        else if (!own.empty())
            responder = Acore::Containers::SelectRandomContainerElement(own);
        else if (!others.empty() && roll_chance_i(BotCfg::GetBotChatterReplyChance()))
            responder = Acore::Containers::SelectRandomContainerElement(others);

        if (responder && responder->RequestAIReply(player, message, mode))
            return;
        if (!canned)
            return;
    }

    // nobody in particular was addressed: one of the player's own bots always answers,
    // a couple of other random bots may answer too
    if (addressed.empty())
    {
        Acore::Containers::RandomShuffle(own);
        Acore::Containers::RandomShuffle(others);
        if (!own.empty())
        {
            addressed.push_back(own.front());
            own.erase(own.begin());
        }

        others.insert(others.begin(), own.begin(), own.end());
        for (BotChatter* chatter : others)
        {
            if (addressed.size() >= CHATTER_MAX_REPLIES)
                break;
            if (roll_chance_i(BotCfg::GetBotChatterReplyChance()))
                addressed.push_back(chatter);
        }
    }

    uint32 delay = urand(1000, 2500);
    for (BotChatter* chatter : addressed)
    {
        chatter->QueueReply(player, category, mode, delay);
        delay += urand(1000, 2500);
    }
}

bool BotChatter::RequestAIReply(Player const* player, std::string_view message, BotChatterReplyMode mode)
{
    if (!BotOpenAI::IsEnabled())
        return false;

    time_t now = GameTime::GetGameTime().count();

    // per player cooldown, against spam and cost
    {
        std::lock_guard<std::mutex> lock(AIPlayerCooldownsLock);
        auto cooldown = AIPlayerCooldowns.find(player->GetGUID());
        if (cooldown != AIPlayerCooldowns.end() && cooldown->second > now)
            return false;
    }

    std::erase_if(_aiHistory, [now](auto const& history) {
        return history.second.lastUse + CHATTER_AI_HISTORY_EXPIRE <= now;
    });
    AIHistory& history = _aiHistory[player->GetGUID()];

    BotAIRequest request;
    request.botEntry = _me->GetEntry();
    request.playerGuid = player->GetGUID();
    request.replyMode = uint8(mode);
    request.instructions = BuildAIInstructions(player, mode);

    // group and channel answers get the recent chat instead of the conversation with this player
    std::string context;
    bool chatLog = BotCfg::GetBotOpenAIChatContextSize() > 0;
    if (chatLog && (mode == BOT_CHATTER_REPLY_PARTY || mode == BOT_CHATTER_REPLY_RAID))
    {
        if (Group const* group = _ai->GetGroup())
            context = GetChatLogContext(GroupChatLogKey(group));
    }
    else if (chatLog && mode == BOT_CHATTER_REPLY_CHANNEL)
        context = GetChatLogContext(ChannelChatLogKey(player->GetZoneId(), player->GetTeamId()));
    else
        request.input.assign(history.messages.begin(), history.messages.end());

    if (!context.empty())
        request.instructions += "\n\nRecent messages in this chat, oldest first:\n" + context;

    request.input.push_back({ false, std::string(message) });
    request.allowActivity = true;

    if (BotCfg::IsBotOpenAIEmotesEnabled())
        for (ChatterEmote const& emote : ChatterEmotes)
            if (sEmotesTextStore.LookupEntry(emote.textEmote))
                request.emotes.emplace_back(emote.name);

    if (!BotOpenAI::Enqueue(std::move(request)))
        return false;

    {
        std::lock_guard<std::mutex> lock(AIPlayerCooldownsLock);
        std::erase_if(AIPlayerCooldowns, [now](auto const& entry) { return entry.second <= now; });
        AIPlayerCooldowns[player->GetGUID()] = now + BotCfg::GetBotOpenAIPlayerCooldown();
    }

    history.lastUse = now;
    history.messages.push_back({ false, std::string(message) });
    while (history.messages.size() > CHATTER_AI_HISTORY_SIZE)
        history.messages.pop_front();

    // a lost request does not block the bot forever
    _aiPendingUntil = now + BotCfg::GetBotOpenAITimeout() + MINUTE;
    return true;
}

bool BotChatter::IsAIPending() const
{
    return _aiPendingUntil > GameTime::GetGameTime().count();
}

void BotChatter::ProcessAIReplies()
{
    BotAIResult result;
    while (BotOpenAI::PollResult(result))
    {
        Creature const* bot = BotDataMgr::FindBot(result.botEntry);
        if (bot && bot->GetBotAI())
            bot->GetBotAI()->GetChatter().OnAIReply(result);
    }
}

void BotChatter::OnAIReply(BotAIResult const& result)
{
    ChatterText text;
    text.raw = SanitizeAIText(result.text);
    uint32 textEmote = FindChatterEmote(result.emote);

    // the next sentence of a story told to the player
    if (result.replyMode == BOT_CHATTER_REPLY_STORY)
    {
        BotActivity& activity = _ai->GetActivity();
        if (!activity.IsTellingStoryTo(result.playerGuid))
            return;

        Player* player = ObjectAccessor::FindConnectedPlayer(result.playerGuid);
        if (textEmote)
            PerformEmote(textEmote, player);
        if (!text.raw.empty() && player)
            SayNearby(text, player, CHAT_MSG_MONSTER_SAY);

        activity.OnStoryLine(text.raw, result.storyEnd);
        return;
    }

    _aiPendingUntil = 0;

    ApplyAIActivity(result.activity, ObjectAccessor::FindConnectedPlayer(result.playerGuid));

    if (text.raw.empty() && !textEmote)
        return;

    auto itr = _aiHistory.find(result.playerGuid);
    if (itr != _aiHistory.end())
    {
        std::string line = textEmote ? '*' + result.emote + '*' : "";
        if (!text.raw.empty())
            line += (line.empty() ? "" : " ") + text.raw;

        itr->second.messages.push_back({ true, std::move(line) });
        while (itr->second.messages.size() > CHATTER_AI_HISTORY_SIZE)
            itr->second.messages.pop_front();
    }

    if (textEmote)
        PerformEmote(textEmote, ObjectAccessor::FindConnectedPlayer(result.playerGuid));

    if (!text.raw.empty())
        DeliverReply(text, result.playerGuid, BotChatterReplyMode(result.replyMode));
}

// OpenAI decided the bot should change what it is doing
void BotChatter::ApplyAIActivity(std::string const& activity, Player const* player)
{
    BotActivity& botActivity = _ai->GetActivity();
    if (activity == "active")
        botActivity.Stop();
    else if (activity == "rest")
        botActivity.RequestRest(player);
    else if (activity == "story" && player)
        botActivity.StartStoryWith(player);
}

bool BotChatter::RequestStoryStep(Player const* player)
{
    if (!BotOpenAI::IsEnabled())
        return false;

    BotActivity const& activity = _ai->GetActivity();

    std::ostringstream ss;
    ss << BuildAIInstructions(player, BOT_CHATTER_REPLY_STORY)
        << "\n\nYou are telling " << player->GetName() << " a story you make up, fitting your character and the "
        << "world of Warcraft. The story has about " << uint32(activity.GetStoryLength()) << " sentences, you told "
        << uint32(activity.GetStoryLinesTold()) << " so far. Answer with the next single sentence of the story only. "
        << "If " << player->GetName() << " said something since your last sentence, react to it briefly in the same "
        << "answer and let it shape the story. With the last sentence finish the story, set story_end and ";
    if (IsOwnedBy(player))
        ss << "suggest to " << player->GetName() << " that you move on together.";
    else
        ss << "say goodbye to " << player->GetName() << ".";

    BotAIRequest request;
    request.botEntry = _me->GetEntry();
    request.playerGuid = player->GetGUID();
    request.replyMode = uint8(BOT_CHATTER_REPLY_STORY);
    request.instructions = ss.str();
    request.input = activity.GetStoryTranscript();
    if (request.input.empty() || request.input.back().fromBot)
        request.input.push_back({ false, request.input.empty() ? "(Begin the story.)" : "(Go on.)" });
    request.storyStep = true;

    if (BotCfg::IsBotOpenAIEmotesEnabled())
        for (ChatterEmote const& emote : ChatterEmotes)
            if (sEmotesTextStore.LookupEntry(emote.textEmote))
                request.emotes.emplace_back(emote.name);

    return BotOpenAI::Enqueue(std::move(request));
}

std::string BotChatter::BuildAIInstructions(Player const* player, BotChatterReplyMode mode) const
{
    std::ostringstream ss;
    ss << "You are " << _me->GetName() << ", a level " << uint32(_me->GetLevel()) << ' '
        << GetEnglishRaceName(_ai->GetPlayerRace()) << ' ' << GetEnglishClassName(_ai->GetBotClass())
        << " in World of Warcraft: Wrath of the Lich King, currently in " << GetEnglishZoneName(_me->GetZoneId())
        << ". ";

    std::string const& guild = BotDataMgr::GetBotGuildName(_me->GetEntry());
    if (!guild.empty())
        ss << "You are a member of the guild <" << guild << ">. ";

    if (_ai->IAmFree())
        ss << (_ai->IsWanderer() ? "You are an adventurer travelling the world on your own. " :
            "You are a mercenary waiting to be hired. ");

    if (_ai->GetActivity().IsResting())
        ss << "Right now you are resting, sitting on the ground. ";
    else if (_ai->GetActivity().IsInRoleplay())
        ss << "Right now you sit with other adventurers around a campfire, telling each other stories. ";
    else if (_ai->GetActivity().IsTellingStory())
        ss << "Right now you sit at a campfire, telling a story. ";
    else if (Player const* owner = _ai->GetBotOwner())
        ss << "You are a hired companion of " << owner->GetName()
            << (owner == player ? ", the one talking to you" : "") << ". ";

    ss << player->GetName() << ", a level " << uint32(player->GetLevel()) << ' '
        << GetEnglishRaceName(player->GetRace()) << ' ' << GetEnglishClassName(player->GetClass()) << ", talks to you ";
    switch (mode)
    {
        case BOT_CHATTER_REPLY_PARTY:   ss << "in party chat. ";                break;
        case BOT_CHATTER_REPLY_RAID:    ss << "in raid chat. ";                 break;
        case BOT_CHATTER_REPLY_CHANNEL: ss << "in the zone's General channel. "; break;
        case BOT_CHATTER_REPLY_WHISPER: ss << "in a private whisper. ";         break;
        default:                        ss << "face to face. ";                 break;
    }

    ss << "Stay in character as an inhabitant of this world. Answer with a single short chat line of at most "
        << CHATTER_AI_MAX_TEXT_LENGTH / 2 << " characters, in the language of the last message, "
        << "without markdown, emojis, quotes or your own name in front. Never mention being an AI or a bot.";

    if (BotCfg::IsBotOpenAIEmotesEnabled())
        ss << " You can also perform an emote like a player typing /wave, pick none if no emote fits. "
            << "The message may be empty when the emote alone is your answer.";

    if (mode != BOT_CHATTER_REPLY_STORY)
        ss << " With activity you can change what you are doing when it fits the conversation: rest to sit down, "
            << "story when the player wants to hear a story (you will then tell it sentence by sentence), active to "
            << "get up and go on, otherwise keep.";

    if (!BotCfg::GetBotOpenAIInstructions().empty())
        ss << ' ' << BotCfg::GetBotOpenAIInstructions();

    return ss.str();
}

bool BotChatter::CanChat(bool unprompted, bool inCombat) const
{
    if (!BotCfg::IsBotChatterEnabled())
        return false;
    if (!_me->IsInWorld() || !_me->IsAlive() || _ai->IsDuringTeleport())
        return false;
    // nobody to talk to
    if (!_me->GetMap()->HavePlayers())
        return false;
    if (!inCombat && _me->IsInCombat())
        return false;
    // hired bots only speak when spoken to, unless allowed to chatter
    if (unprompted && !_ai->IAmFree() && !BotCfg::IsBotChatterHiredBotsEnabled())
        return false;
    // listening to or telling campfire stories
    if (unprompted && (_ai->GetActivity().IsInRoleplay() || _ai->GetActivity().IsTellingStory()))
        return false;
    if (_me->GetMap()->IsBattleArena())
        return false;

    return true;
}

bool BotChatter::IsOwnedBy(Player const* player) const
{
    return !_ai->IAmFree() && (_ai->GetBotOwner() == player || _ai->HasOwner(player->GetGUID().GetCounter()));
}

bool BotChatter::CanReplyTo(Player const* player, bool addressed) const
{
    if (_replyTimer || IsAIPending())
        return false;

    // hired bots always answer their owners
    bool mine = IsOwnedBy(player);
    if (!CanChat(!(addressed || mine)))
        return false;
    if (_me->IsHostileTo(player))
        return false;
    if (!mine && !_me->CanSeeOrDetect(player))
        return false;

    return true;
}

void BotChatter::QueueReply(Player const* player, BotChatterCategory category, BotChatterReplyMode mode, uint32 delay)
{
    _replyTarget = player->GetGUID();
    _replyCategory = category;
    _replyMode = mode;
    _replyTimer = delay;

    // already talked to each other, no need to greet them on our own
    if (category == BOT_CHATTER_GREET_REPLY)
        _greeted[player->GetGUID()] = GameTime::GetGameTime().count();
}

void BotChatter::SendPendingReply()
{
    ObjectGuid targetGuid = _replyTarget;
    _replyTarget.Clear();

    ChatterText text;
    if (SelectText(_replyCategory, text))
        DeliverReply(text, targetGuid, _replyMode);
}

// answer a player in the chat they used
void BotChatter::DeliverReply(ChatterText const& text, ObjectGuid targetGuid, BotChatterReplyMode mode)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(targetGuid);
    if (!player || !player->IsInWorld())
        return;

    // whispers work across maps
    if (mode == BOT_CHATTER_REPLY_WHISPER)
    {
        if (_me->IsInWorld() && _me->IsAlive())
            WhisperTo(text, player);
        return;
    }

    if (!CanChat(false, mode != BOT_CHATTER_REPLY_SAY))
        return;

    switch (mode)
    {
        case BOT_CHATTER_REPLY_SAY:
        {
            if (player->GetMap() != _me->GetMap() ||
                !_me->IsWithinDistInMap(player, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY)))
                return;

            if (!_me->isMoving())
            {
                _me->SetFacingToObject(player);
                // canned replies are greetings and farewells
                _me->HandleEmoteCommand(text.textId ? EMOTE_ONESHOT_WAVE : EMOTE_ONESHOT_TALK);
            }

            SayNearby(text, player, CHAT_MSG_MONSTER_SAY);
            break;
        }
        case BOT_CHATTER_REPLY_PARTY:
        case BOT_CHATTER_REPLY_RAID:
            SayToGroup(text, player, mode == BOT_CHATTER_REPLY_RAID ? CHAT_MSG_RAID : CHAT_MSG_PARTY);
            break;
        case BOT_CHATTER_REPLY_CHANNEL:
            if (player->GetMap() == _me->GetMap())
                SayToZoneChannel(text, player, true);
            break;
        default:
            break;
    }
}

bool BotChatter::TryGreetNearbyPlayer()
{
    time_t now = GameTime::GetGameTime().count();
    time_t cooldown = BotCfg::GetBotChatterGreetCooldown();
    std::erase_if(_greeted, [now, cooldown](auto const& greeted) { return greeted.second + cooldown <= now; });

    std::list<Player*> players;
    Acore::AnyPlayerInObjectRangeCheck check(_me, CHATTER_GREET_RANGE, true, true);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(_me, players, check);
    Cell::VisitObjects(_me, searcher, CHATTER_GREET_RANGE);

    for (Player* player : players)
    {
        if (_greeted.contains(player->GetGUID()) || player->IsInCombat())
            continue;
        // no need to greet own master or group mates
        if (player == _ai->GetBotOwner() || (_ai->GetGroup() && player->GetGroup() == _ai->GetGroup()))
            continue;
        if (_me->IsHostileTo(player) || !_me->CanSeeOrDetect(player))
            continue;

        // roll once per encounter
        _greeted[player->GetGUID()] = now;
        if (!roll_chance_i(BotCfg::GetBotChatterGreetChance()))
            continue;

        if (!_me->isMoving())
        {
            _me->SetFacingToObject(player);
            _me->HandleEmoteCommand(EMOTE_ONESHOT_WAVE);
        }

        return SayNearby(BOT_CHATTER_GREET, player, CHAT_MSG_MONSTER_SAY);
    }

    return false;
}

void BotChatter::Chatter(BotChatterCategory category, WorldObject const* subject, bool allowChannel)
{
    bool nearby = HasPlayersInRange(sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY));

    bool preferChannel = !nearby || roll_chance_i(CHATTER_CHANNEL_OVER_SAY_CHANCE);
    if (allowChannel && preferChannel && SayToZoneChannel(category, subject))
        return;

    if (nearby)
        SayNearby(category, subject, CHAT_MSG_MONSTER_SAY);
}

bool BotChatter::HasPlayersInRange(float range) const
{
    Player* player = nullptr;
    Acore::AnyPlayerInObjectRangeCheck check(_me, range, true, false);
    Acore::PlayerSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(_me, player, check);
    Cell::VisitObjects(_me, searcher, range);
    return player != nullptr;
}

bool BotChatter::SayNearby(BotChatterCategory category, WorldObject const* subject, ChatMsg msgType)
{
    ChatterText text;
    return SelectText(category, text) && SayNearby(text, subject, msgType);
}

bool BotChatter::SayNearby(ChatterText const& text, WorldObject const* subject, ChatMsg msgType)
{
    bool yell = msgType == CHAT_MSG_MONSTER_YELL;
    float range = sWorld->getFloatConfig(yell ? CONFIG_LISTEN_RANGE_YELL : CONFIG_LISTEN_RANGE_SAY);
    if (!HasPlayersInRange(range))
        return false;

    // every listener gets the same variant in their own language
    TextBuilder builder(*this, msgType, text, subject);
    Acore::LocalizedPacketDo<TextBuilder> localizer(builder);
    Acore::PlayerDistWorker<Acore::LocalizedPacketDo<TextBuilder>> worker(_me, range, localizer);
    Cell::VisitObjects(_me, worker, range);
    PauseToType(text, subject);
    return true;
}

bool BotChatter::SayToGroup(ChatterText const& text, WorldObject const* subject, ChatMsg msgType)
{
    Group* group = _ai->GetGroup();
    if (!group)
        return false;

    TextBuilder builder(*this, msgType, text, subject);
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !member->GetSession())
            continue;

        WorldPacket data;
        builder(data, member->GetSession()->GetSessionDbLocaleIndex());
        member->SendDirectMessage(&data);
    }

    LogChatLine(GroupChatLogKey(group), _me->GetName(), GetDefaultText(text, subject));
    PauseToType(text, subject);
    return true;
}

bool BotChatter::SayToZoneChannel(BotChatterCategory category, WorldObject const* subject, bool ignoreCooldown)
{
    ChatterText text;
    return SelectText(category, text) && SayToZoneChannel(text, subject, ignoreCooldown);
}

bool BotChatter::SayToZoneChannel(ChatterText const& text, WorldObject const* subject, bool ignoreCooldown)
{
    if (!BotCfg::IsBotChatterChannelEnabled())
        return false;

    Map* map = _me->GetMap();
    if (map->Instanceable())
        return false;

    uint32 zoneId = _me->GetZoneId();
    time_t now = GameTime::GetGameTime().count();
    {
        std::lock_guard<std::mutex> guard(ChannelCooldownsLock);
        auto itr = ChannelCooldowns.find(zoneId);
        if (!ignoreCooldown && itr != ChannelCooldowns.end() && itr->second > now)
            return false;
    }

    // bots are not channel members, so post directly to everyone friendly in the zone who has General joined
    std::vector<std::pair<Player*, Channel*>> listeners;
    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* player = itr->GetSource();
        if (!player || !player->IsInWorld() || player->GetZoneId() != zoneId || !_me->IsFriendlyTo(player))
            continue;

        for (Channel* channel : player->GetJoinedChannelsForBots())
        {
            if (channel && channel->GetChannelId() == CHATTER_CHANNEL_GENERAL)
            {
                listeners.emplace_back(player, channel);
                break;
            }
        }
    }

    if (listeners.empty())
        return false;

    {
        std::lock_guard<std::mutex> guard(ChannelCooldownsLock);
        ChannelCooldowns[zoneId] = now + BotCfg::GetBotChatterChannelCooldown();
    }

    std::set<TeamId> teams;
    for (auto const& [player, channel] : listeners)
    {
        WorldPacket data;
        TextBuilder builder(*this, CHAT_MSG_CHANNEL, text, subject, channel->GetName());
        builder(data, player->GetSession()->GetSessionDbLocaleIndex());
        player->SendDirectMessage(&data);
        teams.insert(player->GetTeamId());
    }

    std::string const defaultText = GetDefaultText(text, subject);
    for (TeamId team : teams)
        LogChatLine(ChannelChatLogKey(zoneId, team), _me->GetName(), defaultText);

    PauseToType(text, subject);
    return true;
}

// text emote with its animation, seen by players around the bot
void BotChatter::PerformEmote(uint32 textEmote, Player* target)
{
    EmotesTextEntry const* emote = sEmotesTextStore.LookupEntry(textEmote);
    if (!emote || !_me->IsInWorld() || !_me->IsAlive())
        return;

    float range = sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE);
    if (target && (!target->IsInWorld() || target->GetMap() != _me->GetMap() ||
        !_me->IsWithinDistInMap(target, range)))
        target = nullptr;

    switch (emote->textid)
    {
        // lasting states only for a few seconds, standing still meanwhile
        case EMOTE_STATE_DANCE:
        case EMOTE_STATE_SLEEP:
        case EMOTE_STATE_SIT:
        case EMOTE_STATE_KNEEL:
        {
            if (_me->IsInCombat())
                break;

            EndStateEmote();
            uint32 duration = emote->textid == EMOTE_STATE_DANCE ? urand(3000, 5000) : urand(5000, 8000);
            _ai->PauseForTalking(duration);
            if (target)
                _me->SetFacingToObject(target);

            switch (emote->textid)
            {
                case EMOTE_STATE_DANCE: _me->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_DANCE); break;
                case EMOTE_STATE_SLEEP: _me->SetStandState(UNIT_STAND_STATE_SLEEP);                 break;
                case EMOTE_STATE_SIT:   _me->SetStandState(UNIT_STAND_STATE_SIT);                   break;
                default:                _me->SetStandState(UNIT_STAND_STATE_KNEEL);                 break;
            }

            _stateEmote = emote->textid;
            _stateEmoteTimer = duration;
            break;
        }
        case EMOTE_ONESHOT_NONE:
            break;
        default:
            if (target && !_me->isMoving())
                _me->SetFacingToObject(target);
            _me->HandleEmoteCommand(emote->textid);
            break;
    }

    EmoteBuilder builder(_me, textEmote, target);
    Acore::LocalizedPacketDo<EmoteBuilder> localizer(builder);
    Acore::PlayerDistWorker<Acore::LocalizedPacketDo<EmoteBuilder>> worker(_me, range, localizer);
    Cell::VisitObjects(_me, worker, range);
}

// back to what the bot was doing: standing, or sitting when resting or at a campfire
void BotChatter::EndStateEmote()
{
    if (!_stateEmote)
        return;

    if (_stateEmote == EMOTE_STATE_DANCE)
        _me->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
    else if (_me->IsAlive())
        _me->SetStandState(_ai->GetActivity().IsSeated() ? UNIT_STAND_STATE_SIT : UNIT_STAND_STATE_STAND);

    _stateEmote = 0;
    _stateEmoteTimer = 0;
}

bool BotChatter::WhisperTo(ChatterText const& text, Player* player)
{
    WorldPacket data;
    TextBuilder builder(*this, CHAT_MSG_WHISPER, text, player);
    builder(data, player->GetSession()->GetSessionDbLocaleIndex());
    player->SendDirectMessage(&data);
    PauseToType(text, player);
    return true;
}

void BotChatter::PauseToType(ChatterText const& text, WorldObject const* subject)
{
    std::size_t length = GetDefaultText(text, subject).size();
    _ai->PauseForTalking(std::clamp<uint32>(uint32(400 + 25 * length), 800, 3000));
}

std::string BotChatter::GetDefaultText(ChatterText const& text, WorldObject const* subject) const
{
    return text.textId ? FormatText(text.textId, text.slot, DEFAULT_LOCALE, subject) : text.raw;
}

std::string BotChatter::FormatText(uint32 textId, uint8 slot, LocaleConstant locale, WorldObject const* subject) const
{
    std::string text = GetTextVariant(textId, slot, locale);
    if (text.find('%') == std::string::npos)
        return text;

    LocaleConstant dbcLocale = sWorld->GetAvailableDbcLocale(locale);

    if (subject)
    {
        Unit const* unit = subject->ToUnit();
        ReplaceAll(text, "%target_level", unit ? std::to_string(unit->GetLevel()) : "");
        ReplaceAll(text, "%target", subject->GetNameForLocaleIdx(locale));
    }

    ReplaceAll(text, "%name", _me->GetNameForLocaleIdx(locale));
    ReplaceAll(text, "%level", std::to_string(_me->GetLevel()));

    if (text.find("%class") != std::string::npos)
    {
        uint32 classTextId = GetClassTextId(_ai->GetBotClass());
        ReplaceAll(text, "%class", classTextId ? GetTextVariant(classTextId, 0, locale) : "");
    }

    if (text.find("%race") != std::string::npos)
    {
        ChrRacesEntry const* race = sChrRacesStore.LookupEntry(_ai->GetPlayerRace());
        ReplaceAll(text, "%race", race ? race->name[dbcLocale] : "");
    }

    if (text.find("%zone") != std::string::npos)
    {
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(_me->GetZoneId());
        ReplaceAll(text, "%zone", (zone && *zone->area_name[dbcLocale]) ? zone->area_name[dbcLocale] :
            GetTextVariant(BOT_TEXT_CHATTER_UNKNOWN_AREA, 0, locale));
    }

    return text;
}
