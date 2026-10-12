#include "cloud_translation.h"
#include "config/ime_config.h"
#include "custom_translation.h"
#include "niutrans_translation.h"
#include "tencent_tmt.h"
#include "ai_translation.h"
#include "translation_gloss.h"
#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace
{
constexpr auto kIdleDelay = std::chrono::milliseconds(500);
constexpr auto kNegativeTtl = std::chrono::minutes(8);
constexpr size_t kMaxCacheEntries = 4096;
std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_job{0};
std::vector<EnglishIme::TranslationQuery> g_latest_queries;
uint64_t g_latest_generation = 0;
std::chrono::steady_clock::time_point g_last_request;
CloudTranslation::ApplyCallback g_callback;
std::unordered_map<std::string, std::string> g_cache;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_negative;
std::string Scope(const CandidateTranslationConfig &config)
{
    // Never expose credentials in a key, message or log. Any provider/endpoint/model/key update changes revision.
    return std::to_string(config.revision) + ":" + CloudTranslation::EffectiveProvider(config) + ":" +
           config.tencent.target_language;
}
std::string Identity(const EnglishIme::TranslationQuery &query, const std::string &scope)
{
    return scope + (query.direction == EnglishIme::TranslationDirection::EnglishToChinese ? ":e:" : ":z:") + query.key;
}
bool Current(const CandidateTranslationConfig &config, uint64_t job, uint64_t generation)
{
    return g_running && GetConfiguredCandidateTranslationCloudEnabled() && g_job == job &&
           EnglishIme::IsTranslationCurrent(generation) &&
           Scope(GetConfiguredCandidateTranslationSnapshot()) == Scope(config);
}
bool Usable(const CandidateTranslationConfig &config)
{
    const auto provider = CloudTranslation::EffectiveProvider(config);
    if (provider == "ai")
        return AiTranslation::IsUsableConfig(config);
    if (provider == "niutrans")
        return NiuTransTranslation::IsUsableConfig({CloudTranslation::TrimSecret(config.niutrans.app_id),
                                                    CloudTranslation::TrimSecret(config.niutrans.apikey)});
    if (provider == "custom")
        return CustomTranslation::IsSupportedEndpoint(CloudTranslation::TrimSecret(config.custom.endpoint)) &&
               (CloudTranslation::TrimSecret(config.custom.api_key).empty() ||
                CloudTranslation::IsUsableSecret(config.custom.api_key));
    return provider == "tencent" && CloudTranslation::IsUsableSecret(config.tencent.secret_id) &&
           CloudTranslation::IsUsableSecret(config.tencent.secret_key);
}
std::vector<std::string> Translate(const CandidateTranslationConfig &config, const std::vector<std::string> &texts,
                                   const std::string &source, const std::string &target,
                                   const std::function<bool()> &cancelled)
{
    const auto provider = CloudTranslation::EffectiveProvider(config);
    if (provider == "ai")
        return AiTranslation::TextTranslateBatch(config, texts, source, target, cancelled);
    if (provider == "custom")
        return CustomTranslation::TextTranslateBatch(
            {CloudTranslation::TrimSecret(config.custom.endpoint), CloudTranslation::TrimSecret(config.custom.api_key)},
            texts, source, target);
    if (provider == "niutrans")
        return NiuTransTranslation::TextTranslateBatch({CloudTranslation::TrimSecret(config.niutrans.app_id),
                                                        CloudTranslation::TrimSecret(config.niutrans.apikey)},
                                                       texts, source, target);
    if (provider == "tencent")
        return TencentTmt::TextTranslateBatch({CloudTranslation::TrimSecret(config.tencent.secret_id),
                                               CloudTranslation::TrimSecret(config.tencent.secret_key),
                                               config.tencent.region},
                                              texts, source, target);
    return std::vector<std::string>(texts.size());
}
void TranslateGroup(const CandidateTranslationConfig &config, const std::vector<EnglishIme::TranslationQuery> &group,
                    const std::string &source, const std::string &target, uint64_t job, uint64_t generation,
                    std::vector<EnglishIme::TranslationResult> &results)
{
    if (group.empty() || !Current(config, job, generation))
        return;
    std::vector<std::string> texts;
    for (const auto &query : group)
        texts.push_back(query.key);
    const auto translated = Translate(config, texts, source, target, [&] { return !Current(config, job, generation); });
    if (!Current(config, job, generation))
        return;
    std::lock_guard lock(g_mutex);
    if (!Current(config, job, generation))
        return;
    for (size_t i = 0; i < group.size(); ++i)
    {
        const auto identity = Identity(group[i], Scope(config));
        auto gloss = CloudTranslation::FormatGloss(i < translated.size() ? translated[i] : std::string());
        if (gloss.empty())
        {
            if (g_negative.size() >= kMaxCacheEntries)
                g_negative.clear();
            g_negative[identity] = std::chrono::steady_clock::now() + kNegativeTtl;
            continue;
        }
        if (g_cache.size() >= kMaxCacheEntries)
            g_cache.clear();
        g_cache[identity] = gloss;
        g_negative.erase(identity);
        // Keep machine results scoped. english.db has no provider metadata and must not be overwritten by AI output.
        results.push_back({group[i].key, group[i].direction, std::move(gloss)});
    }
}
void WorkerLoop()
{
    uint64_t observed = UINT64_MAX;
    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] { return !g_running || g_job != observed; });
        if (!g_running)
            break;
        observed = g_job;
        auto queries = g_latest_queries;
        auto generation = g_latest_generation;
        auto target = g_last_request + kIdleDelay;
        while (g_running && g_cv.wait_until(lock, target, [&] { return !g_running || g_job != observed; }))
        {
            if (!g_running)
                return;
            observed = g_job;
            queries = g_latest_queries;
            generation = g_latest_generation;
            target = g_last_request + kIdleDelay;
        }
        lock.unlock();
        const auto config = GetConfiguredCandidateTranslationSnapshot();
        if (queries.empty() || !Current(config, observed, generation) || !Usable(config))
            continue;
        std::vector<EnglishIme::TranslationQuery> en_zh, zh_target;
        std::vector<EnglishIme::TranslationResult> results;
        {
            std::lock_guard cacheLock(g_mutex);
            for (const auto &query : queries)
            {
                if (CloudTranslation::Utf8Length(query.key) > 40)
                    continue;
                const auto identity = Identity(query, Scope(config));
                if (const auto found = g_cache.find(identity); found != g_cache.end())
                {
                    results.push_back({query.key, query.direction, found->second});
                    continue;
                }
                if (const auto found = g_negative.find(identity); found != g_negative.end())
                {
                    if (found->second > std::chrono::steady_clock::now())
                        continue;
                    g_negative.erase(found);
                }
                if (query.direction == EnglishIme::TranslationDirection::EnglishToChinese &&
                    CloudTranslation::IsCloudTranslatableEnglish(query.key))
                    en_zh.push_back(query);
                else if (query.direction == EnglishIme::TranslationDirection::ChineseToEnglish &&
                         CloudTranslation::IsCloudTranslatableChinese(query.key))
                    zh_target.push_back(query);
            }
        }
        // English candidates always receive Chinese explanations, independently of the Chinese candidate target.
        TranslateGroup(config, en_zh, "en", "zh", observed, generation, results);
        TranslateGroup(config, zh_target, "zh", config.tencent.target_language, observed, generation, results);
        if (!results.empty() && Current(config, observed, generation) && g_callback)
            g_callback(std::move(results), generation, Scope(config));
    }
}
} // namespace
namespace CloudTranslation
{
std::string EffectiveProvider(const CandidateTranslationConfig &config)
{
    if (config.provider != "legacy")
        return config.provider;
    if (config.niutrans.enabled)
        return "niutrans";
    if (config.custom.enabled)
        return "custom";
    return config.tencent.enabled ? "tencent" : "dictionary";
}
std::string ConfigurationScope()
{
    return Scope(GetConfiguredCandidateTranslationSnapshot());
}
void Start(const std::string &, ApplyCallback callback)
{
    if (g_running)
        return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_callback = std::move(callback);
    g_running = true;
    g_worker = std::thread(WorkerLoop);
}
void Stop()
{
    if (!g_running)
        return;
    g_running = false;
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
    g_callback = {};
}
void RequestMisses(std::vector<EnglishIme::TranslationQuery> queries, uint64_t generation)
{
    if (!GetConfiguredCandidateTranslationCloudEnabled())
        queries.clear();
    {
        std::lock_guard lock(g_mutex);
        g_latest_queries = std::move(queries);
        g_latest_generation = generation;
        g_last_request = std::chrono::steady_clock::now();
        ++g_job;
    }
    g_cv.notify_one();
}
void Clear()
{
    RequestMisses({}, 0);
}
std::string LookupCache(const std::string &key, EnglishIme::TranslationDirection direction)
{
    if (!GetConfiguredCandidateTranslationCloudEnabled())
        return {};
    const auto config = GetConfiguredCandidateTranslationSnapshot();
    if (!Usable(config))
        return {};
    std::lock_guard lock(g_mutex);
    const auto found = g_cache.find(Identity({key, direction}, Scope(config)));
    return found == g_cache.end() ? std::string() : found->second;
}
} // namespace CloudTranslation
