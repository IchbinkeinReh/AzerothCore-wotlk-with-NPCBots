#include "botchatter.h"
#include "botcommon.h"
#include "botconfig.h"
#include "botdefine.h"
#include "botmemory.h"
#include "Log.h"
#include "Timer.h"
#include "World.h"

#include <boost/version.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#if BOOST_VERSION >= 107500 && __has_include(<boost/json.hpp>)
#include <boost/json.hpp>
#define BOT_MEMORY_JSON 1
#endif

/*
NpcBot Memory, see botmemory.h
*/

namespace
{
    constexpr uint32 PERSONA_KEY_FLAG = 0x80000000;
    constexpr uint32 MEMORY_FILE_VERSION = 1;

    std::mutex PersonasLock;
    std::vector<BotPersona> Personas;
    std::unordered_map<uint32 /*personaId*/, std::size_t /*index*/> PersonaIndex;
    std::unordered_map<uint32 /*entry*/, uint32 /*personaId*/> EntryPersonas;
    std::unordered_map<uint32 /*personaId*/, uint32 /*entry*/> UsedPersonas;
    std::unordered_map<uint32 /*entry*/, uint32 /*originalEntry*/> OriginalEntries;
    uint32 NextPersonaId = 1;
    bool Loaded = false;
    uint32 SaveTimer = 0;

    std::string GetMemoryFilePath()
    {
        std::filesystem::path file(BotCfg::GetBotMemoryFile());
        if (file.is_relative())
            file = std::filesystem::path(sWorld->GetDataPath()) / file;
        return file.string();
    }
}

#ifdef BOT_MEMORY_JSON
namespace
{
    namespace json = boost::json;

    template<typename T>
    T GetNumber(json::object const& obj, std::string_view key, T def = T())
    {
        json::value const* value = obj.if_contains(key);
        if (!value)
            return def;
        if (value->is_int64())
            return T(value->as_int64());
        if (value->is_uint64())
            return T(value->as_uint64());
        if (value->is_bool())
            return T(value->as_bool());
        return def;
    }

    std::string GetString(json::object const& obj, std::string_view key)
    {
        json::value const* value = obj.if_contains(key);
        return (value && value->is_string()) ? std::string(value->as_string()) : std::string();
    }

    json::value PersonaToJson(BotPersona const& persona)
    {
        json::array appearance;
        for (uint8 value : persona.appearance)
            appearance.push_back(value);

        return json::object{
            { "id", persona.id }, { "class", persona.botClass }, { "proto", persona.protoEntry },
            { "race", persona.race }, { "gender", persona.gender }, { "display", persona.displayId },
            { "hasAppearance", persona.hasAppearance }, { "appearance", std::move(appearance) },
            { "name", persona.name }
        };
    }

    bool PersonaFromJson(json::object const& obj, BotPersona& persona)
    {
        persona.id = GetNumber<uint32>(obj, "id");
        persona.botClass = GetNumber<uint8>(obj, "class");
        persona.protoEntry = GetNumber<uint32>(obj, "proto");
        persona.race = GetNumber<uint8>(obj, "race");
        persona.gender = GetNumber<uint8>(obj, "gender");
        persona.displayId = GetNumber<uint32>(obj, "display");
        persona.hasAppearance = GetNumber<bool>(obj, "hasAppearance");
        persona.name = GetString(obj, "name");
        if (json::value const* appearance = obj.if_contains("appearance"); appearance && appearance->is_array())
        {
            json::array const& values = appearance->as_array();
            for (std::size_t i = 0; i < persona.appearance.size() && i < values.size(); ++i)
                if (values[i].is_int64())
                    persona.appearance[i] = uint8(values[i].as_int64());
        }
        return persona.id && persona.botClass && persona.protoEntry && !persona.name.empty();
    }

    json::value SocialToJson(BotChatter::SocialRecord const& record)
    {
        json::array relations;
        for (BotChatter::RelationRecord const& rel : record.relations)
        {
            relations.push_back(json::object{
                { "player", rel.player }, { "name", rel.name }, { "affinity", rel.affinity },
                { "talks", rel.talks }, { "fights", rel.fights }, { "attacks", rel.attacks },
                { "killedMe", rel.killedMe }, { "killedThem", rel.killedThem }, { "stories", rel.stories },
                { "friendly", rel.friendly }, { "hostile", rel.hostile }, { "hired", rel.hired },
                { "lastSeen", int64(rel.lastSeen) }
            });
        }

        return json::object{
            { "key", record.key }, { "mood", record.mood }, { "activeMinutes", record.activeMinutes },
            { "relations", std::move(relations) }
        };
    }

    BotChatter::SocialRecord SocialFromJson(json::object const& obj)
    {
        BotChatter::SocialRecord record;
        record.key = GetNumber<uint32>(obj, "key");
        record.mood = GetNumber<int32>(obj, "mood");
        record.activeMinutes = GetNumber<uint32>(obj, "activeMinutes");
        if (json::value const* relations = obj.if_contains("relations"); relations && relations->is_array())
        {
            for (json::value const& value : relations->as_array())
            {
                if (!value.is_object())
                    continue;
                json::object const& rel = value.as_object();
                BotChatter::RelationRecord r;
                r.player = GetNumber<uint32>(rel, "player");
                r.name = GetString(rel, "name");
                r.affinity = GetNumber<int32>(rel, "affinity");
                r.talks = GetNumber<uint32>(rel, "talks");
                r.fights = GetNumber<uint32>(rel, "fights");
                r.attacks = GetNumber<uint32>(rel, "attacks");
                r.killedMe = GetNumber<uint32>(rel, "killedMe");
                r.killedThem = GetNumber<uint32>(rel, "killedThem");
                r.stories = GetNumber<uint32>(rel, "stories");
                r.friendly = GetNumber<uint32>(rel, "friendly");
                r.hostile = GetNumber<uint32>(rel, "hostile");
                r.hired = GetNumber<bool>(rel, "hired");
                r.lastSeen = GetNumber<time_t>(rel, "lastSeen");
                if (r.player)
                    record.relations.push_back(std::move(r));
            }
        }
        return record;
    }
}
#endif

void BotMemory::Load()
{
    Loaded = true;
    SaveTimer = BotCfg::GetBotMemorySaveInterval() * IN_MILLISECONDS;

    if (!BotCfg::IsBotMemoryEnabled())
        return;

#ifdef BOT_MEMORY_JSON
    uint32 oldMSTime = getMSTime();
    std::string const path = GetMemoryFilePath();

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        BOT_LOG_INFO("server.loading", ">> No bot memory file '{}' yet, bots start without memories", path);
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    boost::system::error_code ec;
    json::value root = json::parse(buffer.str(), ec);
    if (ec || !root.is_object())
    {
        BOT_LOG_ERROR("server.loading", "Bot memory file '{}' is not valid JSON ({}), ignored", path, ec.message());
        return;
    }

    json::object const& obj = root.as_object();
    std::vector<BotChatter::SocialRecord> social;
    {
        std::lock_guard<std::mutex> lock(PersonasLock);
        Personas.clear();
        PersonaIndex.clear();
        NextPersonaId = std::max<uint32>(GetNumber<uint32>(obj, "nextPersona", 1), 1);

        if (json::value const* personas = obj.if_contains("personas"); personas && personas->is_array())
        {
            for (json::value const& value : personas->as_array())
            {
                BotPersona persona;
                if (!value.is_object() || !PersonaFromJson(value.as_object(), persona) ||
                    PersonaIndex.contains(persona.id))
                    continue;

                NextPersonaId = std::max(NextPersonaId, persona.id + 1);
                PersonaIndex[persona.id] = Personas.size();
                Personas.push_back(std::move(persona));
            }
        }
    }

    if (json::value const* states = obj.if_contains("social"); states && states->is_array())
        for (json::value const& value : states->as_array())
            if (value.is_object())
                social.push_back(SocialFromJson(value.as_object()));

    BotChatter::ImportSocial(social);

    BOT_LOG_INFO("server.loading", ">> Loaded {} bot personas and memories of {} bots from '{}' in {} ms",
        uint32(Personas.size()), uint32(social.size()), path, GetMSTimeDiffToNow(oldMSTime));
#else
    BOT_LOG_WARN("server.loading", "Bot memory needs Boost.JSON (Boost 1.75+), bots forget everything on restart");
#endif
}

void BotMemory::Save()
{
    if (!Loaded || !BotCfg::IsBotMemoryEnabled())
        return;

#ifdef BOT_MEMORY_JSON
    json::object root;
    root["version"] = MEMORY_FILE_VERSION;

    json::array personas;
    {
        std::lock_guard<std::mutex> lock(PersonasLock);
        root["nextPersona"] = NextPersonaId;
        for (BotPersona const& persona : Personas)
            personas.push_back(PersonaToJson(persona));
    }
    root["personas"] = std::move(personas);

    json::array social;
    for (BotChatter::SocialRecord const& record : BotChatter::ExportSocial())
        if (IsPersistentSocialKey(record.key))
            social.push_back(SocialToJson(record));
    root["social"] = std::move(social);

    // write a temporary file first, a crash while writing must not destroy the memories
    std::string const path = GetMemoryFilePath();
    std::string const tmpPath = path + ".tmp";
    {
        std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            BOT_LOG_ERROR("npcbots", "BotMemory: cannot write '{}'", tmpPath);
            return;
        }
        file << json::serialize(root);
        if (!file)
        {
            BOT_LOG_ERROR("npcbots", "BotMemory: writing '{}' failed", tmpPath);
            return;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    if (ec)
        BOT_LOG_ERROR("npcbots", "BotMemory: cannot replace '{}': {}", path, ec.message());
#endif
}

void BotMemory::Update(uint32 diff)
{
    if (!Loaded || !BotCfg::IsBotMemoryEnabled() || !BotCfg::GetBotMemorySaveInterval())
        return;

    if (SaveTimer > diff)
    {
        SaveTimer -= diff;
        return;
    }

    SaveTimer = BotCfg::GetBotMemorySaveInterval() * IN_MILLISECONDS;
    Save();
}

std::optional<BotPersona> BotMemory::TakePersona(uint8 botClass)
{
    if (!BotCfg::IsBotMemoryEnabled())
        return std::nullopt;

    std::lock_guard<std::mutex> lock(PersonasLock);
    for (BotPersona const& persona : Personas)
        if (persona.botClass == botClass && !UsedPersonas.contains(persona.id))
            return persona;
    return std::nullopt;
}

uint32 BotMemory::AddPersona(BotPersona persona)
{
    if (!BotCfg::IsBotMemoryEnabled())
        return 0;

    std::lock_guard<std::mutex> lock(PersonasLock);
    persona.id = NextPersonaId++;
    PersonaIndex[persona.id] = Personas.size();
    Personas.push_back(std::move(persona));
    return Personas.back().id;
}

void BotMemory::BindPersona(uint32 entry, uint32 personaId)
{
    if (!personaId)
        return;

    std::lock_guard<std::mutex> lock(PersonasLock);
    EntryPersonas[entry] = personaId;
    UsedPersonas[personaId] = entry;
}

void BotMemory::BindOriginalEntry(uint32 entry, uint32 originalEntry)
{
    std::lock_guard<std::mutex> lock(PersonasLock);
    OriginalEntries[entry] = originalEntry;
}

uint32 BotMemory::GetSocialKey(uint32 entry)
{
    std::lock_guard<std::mutex> lock(PersonasLock);
    if (auto itr = EntryPersonas.find(entry); itr != EntryPersonas.end())
        return PERSONA_KEY_FLAG | itr->second;
    if (auto itr = OriginalEntries.find(entry); itr != OriginalEntries.end())
        return itr->second;
    return entry;
}

uint32 BotMemory::GetGuildSeed(uint32 entry)
{
    return GetSocialKey(entry);
}

bool BotMemory::IsPersistentSocialKey(uint32 key)
{
    return (key & PERSONA_KEY_FLAG) || key < uint32(BOT_ENTRY_CREATE_BEGIN);
}
