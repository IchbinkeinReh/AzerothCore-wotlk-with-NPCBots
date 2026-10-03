#include "bot_ai.h"
#include "botactivity.h"
#include "botchatter.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botdefine.h"
#include "botmemory.h"
#include "botnews.h"
#include "botopenai.h"
#include "botstoryarcs.h"
#include "bottext.h"
#include "botworldevents.h"
#include "Chat.h"
#include "Containers.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <boost/version.hpp>
#include <sstream>
#include <unordered_map>

#if BOOST_VERSION >= 107500 && __has_include(<boost/json.hpp>)
#include <boost/json.hpp>
#define BOT_STORY_ARCS_JSON 1
#endif

/*
NpcBot Story Arcs, see botstoryarcs.h
*/

namespace
{
    using Arc = BotStoryArcs::ArcRecord;

    constexpr uint32 ARC_UPDATE_INTERVAL = 3 * IN_MILLISECONDS;
    constexpr time_t ARC_LIFETIME = 3 * DAY;
    constexpr time_t ARC_PENDING_TIMEOUT = 2 * MINUTE;
    constexpr uint8 ARC_MIN_LEVEL = 10;
    constexpr float ARC_GIVER_RANGE = 20.0f;
    constexpr float ARC_HELPER_MIN_DISTANCE = 250.0f;
    constexpr float ARC_HELPER_MAX_DISTANCE = 1500.0f;
    constexpr float ARC_HELPER_TALK_RANGE = 8.0f;
    constexpr float ARC_CAMP_TRIGGER_RANGE = 45.0f;
    constexpr float ARC_GIVER_SAY_RANGE = 40.0f;
    constexpr uint32 ARC_HELPER_TRIES = 6;
    constexpr uint32 ARC_MAX_OUTPUT_TOKENS = 1500;

    enum ArcStep : uint8
    {
        ARC_FIND_HELPER = 0,
        ARC_GO_TO_CAMP,
        ARC_BATTLE
    };

    // waiting for OpenAI
    struct PendingArc
    {
        Arc arc;
        uint32 giverEntry;
        time_t expires;
    };

    std::unordered_map<uint32 /*player*/, Arc> Arcs;
    std::unordered_map<uint32 /*player*/, uint32 /*event id*/> Battles;
    std::unordered_map<uint32 /*player*/, PendingArc> Pending;
    std::unordered_map<uint32 /*player*/, time_t> NextArc;
    uint32 UpdateTimer = 0;

    time_t Now()
    {
        return GameTime::GetGameTime().count();
    }

    // the bot behind a memory key (persona or entry), nullptr if it is not around this session
    Creature* FindBotByKey(uint32 key)
    {
        for (Creature const* bot : BotDataMgr::GetExistingNPCBots())
            if (BotMemory::GetSocialKey(bot->GetEntry()) == key)
                return const_cast<Creature*>(bot);
        return nullptr;
    }

    bool NameMatches(std::string const& a, std::string const& b)
    {
        std::wstring wa, wb;
        if (!Utf8toWStr(a, wa) || !Utf8toWStr(b, wb))
            return false;
        wstrToLower(wa);
        wstrToLower(wb);
        return wa == wb;
    }

    void ReplaceAll(std::string& text, std::string_view token, std::string_view value)
    {
        for (std::size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size()))
            text.replace(pos, token.size(), value);
    }

    std::string AreaName(uint32 areaId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        return area ? area->area_name[sWorld->GetAvailableDbcLocale(BotChatter::GetServerLocale())] : "";
    }

    std::string PlaceName(Map* map, Position const& pos)
    {
        std::string name = AreaName(map->GetAreaId(PHASEMASK_NORMAL, pos.GetPositionX(), pos.GetPositionY(),
            pos.GetPositionZ()));
        std::string zone = AreaName(map->GetZoneId(PHASEMASK_NORMAL, pos.GetPositionX(), pos.GetPositionY(),
            pos.GetPositionZ()));
        if (name.empty())
            return zone;
        return (zone.empty() || zone == name) ? name : name + ", " + zone;
    }

    void SendHint(Player* player, uint32 textId, Arc const& arc)
    {
        std::string text = BotChatter::GetServerText(textId);
        ReplaceAll(text, "%title", arc.title);
        ReplaceAll(text, "%helper", arc.helperName);
        ReplaceAll(text, "%place", textId == BOT_TEXT_ARC_HINT_HELPER ? arc.helperPlace : arc.campPlace);
        ReplaceAll(text, "%enemy", arc.enemyName);
        ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    // a whisper of a bot that may not be in the world right now (far away, parked)
    void WhisperFrom(Player* player, uint32 botKey, std::string const& name, std::string const& text)
    {
        if (text.empty())
            return;

        Creature* bot = FindBotByKey(botKey);
        if (bot && bot->IsInWorld() && bot->GetBotAI() &&
            bot->GetBotAI()->GetChatter().SayRaw(text, CHAT_MSG_WHISPER, player))
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_UNIVERSAL, bot ? bot->GetGUID() : ObjectGuid::Empty,
            player->GetGUID(), text, 0, name, "", 0, true);
        player->SendDirectMessage(&data);
    }

    // "To [bot]: ..." for whispers handled here
    void WhisperInform(Player* player, uint32 botKey, std::string const& name, std::string_view message)
    {
        Creature const* bot = FindBotByKey(botKey);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_INFORM, LANG_UNIVERSAL,
            bot ? bot->GetGUID() : ObjectGuid::Empty, player->GetGUID(), message, 0, name, "", 0, true);
        player->SendDirectMessage(&data);
    }

    bool IsFreeWandererOf(Creature const* bot, Player const* player)
    {
        bot_ai const* ai = bot->GetBotAI();
        return ai && bot->IsInWorld() && bot->IsAlive() && ai->IsWanderer() && ai->IAmFree() &&
            !ai->IsDuringTeleport() && !ai->GetBG() && !ai->GetActivity().IsInEvent() &&
            bot->GetMap() == player->GetMap() &&
            BotDataMgr::GetTeamIdForFaction(bot->GetFaction()) == player->GetTeamId();
    }

    char const* EnglishClassName(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:      return "warrior";
            case CLASS_PALADIN:      return "paladin";
            case CLASS_HUNTER:       return "hunter";
            case CLASS_ROGUE:        return "rogue";
            case CLASS_PRIEST:       return "priest";
            case CLASS_DEATH_KNIGHT: return "death knight";
            case CLASS_SHAMAN:       return "shaman";
            case CLASS_MAGE:         return "mage";
            case CLASS_WARLOCK:      return "warlock";
            case CLASS_DRUID:        return "druid";
            default:                 return "adventurer";
        }
    }

    std::string EnglishRaceName(uint8 race)
    {
        ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race);
        return entry ? entry->name[LOCALE_enUS] : "";
    }

    std::string BuildArcInstructions(Player const* player, Creature const* giver, Creature const* helper,
        Arc const& arc)
    {
        bot_ai const* giverAI = giver->GetBotAI();
        bot_ai const* helperAI = helper->GetBotAI();

        std::ostringstream ss;
        ss << "You write a short quest storyline for a player of World of Warcraft: Wrath of the Lich King. "
            << "Write everything in " << BotChatter::GetServerLanguageName() << ".\n"
            << "The quest giver is " << arc.giverName << ", a level " << uint32(giver->GetLevel()) << ' '
            << EnglishRaceName(giverAI->GetPlayerRace()) << " adventurer, standing in "
            << PlaceName(giver->GetMap(), giver->GetPosition()) << ". " << arc.giverName << " asks "
            << player->GetName() << " (a level " << uint32(player->GetLevel()) << ' '
            << EnglishRaceName(player->GetRace()) << ' ' << EnglishClassName(player->GetClass()) << ") for help.\n"
            << player->GetName() << " has to find " << arc.helperName << ", a "
            << EnglishRaceName(helperAI->GetPlayerRace()) << " adventurer near " << arc.helperPlace
            << ", who knows more.\n"
            << "The trail leads to " << arc.enemyName << " at " << arc.campPlace << ", where " << player->GetName()
            << " has to fight waves of " << arc.enemyName << ".\n"
            << "Make up why: something lost, stolen, someone missing, a threat, a debt, fitting these creatures and "
            << "places. Write:\n"
            << "- title: a short quest title, at most 6 words\n"
            << "- intro: what " << arc.giverName << " says to " << player->GetName() << ", 1-2 sentences, naming "
            << arc.helperName << " and " << arc.helperPlace << "\n"
            << "- helper_line: what " << arc.helperName << " tells " << player->GetName() << ", 1-2 sentences, "
            << "pointing to the " << arc.enemyName << " at " << arc.campPlace << "\n"
            << "- battle_cry: what a fighter yells when the " << arc.enemyName << " attack, one short sentence\n"
            << "- finale: what " << arc.giverName << " whispers to " << player->GetName() << " after the victory, "
            << "1-2 sentences, thanking and closing the story\n"
            << "Each text at most 230 characters, spoken in character as an inhabitant of this world, no markdown, "
            << "no quotes, no emojis, never mention being an AI or a game.";
        return ss.str();
    }

    std::string const& ArcSchema()
    {
        static std::string const schema = R"({"type":"object","properties":{)"
            R"("title":{"type":"string"},"intro":{"type":"string"},"helper_line":{"type":"string"},)"
            R"("battle_cry":{"type":"string"},"finale":{"type":"string"}},)"
            R"("required":["title","intro","helper_line","battle_cry","finale"],"additionalProperties":false})";
        return schema;
    }

    // a giver next to the player, a helper elsewhere in the zone with a camp of enemies near it
    void TryCreateArc(Player* player)
    {
        Creature* giver = nullptr;
        std::vector<Creature*> helpers;
        for (Creature const* cbot : BotDataMgr::GetExistingNPCBots())
        {
            if (!IsFreeWandererOf(cbot, player))
                continue;

            float dist = player->GetExactDist2d(cbot);
            if (dist <= ARC_GIVER_RANGE && (!giver || dist < player->GetExactDist2d(giver)))
                giver = const_cast<Creature*>(cbot);
            else if (dist >= ARC_HELPER_MIN_DISTANCE && dist <= ARC_HELPER_MAX_DISTANCE)
                helpers.push_back(const_cast<Creature*>(cbot));
        }
        if (!giver || helpers.empty())
            return;

        // helpers in the player's zone first
        Acore::Containers::RandomShuffle(helpers);
        std::ranges::stable_partition(helpers, [player](Creature const* bot) {
            return bot->GetZoneId() == player->GetZoneId();
        });

        Arc arc;
        Creature* helper = nullptr;
        Position camp;
        for (std::size_t i = 0; i < helpers.size() && i < ARC_HELPER_TRIES && !helper; ++i)
            if (BotWorldEvents::FindStoryCamp(player, helpers[i], camp, arc.enemyEntry, arc.enemyName))
                helper = helpers[i];
        if (!helper)
            return;

        Map* map = player->GetMap();
        arc.player = player->GetGUID().GetCounter();
        arc.giverKey = BotMemory::GetSocialKey(giver->GetEntry());
        arc.giverName = giver->GetName();
        arc.helperKey = BotMemory::GetSocialKey(helper->GetEntry());
        arc.helperName = helper->GetName();
        arc.helperPlace = PlaceName(map, helper->GetPosition());
        arc.mapId = map->GetId();
        arc.x = camp.GetPositionX();
        arc.y = camp.GetPositionY();
        arc.z = camp.GetPositionZ();
        arc.zoneId = map->GetZoneId(PHASEMASK_NORMAL, camp.GetPositionX(), camp.GetPositionY(),
            camp.GetPositionZ());
        arc.campPlace = PlaceName(map, camp);
        arc.step = ARC_FIND_HELPER;
        arc.expires = Now() + ARC_LIFETIME;

        BotAIRequest request;
        request.botEntry = giver->GetEntry();
        request.playerGuid = player->GetGUID();
        request.kind = BOT_AI_KIND_STORY_ARC;
        request.schemaName = "story_arc";
        request.schema = ArcSchema();
        request.maxOutputTokens = ARC_MAX_OUTPUT_TOKENS;
        request.instructions = BuildArcInstructions(player, giver, helper, arc);
        request.input.push_back({ false, "(Write the story.)" });
        if (!BotOpenAI::Enqueue(std::move(request)))
            return;

        Pending[arc.player] = { std::move(arc), giver->GetEntry(), Now() + ARC_PENDING_TIMEOUT };
    }

    void AdvanceToCamp(Player* player, Arc& arc)
    {
        Creature* helper = FindBotByKey(arc.helperKey);
        if (helper && helper->IsInWorld() && helper->GetMap() == player->GetMap() &&
            helper->IsWithinDistInMap(player, ARC_HELPER_TALK_RANGE) && helper->GetBotAI())
        {
            helper->SetFacingToObject(player);
            helper->GetBotAI()->GetChatter().SayRaw(arc.helperLine, CHAT_MSG_MONSTER_SAY, player);
        }
        else
            WhisperFrom(player, arc.helperKey, arc.helperName, arc.helperLine);

        arc.step = ARC_GO_TO_CAMP;
        SendHint(player, BOT_TEXT_ARC_HINT_CAMP, arc);
    }

    void CompleteArc(Player* player, Arc const& arc)
    {
        WhisperFrom(player, arc.giverKey, arc.giverName, arc.finale);

        uint32 level = player->GetLevel();
        uint32 reward = std::max<uint32>(level * level * 25, 100);
        player->ModifyMoney(int32(reward));

        std::string text = BotChatter::GetServerText(BOT_TEXT_ARC_COMPLETE);
        ReplaceAll(text, "%title", arc.title);
        ReplaceAll(text, "%price", BotWorldEvents::FormatMoney(reward));
        ChatHandler(player->GetSession()).SendSysMessage(text);

        BotNews::RecordDeed(player, BOT_DEED_STORY_ARC, arc.title, arc.campPlace, arc.zoneId);
        for (uint32 key : { arc.giverKey, arc.helperKey })
            if (Creature* bot = FindBotByKey(key); bot && bot->GetBotAI())
                bot->GetBotAI()->GetChatter().NoteRelation(player, BOT_RELATION_STORY);
    }

    // returns false when the arc is over
    bool UpdateArc(Player* player, Arc& arc)
    {
        if (arc.expires <= Now())
            return false;

        switch (arc.step)
        {
            case ARC_FIND_HELPER:
            {
                Creature const* helper = FindBotByKey(arc.helperKey);
                if (helper && helper->IsInWorld() && helper->GetMap() == player->GetMap() &&
                    helper->IsWithinDistInMap(player, ARC_HELPER_TALK_RANGE))
                    AdvanceToCamp(player, arc);
                return true;
            }
            case ARC_GO_TO_CAMP:
            {
                if (player->GetMapId() != arc.mapId || !player->IsAlive() || player->IsInFlight() ||
                    player->GetExactDist2d(arc.x, arc.y) > ARC_CAMP_TRIGGER_RANGE)
                    return true;

                Position camp(arc.x, arc.y, arc.z);
                if (uint32 eventId = BotWorldEvents::StartStoryBattle(player, camp, arc.enemyEntry, arc.battleCry))
                {
                    Battles[arc.player] = eventId;
                    arc.step = ARC_BATTLE;
                }
                return true;
            }
            default:
            {
                auto battle = Battles.find(arc.player);
                BotEventOutcome outcome = battle != Battles.end() ?
                    BotWorldEvents::GetEventOutcome(battle->second) : BOT_EVENT_UNKNOWN;
                if (outcome == BOT_EVENT_RUNNING)
                    return true;

                Battles.erase(arc.player);
                if (outcome == BOT_EVENT_VICTORY)
                {
                    CompleteArc(player, arc);
                    return false;
                }

                // try again: the player has to come back
                arc.step = ARC_GO_TO_CAMP;
                SendHint(player, BOT_TEXT_ARC_FAILED, arc);
                return true;
            }
        }
    }
}

void BotStoryArcs::Update(uint32 diff)
{
    if (UpdateTimer > diff)
    {
        UpdateTimer -= diff;
        return;
    }
    UpdateTimer = ARC_UPDATE_INTERVAL;

    time_t now = Now();
    std::erase_if(Pending, [now](auto const& entry) { return entry.second.expires <= now; });
    std::erase_if(Arcs, [now](auto const& entry) { return entry.second.expires <= now; });

    if (!BotCfg::IsNpcBotModEnabled() || !BotDataMgr::AllBotsLoaded())
        return;

    // only players online: no OpenAI request for anyone else
    for (auto const& [_, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || player->IsBeingTeleported())
            continue;

        uint32 counter = player->GetGUID().GetCounter();
        if (auto arc = Arcs.find(counter); arc != Arcs.end())
        {
            if (!UpdateArc(player, arc->second))
                Arcs.erase(arc);
            continue;
        }

        if (!BotCfg::IsBotStoryArcsEnabled() || !BotOpenAI::IsEnabled() || Pending.contains(counter))
            continue;

        time_t& next = NextArc[counter];
        if (!next)
            next = now + urand(15, 30) * MINUTE;
        if (next > now)
            continue;

        if (player->GetLevel() < ARC_MIN_LEVEL || !player->IsAlive() || player->IsInCombat() ||
            player->IsGameMaster() || player->IsInFlight() || !player->GetMap()->GetEntry()->IsContinent())
            continue;

        uint32 interval = BotCfg::GetBotStoryArcsInterval();
        next = now + urand(interval / 2, interval + interval / 2);
        TryCreateArc(player);
    }
}

void BotStoryArcs::OnArcGenerated(BotAIResult const& result)
{
    uint32 counter = result.playerGuid.GetCounter();
    auto itr = Pending.find(counter);
    if (itr == Pending.end())
        return;

    PendingArc pending = std::move(itr->second);
    Pending.erase(itr);

    Player* player = ObjectAccessor::FindConnectedPlayer(result.playerGuid);
    if (!player || result.text.empty())
        return;

#ifdef BOT_STORY_ARCS_JSON
    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(result.text, ec);
    boost::json::object const* obj = ec ? nullptr : root.if_object();
    if (!obj)
        return;

    auto field = [obj](std::string_view key) -> std::string {
        boost::json::value const* value = obj->if_contains(key);
        return (value && value->is_string()) ? BotChatter::SanitizeText(std::string(value->as_string())) : "";
    };

    Arc& arc = pending.arc;
    arc.title = field("title");
    arc.intro = field("intro");
    arc.helperLine = field("helper_line");
    arc.battleCry = field("battle_cry");
    arc.finale = field("finale");
    if (arc.title.empty() || arc.intro.empty() || arc.helperLine.empty() || arc.finale.empty())
        return;

    // the giver tells the player, face to face if still around
    Creature* giver = const_cast<Creature*>(BotDataMgr::FindBot(pending.giverEntry));
    if (giver && giver->IsInWorld() && giver->GetBotAI() && giver->GetMap() == player->GetMap() &&
        giver->IsWithinDistInMap(player, ARC_GIVER_SAY_RANGE))
    {
        giver->SetFacingToObject(player);
        giver->GetBotAI()->GetChatter().SayRaw(arc.intro, CHAT_MSG_MONSTER_SAY, player);
    }
    else
        WhisperFrom(player, arc.giverKey, arc.giverName, arc.intro);

    SendHint(player, BOT_TEXT_ARC_HINT_HELPER, arc);
    BOT_LOG_DEBUG("npcbots", "Story arc '{}' for {}: {} -> {} -> {} at {}", arc.title, player->GetName(),
        arc.giverName, arc.helperName, arc.enemyName, arc.campPlace);
    Arcs[counter] = std::move(arc);
#endif
}

bool BotStoryArcs::OnPlayerWhisper(Player* player, std::string const& botName, std::string_view message)
{
    auto itr = Arcs.find(player->GetGUID().GetCounter());
    if (itr == Arcs.end() || itr->second.step != ARC_FIND_HELPER || !NameMatches(botName, itr->second.helperName))
        return false;

    WhisperInform(player, itr->second.helperKey, itr->second.helperName, message);
    AdvanceToCamp(player, itr->second);
    return true;
}

std::vector<BotStoryArcs::ArcRecord> BotStoryArcs::ExportArcs()
{
    std::vector<ArcRecord> arcs;
    arcs.reserve(Arcs.size());
    for (auto const& [_, arc] : Arcs)
        arcs.push_back(arc);
    return arcs;
}

void BotStoryArcs::ImportArcs(std::vector<ArcRecord> const& arcs)
{
    time_t now = Now();
    for (ArcRecord const& record : arcs)
    {
        if (!record.player || record.expires <= now || record.title.empty())
            continue;
        Arc& arc = Arcs[record.player];
        arc = record;
        // a battle does not survive a restart
        if (arc.step == ARC_BATTLE)
            arc.step = ARC_GO_TO_CAMP;
    }
}
