#include "bot_ai.h"
#include "botactivity.h"
#include "botchatter.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botdefine.h"
#include "botopenai.h"
#include "bottext.h"
#include "botvillains.h"
#include "botworldevents.h"
#include "Containers.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <boost/version.hpp>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#if BOOST_VERSION >= 107500 && __has_include(<boost/json.hpp>)
#include <boost/json.hpp>
#define BOT_VILLAINS_JSON 1
#endif

/*
NpcBot Villains, see botvillains.h
*/

namespace
{
    using Villain = BotVillains::VillainRecord;

    constexpr uint32 VILLAIN_UPDATE_INTERVAL = 10 * IN_MILLISECONDS;
    constexpr uint8 VILLAIN_MIN_LEVEL = 15;
    constexpr uint8 VILLAIN_LEVEL_SPREAD = 5;
    constexpr time_t VILLAIN_CREATE_DELAY = 20 * MINUTE;
    constexpr time_t VILLAIN_REST_AFTER_DEFEAT = DAY;
    constexpr time_t VILLAIN_PENDING_TIMEOUT = 2 * MINUTE;
    constexpr float VILLAIN_LAIR_MIN_DISTANCE = 200.0f;
    constexpr float VILLAIN_LAIR_MAX_DISTANCE = 1500.0f;
    constexpr float VILLAIN_FINALE_RANGE = 600.0f;
    constexpr uint32 VILLAIN_LAIR_TRIES = 6;
    constexpr uint32 VILLAIN_MAX_OUTPUT_TOKENS = 1200;

    enum VillainState : uint8
    {
        VILLAIN_ACTIVE = 0,     // followers out there
        VILLAIN_READY,          // weak enough to show itself
        VILLAIN_FIGHTING,       // last stand at the lair
        VILLAIN_DEFEATED
    };

    struct PendingVillain
    {
        Villain villain;
        ObjectGuid player;
        time_t expires;
    };

    std::vector<Villain> Villains;
    std::unordered_map<uint32 /*villain id*/, uint32 /*event id*/> Fights;
    std::vector<PendingVillain> Pending;
    uint32 NextVillainId = 1;
    time_t NextCreation = 0;
    uint32 UpdateTimer = 0;

    time_t Now()
    {
        return GameTime::GetGameTime().count();
    }

    LocaleConstant DbcLocale()
    {
        return sWorld->GetAvailableDbcLocale(BotChatter::GetServerLocale());
    }

    std::string AreaName(uint32 areaId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        return area ? area->area_name[DbcLocale()] : "";
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

    std::string CreatureName(uint32 entry)
    {
        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(entry);
        if (!proto)
            return "";
        std::string name = proto->Name;
        if (CreatureLocale const* locale = sObjectMgr->GetCreatureLocale(entry))
            ObjectMgr::GetLocaleString(locale->Name, BotChatter::GetServerLocale(), name);
        return name;
    }

    std::string DisplayName(Villain const& villain)
    {
        return villain.title.empty() ? villain.name : villain.name + " " + villain.title;
    }

    // named elite creatures of a kind: spawned once at most on continents, nowhere in an instance, no bosses
    std::array<std::vector<CreatureTemplate const*>, 3> const& GetVillainBodies()
    {
        static std::array<std::vector<CreatureTemplate const*>, 3> bodies;
        static bool initialized = false;
        if (initialized)
            return bodies;
        initialized = true;

        std::unordered_map<uint32, uint32> continentSpawns;
        std::unordered_set<uint32> instanceCreatures;
        for (auto const& [_, data] : sObjectMgr->GetAllCreatureData())
        {
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            if (!map)
                continue;
            if (map->IsContinent())
                ++continentSpawns[data.id];
            else
                instanceCreatures.insert(data.id);
        }

        for (auto const& [entry, proto] : *sObjectMgr->GetCreatureTemplates())
        {
            if ((proto.rank != CREATURE_ELITE_ELITE && proto.rank != CREATURE_ELITE_RAREELITE) ||
                instanceCreatures.contains(entry) || continentSpawns[entry] > 1)
                continue;
            if ((proto.type_flags & CREATURE_TYPE_FLAG_BOSS_MOB) ||
                (proto.flags_extra & CREATURE_FLAG_EXTRA_DUNGEON_BOSS))
                continue;
            if (proto.ScriptID || (!proto.AIName.empty() && proto.AIName != "SmartAI") || proto.VehicleId ||
                proto.Models.empty() || proto.npcflag || proto.maxlevel > DEFAULT_MAX_LEVEL)
                continue;
            if (proto.unit_flags & (UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC))
                continue;
            if (proto.flags_extra & (CREATURE_FLAG_EXTRA_CIVILIAN | CREATURE_FLAG_EXTRA_TRIGGER))
                continue;

            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(proto.faction);
            if (!faction || !faction->IsHostileToPlayers() || (faction->friendlyMask & FACTION_MASK_PLAYER))
                continue;

            switch (proto.type)
            {
                case CREATURE_TYPE_UNDEAD:   bodies[0].push_back(&proto); break;
                case CREATURE_TYPE_DEMON:    bodies[1].push_back(&proto); break;
                case CREATURE_TYPE_HUMANOID: bodies[2].push_back(&proto); break;
                default:                                                  break;
            }
        }
        return bodies;
    }

    // a free wandering bot in the villain's zone tells the zone
    bool AnnounceInZone(Villain const& villain, uint32 textId, BotChatter::TextVars vars, std::string const& raw = "")
    {
        for (Creature const* cbot : BotDataMgr::GetExistingNPCBots())
        {
            bot_ai const* ai = cbot->GetBotAI();
            if (!ai || !cbot->IsInWorld() || !cbot->IsAlive() || !ai->IsWanderer() || !ai->IAmFree() ||
                cbot->GetMapId() != villain.mapId || cbot->GetZoneId() != villain.zoneId)
                continue;

            BotChatter& chatter = const_cast<Creature*>(cbot)->GetBotAI()->GetChatter();
            return raw.empty() ? chatter.Announce(textId, std::move(vars), CHAT_MSG_CHANNEL) :
                chatter.SayRaw(raw, CHAT_MSG_CHANNEL);
        }
        return false;
    }

    BotChatter::TextVars VillainVars(Villain const& villain)
    {
        return { { "%villain", villain.name }, { "%title", villain.title }, { "%cult", villain.cult },
            { "%place", villain.lairPlace } };
    }

    char const* ThemeDescription(uint8 theme)
    {
        switch (theme)
        {
            case 0:  return "an undead servant of the Scourge";
            case 1:  return "a demon of the Burning Legion";
            default: return "the leader of a band of raiders";
        }
    }

    void TryCreateVillain(Player* player)
    {
        uint8 level = player->GetLevel();

        // a kind of villain with followers of the player's level
        std::vector<uint8> themes;
        for (uint8 theme = 0; theme < 3; ++theme)
            if (BotWorldEvents::GetInvaderEntries(theme, level).size() >= 2)
                themes.push_back(theme);
        Acore::Containers::RandomShuffle(themes);

        CreatureTemplate const* body = nullptr;
        uint8 theme = 0;
        for (uint8 candidate : themes)
        {
            std::vector<CreatureTemplate const*> bodies;
            for (CreatureTemplate const* proto : GetVillainBodies()[candidate])
                if (proto->minlevel <= uint32(level) + 4 && proto->maxlevel >= uint32(level))
                    bodies.push_back(proto);
            if (!bodies.empty())
            {
                body = Acore::Containers::SelectRandomContainerElement(bodies);
                theme = candidate;
                break;
            }
        }
        if (!body)
            return;

        // the lair: a camp of hostile creatures somewhere in the zone, near another bot
        std::vector<Creature*> anchors;
        for (Creature const* cbot : BotDataMgr::GetExistingNPCBots())
        {
            if (!cbot->IsInWorld() || cbot->GetMap() != player->GetMap() || cbot->GetZoneId() != player->GetZoneId())
                continue;
            float dist = player->GetExactDist2d(cbot);
            if (dist >= VILLAIN_LAIR_MIN_DISTANCE && dist <= VILLAIN_LAIR_MAX_DISTANCE)
                anchors.push_back(const_cast<Creature*>(cbot));
        }
        Acore::Containers::RandomShuffle(anchors);

        Villain villain;
        Position lair;
        uint32 enemyEntry;
        std::string enemyName;
        bool found = false;
        for (std::size_t i = 0; i < anchors.size() && i < VILLAIN_LAIR_TRIES && !found; ++i)
            found = BotWorldEvents::FindStoryCamp(player, anchors[i], lair, enemyEntry, enemyName);
        if (!found)
            return;

        Map* map = player->GetMap();
        villain.entry = body->Entry;
        villain.name = CreatureName(body->Entry);
        villain.theme = theme;
        villain.minLevel = std::max<uint8>(level > VILLAIN_LEVEL_SPREAD ? level - VILLAIN_LEVEL_SPREAD : 1, 1);
        villain.maxLevel = level + VILLAIN_LEVEL_SPREAD;
        villain.mapId = map->GetId();
        villain.zoneId = player->GetZoneId();
        villain.x = lair.GetPositionX();
        villain.y = lair.GetPositionY();
        villain.z = lair.GetPositionZ();
        villain.lairPlace = PlaceName(map, lair);
        villain.state = VILLAIN_ACTIVE;
        villain.created = Now();

        std::ostringstream ss;
        ss << "You create a recurring villain for World of Warcraft: Wrath of the Lich King. Write everything in "
            << BotChatter::GetServerLanguageName() << ".\n"
            << "The villain is " << villain.name << ", " << ThemeDescription(theme) << ", hiding at "
            << villain.lairPlace << ". Its followers raid towns and travellers around. Adventurers will weaken the "
            << "followers until " << villain.name << " shows itself and can be defeated. Write:\n"
            << "- title: an epithet following the name, like 'the Bone Collector', at most 4 words\n"
            << "- cult: the name of the followers as a group, at most 4 words\n"
            << "- backstory: what people tell each other about " << villain.name << ", 1-2 sentences\n"
            << "- taunt: what " << villain.name << " yells when the heroes come, one sentence\n"
            << "- defeat_line: what an adventurer says after " << villain.name << " has fallen, 1-2 sentences\n"
            << "Each text at most 220 characters, in the tone of the game, no markdown, no quotes, no emojis.";

        BotAIRequest request;
        request.botEntry = 0;
        request.playerGuid = player->GetGUID();
        request.kind = BOT_AI_KIND_VILLAIN;
        request.schemaName = "villain";
        request.schema = R"({"type":"object","properties":{"title":{"type":"string"},"cult":{"type":"string"},)"
            R"("backstory":{"type":"string"},"taunt":{"type":"string"},"defeat_line":{"type":"string"}},)"
            R"("required":["title","cult","backstory","taunt","defeat_line"],"additionalProperties":false})";
        request.maxOutputTokens = VILLAIN_MAX_OUTPUT_TOKENS;
        request.instructions = ss.str();
        request.input.push_back({ false, "(Create the villain.)" });
        if (!BotOpenAI::Enqueue(std::move(request)))
            return;

        Pending.push_back({ std::move(villain), player->GetGUID(), Now() + VILLAIN_PENDING_TIMEOUT });
    }

    // a living villain (or one resting after its defeat) around the player's level on the map
    bool HasVillainFor(Player const* player)
    {
        uint8 level = player->GetLevel();
        time_t now = Now();
        for (Villain const& villain : Villains)
        {
            if (villain.mapId != player->GetMapId() || level + VILLAIN_LEVEL_SPREAD < villain.minLevel ||
                level > villain.maxLevel + VILLAIN_LEVEL_SPREAD)
                continue;
            if (villain.state != VILLAIN_DEFEATED || villain.defeated + VILLAIN_REST_AFTER_DEFEAT > now)
                return true;
        }
        return false;
    }

    void StartFinale(Player* player, Villain& villain)
    {
        std::vector<uint32> followers = BotWorldEvents::GetInvaderEntries(villain.theme, player->GetLevel());
        Position lair(villain.x, villain.y, villain.z);
        uint32 eventId = BotWorldEvents::StartVillainBattle(player, villain.id, villain.entry, lair,
            DisplayName(villain), villain.lairPlace, followers, villain.taunt);
        if (!eventId)
            return;

        villain.state = VILLAIN_FIGHTING;
        Fights[villain.id] = eventId;
        AnnounceInZone(villain, BOT_TEXT_VILLAIN_SIGHTED, VillainVars(villain));
        BOT_LOG_DEBUG("npcbots", "Villain {} '{}' shows itself at {} near {}", villain.id, DisplayName(villain),
            villain.lairPlace, player->GetName());
    }

    void UpdateFight(Villain& villain)
    {
        auto fight = Fights.find(villain.id);
        BotEventOutcome outcome = fight != Fights.end() ? BotWorldEvents::GetEventOutcome(fight->second) :
            BOT_EVENT_UNKNOWN;
        if (outcome == BOT_EVENT_RUNNING)
            return;
        Fights.erase(villain.id);

        if (outcome == BOT_EVENT_VICTORY)
        {
            villain.state = VILLAIN_DEFEATED;
            villain.defeated = Now();
            if (!AnnounceInZone(villain, 0, {}, villain.defeatLine))
                AnnounceInZone(villain, BOT_TEXT_VILLAIN_DEFEATED, VillainVars(villain));
            return;
        }

        // escaped: new followers have to be beaten first
        villain.state = VILLAIN_ACTIVE;
        uint32 threshold = BotCfg::GetBotVillainsStrength();
        villain.progress = threshold > 2 ? threshold - 2 : 0;
        AnnounceInZone(villain, BOT_TEXT_VILLAIN_ESCAPED, VillainVars(villain));
    }
}

void BotVillains::Update(uint32 diff)
{
    if (UpdateTimer > diff)
    {
        UpdateTimer -= diff;
        return;
    }
    UpdateTimer = VILLAIN_UPDATE_INTERVAL;

    time_t now = Now();
    std::erase_if(Pending, [now](PendingVillain const& pending) { return pending.expires <= now; });

    if (!BotCfg::IsNpcBotModEnabled() || !BotDataMgr::AllBotsLoaded() || !BotCfg::IsBotVillainsEnabled())
        return;

    for (Villain& villain : Villains)
        if (villain.state == VILLAIN_FIGHTING)
            UpdateFight(villain);

    // only players online: no OpenAI request for anyone else
    for (auto const& [_, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive() ||
            player->IsGameMaster() || player->IsInFlight() || player->IsInCombat() ||
            !player->GetMap()->GetEntry()->IsContinent())
            continue;

        // a weakened villain shows itself to a player coming close
        for (Villain& villain : Villains)
            if (villain.state == VILLAIN_READY && villain.mapId == player->GetMapId() &&
                player->GetLevel() + 3 >= villain.minLevel &&
                (player->GetZoneId() == villain.zoneId ||
                    player->GetExactDist2d(villain.x, villain.y) <= VILLAIN_FINALE_RANGE))
                StartFinale(player, villain);

        if (now < NextCreation || player->GetLevel() < VILLAIN_MIN_LEVEL || !BotOpenAI::IsEnabled() ||
            !Pending.empty() || HasVillainFor(player))
            continue;

        NextCreation = now + VILLAIN_CREATE_DELAY;
        TryCreateVillain(player);
    }
}

void BotVillains::OnVillainGenerated(BotAIResult const& result)
{
    auto itr = std::ranges::find(Pending, result.playerGuid, &PendingVillain::player);
    if (itr == Pending.end())
        return;

    Villain villain = std::move(itr->villain);
    Pending.erase(itr);
    if (result.text.empty())
        return;

#ifdef BOT_VILLAINS_JSON
    boost::system::error_code ec;
    boost::json::value root = boost::json::parse(result.text, ec);
    boost::json::object const* obj = ec ? nullptr : root.if_object();
    if (!obj)
        return;

    auto field = [obj](std::string_view key) -> std::string {
        boost::json::value const* value = obj->if_contains(key);
        return (value && value->is_string()) ? BotChatter::SanitizeText(std::string(value->as_string())) : "";
    };

    villain.title = field("title");
    villain.cult = field("cult");
    villain.backstory = field("backstory");
    villain.taunt = field("taunt");
    villain.defeatLine = field("defeat_line");
    if (villain.title.empty() || villain.cult.empty() || villain.backstory.empty())
        return;

    villain.id = NextVillainId++;
    AnnounceInZone(villain, 0, {}, villain.backstory);
    BOT_LOG_DEBUG("npcbots", "Villain {} '{}' ({}) of entry {} at {}", villain.id, DisplayName(villain), villain.cult,
        villain.entry, villain.lairPlace);
    Villains.push_back(std::move(villain));
#endif
}

bool BotVillains::GetVillainFor(uint8 theme, uint8 level, uint32 mapId, BotVillainRef& out)
{
    if (!BotCfg::IsBotVillainsEnabled())
        return false;

    for (Villain const& villain : Villains)
    {
        if ((villain.state != VILLAIN_ACTIVE && villain.state != VILLAIN_READY) || villain.mapId != mapId ||
            (theme != ANY_THEME && villain.theme != theme) || level + VILLAIN_LEVEL_SPREAD < villain.minLevel ||
            level > villain.maxLevel + VILLAIN_LEVEL_SPREAD)
            continue;

        out = { villain.id, villain.name, villain.title, villain.cult, villain.backstory };
        return true;
    }
    return false;
}

void BotVillains::OnFollowersDefeated(uint32 villainId)
{
    auto itr = std::ranges::find(Villains, villainId, &Villain::id);
    if (itr == Villains.end() || itr->state != VILLAIN_ACTIVE)
        return;

    ++itr->progress;
    AnnounceInZone(*itr, BOT_TEXT_VILLAIN_PROGRESS, VillainVars(*itr));
    if (itr->progress >= BotCfg::GetBotVillainsStrength())
        itr->state = VILLAIN_READY;
}

std::vector<BotVillains::VillainRecord> BotVillains::ExportVillains()
{
    return Villains;
}

void BotVillains::ImportVillains(std::vector<VillainRecord> const& villains)
{
    for (VillainRecord const& record : villains)
    {
        if (!record.id || !record.entry || !sObjectMgr->GetCreatureTemplate(record.entry))
            continue;
        Villain& villain = Villains.emplace_back(record);
        // a fight does not survive a restart: the villain shows itself again
        if (villain.state == VILLAIN_FIGHTING)
            villain.state = VILLAIN_READY;
        NextVillainId = std::max(NextVillainId, villain.id + 1);
    }
}
