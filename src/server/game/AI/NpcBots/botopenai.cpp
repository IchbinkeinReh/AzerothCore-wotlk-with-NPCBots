#include "botconfig.h"
#include "botdefine.h"
#include "botopenai.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include <boost/version.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

/*
NpcBot OpenAI, see botopenai.h
*/

#if BOOST_VERSION >= 107500 && __has_include(<boost/json.hpp>)
# define BOT_OPENAI_SUPPORTED
#endif

#ifdef BOT_OPENAI_SUPPORTED

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/json.hpp>
// header-only Boost.JSON, this must be the only translation unit including it
#include <boost/json/src.hpp>
#include <openssl/err.h>

namespace
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    namespace json = boost::json;
    using tcp = net::ip::tcp;

    // world thread config snapshot, the worker threads never read the config
    struct Job
    {
        BotAIRequest request;
        std::string apiKey;
        std::string model;
        std::string endpoint;
        std::string reasoningEffort;
        uint32 maxOutputTokens;
        uint32 timeout;
    };

    struct Url
    {
        std::string host;
        std::string port;
        std::string target;
    };

    std::mutex QueueLock;
    std::condition_variable QueueCondition;
    std::deque<Job> Jobs;
    std::deque<BotAIResult> Results;
    std::vector<std::thread> Workers;
    std::atomic<bool> Stopping{ false };
    std::atomic<uint32> FailureCount{ 0 };

    // joins the workers if the server exits without calling BotOpenAI::Shutdown()
    struct WorkersGuard
    {
        ~WorkersGuard() { BotOpenAI::Shutdown(); }
    } WorkersGuardInstance;

    bool ParseUrl(std::string const& url, Url& out)
    {
        static constexpr std::string_view scheme = "https://";
        if (!url.starts_with(scheme))
            return false;

        std::string_view rest = std::string_view(url).substr(scheme.size());
        std::size_t slash = rest.find('/');
        std::string_view hostport = rest.substr(0, slash);
        out.target = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));

        std::size_t colon = hostport.find(':');
        out.host = std::string(hostport.substr(0, colon));
        out.port = colon == std::string_view::npos ? "443" : std::string(hostport.substr(colon + 1));
        return !out.host.empty();
    }

    bool IsStructured(BotAIRequest const& request)
    {
        return request.schema.empty() && (!request.emotes.empty() || request.allowActivity || request.storyStep);
    }

    std::string BuildRequestBody(Job const& job)
    {
        json::array input;
        for (BotAIMessage const& message : job.request.input)
            input.push_back(json::object{
                { "role", message.fromBot ? "assistant" : "user" },
                { "content", message.text }
            });

        json::object body;
        body["model"] = job.model;
        body["instructions"] = job.request.instructions;
        body["input"] = std::move(input);
        body["max_output_tokens"] = job.request.maxOutputTokens ? job.request.maxOutputTokens : job.maxOutputTokens;
        body["store"] = false;
        if (!job.reasoningEffort.empty())
            body["reasoning"] = json::object{ { "effort", job.reasoningEffort } };

        // own structured answer
        if (!job.request.schema.empty())
        {
            boost::system::error_code ec;
            json::value schema = json::parse(job.request.schema, ec);
            if (!ec)
                body["text"] = json::object{ { "format", json::object{
                    { "type", "json_schema" },
                    { "name", job.request.schemaName.empty() ? "bot_answer" : job.request.schemaName },
                    { "strict", true },
                    { "schema", std::move(schema) } } } };
        }
        // structured answer: chat line plus optional emote, activity change and story end
        else if (IsStructured(job.request))
        {
            json::object properties;
            json::array required;

            properties["message"] = json::object{
                { "type", "string" },
                { "description", "Chat line, empty if the emote says it all" } };
            required.push_back("message");

            if (!job.request.emotes.empty())
            {
                json::array emotes;
                emotes.push_back("none");
                for (std::string const& emote : job.request.emotes)
                    emotes.push_back(json::string(emote));

                properties["emote"] = json::object{
                    { "type", "string" },
                    { "description", "Emote performed with the message, none for no emote" },
                    { "enum", std::move(emotes) } };
                required.push_back("emote");
            }

            if (job.request.allowActivity)
            {
                properties["activity"] = json::object{
                    { "type", "string" },
                    { "description", "keep: go on as before, active: get up and go on, rest: sit down and rest, "
                        "story: sit down at a campfire and tell the player a story" },
                    { "enum", json::array{ "keep", "active", "rest", "story" } } };
                required.push_back("activity");
            }

            if (job.request.storyStep)
            {
                properties["story_end"] = json::object{
                    { "type", "boolean" },
                    { "description", "True for the last sentence of the story, with the goodbye" } };
                required.push_back("story_end");
            }

            json::object schema{
                { "type", "object" },
                { "properties", std::move(properties) },
                { "required", std::move(required) },
                { "additionalProperties", false }
            };

            body["text"] = json::object{ { "format", json::object{
                { "type", "json_schema" },
                { "name", "bot_reply" },
                { "strict", true },
                { "schema", std::move(schema) } } } };
        }

        return json::serialize(body);
    }

    // concatenated output_text of all output messages
    std::string ParseResponseText(std::string const& response, std::string& error)
    {
        boost::system::error_code ec;
        json::value root = json::parse(response, ec);
        if (ec || !root.is_object())
        {
            error = "invalid JSON response";
            return {};
        }

        json::object const& obj = root.get_object();
        if (json::value const* err = obj.if_contains("error"); err && err->is_object())
        {
            if (json::value const* message = err->get_object().if_contains("message"); message && message->is_string())
                error = std::string(message->get_string());
            else
                error = "unknown API error";
            return {};
        }

        std::string text;
        if (json::value const* output = obj.if_contains("output"); output && output->is_array())
        {
            for (json::value const& item : output->get_array())
            {
                json::object const* itemObj = item.if_object();
                if (!itemObj)
                    continue;
                json::value const* type = itemObj->if_contains("type");
                json::value const* content = itemObj->if_contains("content");
                if (!type || !type->is_string() || type->get_string() != "message" || !content || !content->is_array())
                    continue;

                for (json::value const& part : content->get_array())
                {
                    json::object const* partObj = part.if_object();
                    if (!partObj)
                        continue;
                    json::value const* partType = partObj->if_contains("type");
                    json::value const* partText = partObj->if_contains("text");
                    if (partType && partType->is_string() && partType->get_string() == "output_text" &&
                        partText && partText->is_string())
                        text += std::string(partText->get_string());
                }
            }
        }

        if (text.empty())
            error = "no output text";
        return text;
    }

    // structured answer, a model ignoring the format still gets its text through
    void ParseStructuredAnswer(std::string const& answer, BotAIResult& result)
    {
        boost::system::error_code ec;
        json::value root = json::parse(answer, ec);
        json::object const* obj = ec ? nullptr : root.if_object();
        if (!obj)
        {
            result.text = answer;
            return;
        }

        if (json::value const* value = obj->if_contains("message"); value && value->is_string())
            result.text = std::string(value->get_string());
        if (json::value const* value = obj->if_contains("emote"); value && value->is_string())
            result.emote = std::string(value->get_string());
        if (json::value const* value = obj->if_contains("activity"); value && value->is_string())
            result.activity = std::string(value->get_string());
        if (json::value const* value = obj->if_contains("story_end"); value && value->is_bool())
            result.storyEnd = value->get_bool();
    }

    // one HTTPS POST, every step is bound by the timeout
    std::string Post(Url const& url, Job const& job, std::string const& body, std::string& error)
    {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        std::string response;
        std::chrono::seconds timeout(job.timeout);

        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            try
            {
                auto executor = co_await net::this_coro::executor;
                tcp::resolver resolver(executor);
                beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);

                if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()))
                    throw boost::system::system_error(static_cast<int>(::ERR_get_error()),
                        net::error::get_ssl_category());
                stream.set_verify_callback(ssl::host_name_verification(url.host));

                auto endpoints = co_await resolver.async_resolve(url.host, url.port, net::use_awaitable);

                beast::get_lowest_layer(stream).expires_after(timeout);
                co_await beast::get_lowest_layer(stream).async_connect(endpoints, net::use_awaitable);
                co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);

                http::request<http::string_body> req{ http::verb::post, url.target, 11 };
                req.set(http::field::host, url.host);
                req.set(http::field::user_agent, "AzerothCore-NPCBots");
                req.set(http::field::content_type, "application/json");
                req.set(http::field::authorization, "Bearer " + job.apiKey);
                req.body() = body;
                req.prepare_payload();

                co_await http::async_write(stream, req, net::use_awaitable);

                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                co_await http::async_read(stream, buffer, res, net::use_awaitable);

                response = std::move(res.body());
                if (res.result() != http::status::ok && response.empty())
                    error = "HTTP " + std::to_string(res.result_int());
            }
            catch (std::exception const& e)
            {
                error = e.what();
            }
        }, net::detached);

        ioc.run();
        return response;
    }

    void Work()
    {
        while (true)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(QueueLock);
                QueueCondition.wait(lock, [] { return Stopping.load() || !Jobs.empty(); });
                if (Stopping)
                    return;
                job = std::move(Jobs.front());
                Jobs.pop_front();
            }

            std::string error;
            std::string text;
            BotAIResult result{ job.request.botEntry, job.request.playerGuid, job.request.replyMode, {}, {}, {},
                false, job.request.kind };
            Url url;
            // the player logged out while the request waited
            if (!ObjectAccessor::FindConnectedPlayer(job.request.playerGuid))
                error.clear();
            else if (!ParseUrl(job.endpoint, url))
                error = "NpcBot.Chatter.OpenAI.Endpoint must be an https:// URL";
            else
            {
                std::string response = Post(url, job, BuildRequestBody(job), error);
                if (!response.empty())
                    text = ParseResponseText(response, error);
                if (!text.empty() && IsStructured(job.request))
                    ParseStructuredAnswer(text, result);
                else
                    result.text = std::move(text);
            }

            if (Stopping)
                return;

            // don't flood the log if the key or the model is wrong
            if (!error.empty() && FailureCount.fetch_add(1) % 50 == 0)
                BOT_LOG_ERROR("npcbots", "BotOpenAI: request failed: {}", error);

            std::lock_guard<std::mutex> lock(QueueLock);
            Results.push_back(std::move(result));
        }
    }
}

bool BotOpenAI::IsEnabled()
{
    return BotCfg::IsBotOpenAIEnabled() && !Stopping;
}

bool BotOpenAI::Enqueue(BotAIRequest&& request)
{
    if (!IsEnabled())
        return false;

    // only for players online
    if (request.playerGuid.IsEmpty() || !ObjectAccessor::FindConnectedPlayer(request.playerGuid))
        return false;

    std::lock_guard<std::mutex> lock(QueueLock);

    if (Jobs.size() >= BotCfg::GetBotOpenAIQueueSize())
        return false;

    // started on first use, the number of threads is not reloadable
    if (Workers.empty())
        for (uint32 i = 0; i < BotCfg::GetBotOpenAIThreads(); ++i)
            Workers.emplace_back(Work);

    Jobs.push_back({
        .request = std::move(request),
        .apiKey = BotCfg::GetBotOpenAIApiKey(),
        .model = BotCfg::GetBotOpenAIModel(),
        .endpoint = BotCfg::GetBotOpenAIEndpoint(),
        .reasoningEffort = BotCfg::GetBotOpenAIReasoningEffort(),
        .maxOutputTokens = BotCfg::GetBotOpenAIMaxOutputTokens(),
        .timeout = BotCfg::GetBotOpenAITimeout()
    });
    QueueCondition.notify_one();
    return true;
}

bool BotOpenAI::PollResult(BotAIResult& result)
{
    std::lock_guard<std::mutex> lock(QueueLock);
    if (Results.empty())
        return false;

    result = std::move(Results.front());
    Results.pop_front();
    return true;
}

void BotOpenAI::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(QueueLock);
        Stopping = true;
        Jobs.clear();
    }
    QueueCondition.notify_all();

    // a request in flight finishes or times out first
    for (std::thread& worker : Workers)
        if (worker.joinable())
            worker.join();
    Workers.clear();
}

#else

bool BotOpenAI::IsEnabled()
{
    static bool reported = false;
    if (BotCfg::IsBotOpenAIEnabled() && !reported)
    {
        reported = true;
        BOT_LOG_ERROR("npcbots", "BotOpenAI: NpcBot.Chatter.OpenAI.Enable is set, "
            "but this build lacks Boost.JSON (Boost 1.75+)");
    }
    return false;
}

bool BotOpenAI::Enqueue(BotAIRequest&& /*request*/)
{
    return false;
}

bool BotOpenAI::PollResult(BotAIResult& /*result*/)
{
    return false;
}

void BotOpenAI::Shutdown()
{
}

#endif
