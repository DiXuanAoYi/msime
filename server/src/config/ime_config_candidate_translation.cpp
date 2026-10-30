#include "config/ime_config_candidate_translation.h"
#include "config/ime_config_internal.h"
#include "ai/ai_task.h"
#include <algorithm>

using namespace ime_config_detail;
namespace
{
CandidateTranslationConfig g_translation;
const std::vector<std::string> kProviders{"deepseek", "openai", "siliconflow", "groq", "custom", "local"};
bool KnownProvider(const std::string &value)
{
    return std::find(kProviders.begin(), kProviders.end(), value) != kProviders.end();
}
bool KnownSelection(const std::string &value)
{
    return value == "legacy" || value == "dictionary" || value == "tencent" || value == "custom" ||
           value == "niutrans" || value == "ai";
}
std::string Fingerprint(const CandidateTranslationConfig &c)
{
    std::string out;
    const auto add = [&](const std::string &v) { out += std::to_string(v.size()) + ":" + v; };
    for (const auto *v : {&c.provider, &c.ai_provider, &c.endpoint, &c.token, &c.model, &c.tencent.secret_id,
                          &c.tencent.secret_key, &c.tencent.region, &c.tencent.target_language, &c.custom.endpoint,
                          &c.custom.api_key, &c.niutrans.app_id, &c.niutrans.apikey})
        add(*v);
    out += c.tencent.enabled ? '1' : '0';
    out += c.custom.enabled ? '1' : '0';
    out += c.niutrans.enabled ? '1' : '0';
    for (const auto *map : {&c.tokens, &c.endpoints, &c.models})
        for (const auto &[key, value] : *map)
        {
            add(key);
            add(value);
        }
    return out;
}
CandidateTranslationConfig SnapshotUnlocked()
{
    auto result = g_translation;
    result.tencent = g_tencent_tmt;
    result.custom = g_custom_translation;
    result.niutrans = g_niutrans;
    return result;
}
} // namespace
namespace ime_config_detail
{
std::shared_mutex g_candidate_translation_mutex;
void TouchCandidateTranslationRevision()
{
    ++g_translation.revision;
}
void LoadCandidateTranslationConfig(const toml::table &tbl)
{
    std::unique_lock lock(g_candidate_translation_mutex);
    const auto before = Fingerprint(SnapshotUnlocked());
    g_tencent_tmt.enabled = tbl["tencent_tmt"]["enabled"].value_or(true);
    g_tencent_tmt.secret_id = tbl["tencent_tmt"]["secret_id"].value_or(std::string());
    g_tencent_tmt.secret_key = tbl["tencent_tmt"]["secret_key"].value_or(std::string());
    g_tencent_tmt.region = tbl["tencent_tmt"]["region"].value_or(std::string("ap-guangzhou"));
    if (g_tencent_tmt.region.empty())
        g_tencent_tmt.region = "ap-guangzhou";
    g_tencent_tmt.target_language = tbl["tencent_tmt"]["target_language"].value_or(std::string("en"));
    const std::vector<std::string> languages{"en", "fr", "ja", "es", "ru", "de", "ko", "vi", "th", "it"};
    if (std::find(languages.begin(), languages.end(), g_tencent_tmt.target_language) == languages.end())
        g_tencent_tmt.target_language = "en";
    g_custom_translation.enabled = tbl["custom_translation"]["enabled"].value_or(false);
    g_custom_translation.endpoint = tbl["custom_translation"]["endpoint"].value_or(std::string());
    g_custom_translation.api_key = tbl["custom_translation"]["api_key"].value_or(std::string());
    g_niutrans.enabled = tbl["niutrans"]["enabled"].value_or(false);
    g_niutrans.app_id = tbl["niutrans"]["app_id"].value_or(std::string());
    g_niutrans.apikey = tbl["niutrans"]["apikey"].value_or(std::string());
    const auto section = tbl["candidate_translation"];
    CandidateTranslationConfig next;
    next.provider = section["provider"].value_or(std::string("legacy"));
    if (!KnownSelection(next.provider))
        next.provider = "legacy";
    next.ai_provider = section["ai_provider"].value_or(std::string("deepseek"));
    if (!KnownProvider(next.ai_provider))
        next.ai_provider = "deepseek";
    const AiAssistantConfig defaults;
    next.endpoints = defaults.endpoints;
    next.models = defaults.models;
    for (const auto &provider : kProviders)
    {
        next.tokens[provider] = section["token_" + provider].value_or(std::string());
        next.endpoints[provider] = section["endpoint_" + provider].value_or(next.endpoints[provider]);
        next.models[provider] = section["model_" + provider].value_or(next.models[provider]);
    }
    // An explicitly stored empty value is preserved, including an intentionally empty Custom slot.
    next.token = section["token"].value_or(next.tokens[next.ai_provider]);
    next.endpoint = section["endpoint"].value_or(next.endpoints[next.ai_provider]);
    next.model = section["model"].value_or(next.models[next.ai_provider]);
    next.tokens[next.ai_provider] = next.token;
    next.endpoints[next.ai_provider] = next.endpoint;
    next.models[next.ai_provider] = next.model;
    next.revision = g_translation.revision;
    g_translation = std::move(next);
    if (Fingerprint(SnapshotUnlocked()) != before)
        TouchCandidateTranslationRevision();
}
} // namespace ime_config_detail
CandidateTranslationConfig GetConfiguredCandidateTranslationSnapshot()
{
    std::shared_lock lock(g_candidate_translation_mutex);
    return SnapshotUnlocked();
}
bool SetConfiguredCandidateTranslationString(const std::string &key, const std::string &value)
{
    if (value.size() > 12000 || value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return false;
    std::unique_lock lock(g_candidate_translation_mutex);
    auto next = g_translation;
    std::vector<ConfigValueUpdate> updates;
    const auto add = [&](const std::string &name, const std::string &v) {
        updates.push_back({"candidate_translation", name, EscapeTomlBasicString(v)});
    };
    if (key == "provider")
    {
        if (!KnownSelection(value))
            return false;
        next.provider = value;
        add(key, value);
    }
    else if (key == "ai_provider")
    {
        if (!KnownProvider(value))
            return false;
        for (const auto &[name, current] : std::vector<std::pair<std::string, std::string>>{
                 {"token", next.token}, {"endpoint", next.endpoint}, {"model", next.model}})
            add(name + "_" + next.ai_provider, current);
        next.tokens[next.ai_provider] = next.token;
        next.endpoints[next.ai_provider] = next.endpoint;
        next.models[next.ai_provider] = next.model;
        next.ai_provider = value;
        next.token = next.tokens[value];
        next.endpoint = next.endpoints[value];
        next.model = next.models[value];
        add(key, value);
        add("token", next.token);
        add("endpoint", next.endpoint);
        add("model", next.model);
    }
    else
    {
        std::string field = key, provider = next.ai_provider;
        for (const auto &name : {"token", "endpoint", "model"})
            if (key.rfind(std::string(name) + "_", 0) == 0)
            {
                field = name;
                provider = key.substr(std::string(name).size() + 1);
                break;
            }
        if (!KnownProvider(provider) || (field != "token" && field != "endpoint" && field != "model"))
            return false;
        if (field == "endpoint" && provider == "local" && !value.empty() && !AiAssistant::IsLoopbackEndpoint(value))
            return false;
        auto &slots = field == "token" ? next.tokens : field == "endpoint" ? next.endpoints : next.models;
        slots[provider] = value;
        add(field + "_" + provider, value);
        if (provider == next.ai_provider)
        {
            auto &current = field == "token" ? next.token : field == "endpoint" ? next.endpoint : next.model;
            current = value;
            add(field, value);
        }
    }
    if (!WriteConfiguredValues(updates))
        return false;
    next.revision = g_translation.revision + 1;
    g_translation = std::move(next);
    return true;
}
