#include "bot_ai.h"
#include "botchatter.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botdefine.h"
#include "botmgr.h"
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
    bool SelectText(BotChatterCategory category, uint32& textId, uint8& slot)
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

        std::tie(textId, slot) = Acore::Containers::SelectRandomContainerElement(variants);
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
    _replyCategory(BOT_CHATTER_GREET_REPLY), _replyMode(BOT_CHATTER_REPLY_SAY), _replyTimer(0)
{
}

void BotChatter::TextBuilder::operator()(WorldPacket& data, LocaleConstant locale) const
{
    std::string text = _chatter.FormatText(_textId, _slot, locale, _subject);

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
}

bool BotChatter::OnPlayerWhisper(Player* player, std::string const& botName, std::string_view message)
{
    if (!BotCfg::IsBotChatterEnabled() || !BotDataMgr::AllBotsLoaded())
        return false;

    LocaleConstant locale = player->GetSession()->GetSessionDbLocaleIndex();
    Creature const* bot = BotDataMgr::FindBot(botName, locale);
    if (!bot && locale != DEFAULT_LOCALE)
        bot = BotDataMgr::FindBot(botName, DEFAULT_LOCALE);

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

    BotChatterCategory category = BOT_CHATTER_WHISPER_REPLY;
    if (_me->IsInCombat())
        category = BOT_CHATTER_WHISPER_BUSY;
    else
    {
        std::wstring normalized = NormalizeForMatching(message);
        if (GetReplyCategory(normalized, category) && category == BOT_CHATTER_GREET_REPLY)
            _greeted[player->GetGUID()] = GameTime::GetGameTime().count();
    }

    uint32 textId;
    uint8 slot;
    if (!SelectText(category, textId, slot))
        return;

    WorldPacket data;
    TextBuilder builder(*this, CHAT_MSG_WHISPER, textId, slot, player);
    builder(data, player->GetSession()->GetSessionDbLocaleIndex());
    player->SendDirectMessage(&data);
}

void BotChatter::HandlePlayerMessage(Player const* player, std::string_view message, BotChatterReplyMode mode,
    std::vector<Creature*> const& bots)
{
    if (bots.empty() || message.empty())
        return;

    std::wstring normalized = NormalizeForMatching(message);
    if (normalized.find_first_not_of(L' ') == std::wstring::npos)
        return;

    BotChatterCategory category;
    if (!GetReplyCategory(normalized, category))
        return;

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
    if (_replyTimer)
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

    if (!CanChat(false))
        return;

    switch (_replyMode)
    {
        case BOT_CHATTER_REPLY_SAY:
        {
            Player* player = ObjectAccessor::GetPlayer(*_me, targetGuid);
            if (!player || !player->IsInWorld())
                return;
            if (!_me->IsWithinDistInMap(player, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_SAY)))
                return;

            if (!_me->isMoving())
            {
                _me->SetFacingToObject(player);
                _me->HandleEmoteCommand(EMOTE_ONESHOT_WAVE);
            }

            SayNearby(_replyCategory, player, CHAT_MSG_MONSTER_SAY);
            break;
        }
        case BOT_CHATTER_REPLY_PARTY:
        case BOT_CHATTER_REPLY_RAID:
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(targetGuid);
            if (!player)
                return;

            SayToGroup(_replyCategory, player, _replyMode == BOT_CHATTER_REPLY_RAID ? CHAT_MSG_RAID : CHAT_MSG_PARTY);
            break;
        }
        case BOT_CHATTER_REPLY_CHANNEL:
        {
            Player* player = ObjectAccessor::GetPlayer(*_me, targetGuid);
            if (!player || !player->IsInWorld())
                return;

            SayToZoneChannel(_replyCategory, player, true);
            break;
        }
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
    bool yell = msgType == CHAT_MSG_MONSTER_YELL;
    float range = sWorld->getFloatConfig(yell ? CONFIG_LISTEN_RANGE_YELL : CONFIG_LISTEN_RANGE_SAY);
    if (!HasPlayersInRange(range))
        return false;

    uint32 textId;
    uint8 slot;
    if (!SelectText(category, textId, slot))
        return false;

    // every listener gets the same variant in their own language
    TextBuilder builder(*this, msgType, textId, slot, subject);
    Acore::LocalizedPacketDo<TextBuilder> localizer(builder);
    Acore::PlayerDistWorker<Acore::LocalizedPacketDo<TextBuilder>> worker(_me, range, localizer);
    Cell::VisitObjects(_me, worker, range);
    return true;
}

bool BotChatter::SayToGroup(BotChatterCategory category, WorldObject const* subject, ChatMsg msgType)
{
    Group* group = _ai->GetGroup();
    if (!group)
        return false;

    uint32 textId;
    uint8 slot;
    if (!SelectText(category, textId, slot))
        return false;

    TextBuilder builder(*this, msgType, textId, slot, subject);
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !member->GetSession())
            continue;

        WorldPacket data;
        builder(data, member->GetSession()->GetSessionDbLocaleIndex());
        member->SendDirectMessage(&data);
    }

    return true;
}

bool BotChatter::SayToZoneChannel(BotChatterCategory category, WorldObject const* subject, bool ignoreCooldown)
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

    uint32 textId;
    uint8 slot;
    if (!SelectText(category, textId, slot))
        return false;

    {
        std::lock_guard<std::mutex> guard(ChannelCooldownsLock);
        ChannelCooldowns[zoneId] = now + BotCfg::GetBotChatterChannelCooldown();
    }

    for (auto const& [player, channel] : listeners)
    {
        WorldPacket data;
        TextBuilder builder(*this, CHAT_MSG_CHANNEL, textId, slot, subject, channel->GetName());
        builder(data, player->GetSession()->GetSessionDbLocaleIndex());
        player->SendDirectMessage(&data);
    }

    return true;
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
