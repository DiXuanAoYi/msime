#include "config/ime_config_internal.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

using namespace ime_config_detail;
namespace
{
std::string NormalizeApplication(std::string value)
{
    const auto separator = value.find_last_of("/\\");
    if (separator != std::string::npos)
        value.erase(0, separator + 1);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return value;
}
bool ValidateRules(const std::string &text)
{
    if (text.size() > 65536)
        return false;
    const auto rows = nlohmann::json::parse(text, nullptr, false);
    if (!rows.is_array() || rows.size() > 256)
        return false;
    std::set<std::string> names;
    for (const auto &row : rows)
    {
        if (!row.is_object() || row.size() != 2 || !row.contains("application") || !row.contains("mode") ||
            !row["application"].is_string() || !row["mode"].is_string())
            return false;
        const auto app = row["application"].get<std::string>();
        const auto mode = row["mode"].get<std::string>();
        if (app.empty() || app.size() > 260 || app.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
            app.find_first_of("/\\") != std::string::npos ||
            (mode != "inherit" && mode != "enabled" && mode != "disabled") ||
            !names.insert(NormalizeApplication(app)).second)
            return false;
    }
    return true;
}
} // namespace

bool GetConfiguredEnglishCandidates()
{
    return g_english_runtime_candidates.load();
}
bool SetConfiguredEnglishCandidates(bool enabled)
{
    if (!WriteConfiguredValue("input", "english_candidates", enabled ? "true" : "false"))
        return false;
    g_english_runtime_candidates.store(enabled);
    return true;
}
std::shared_ptr<const std::string> GetConfiguredEnglishCandidateAppRules()
{
    return std::atomic_load(&g_english_candidate_app_rules);
}
bool SetConfiguredEnglishCandidateAppRules(const std::string &rules)
{
    if (!ValidateRules(rules))
        return false;
    const std::string canonical = nlohmann::json::parse(rules).dump();
    if (!WriteConfiguredValue("input", "english_candidate_app_rules", EscapeTomlBasicString(canonical)))
        return false;
    std::atomic_store(&g_english_candidate_app_rules, std::make_shared<const std::string>(canonical));
    return true;
}
bool GetConfiguredEnglishCandidatesForApplication(const std::string &application)
{
    const auto snapshot = GetConfiguredEnglishCandidateAppRules();
    if (snapshot && ValidateRules(*snapshot))
    {
        const auto name = NormalizeApplication(application);
        for (const auto &row : nlohmann::json::parse(*snapshot))
            if (NormalizeApplication(row["application"].get<std::string>()) == name)
            {
                const auto mode = row["mode"].get<std::string>();
                if (mode != "inherit")
                    return mode == "enabled";
                break;
            }
    }
    return GetConfiguredEnglishCandidates();
}
std::shared_ptr<const std::string> GetConfiguredEnabledDomainDictionaries()
{
    return std::atomic_load(&g_enabled_domain_dictionaries);
}
bool SetConfiguredEnabledDomainDictionaries(const std::string &ids)
{
    if (ids.size() > 1024)
        return false;
    const std::set<std::string> known{"animals", "automotive", "finance",  "food",   "historical", "idioms",
                                      "it",      "law",        "medicine", "places", "poetry"};
    std::set<std::string> chosen;
    std::stringstream stream(ids);
    std::string id;
    while (std::getline(stream, id, ','))
    {
        if (!known.count(id))
            return false;
        chosen.insert(id);
    }
    if (!ids.empty() && ids.back() == ',')
        return false;
    std::string canonical;
    for (const auto &entry : chosen)
    {
        if (!canonical.empty())
            canonical += ',';
        canonical += entry;
    }
    if (!WriteConfiguredValue("input", "enabled_domain_dictionaries", EscapeTomlBasicString(canonical)))
        return false;
    std::atomic_store(&g_enabled_domain_dictionaries, std::make_shared<const std::string>(canonical));
    return true;
}

bool GetConfiguredTrilingualCycleEnabled()
{
    return g_trilingual_cycle_enabled.load();
}
bool GetConfiguredFloatingToolbarShadow()
{
    return g_floating_toolbar_shadow.load();
}
bool SetConfiguredFloatingToolbarShadow(bool enabled)
{
    if (!WriteConfiguredValue("general", "floating_toolbar_shadow", enabled ? "true" : "false"))
        return false;
    g_floating_toolbar_shadow.store(enabled);
    return true;
}
const std::string &GetConfiguredCandidateFitContentMode()
{
    static const std::string follow = "follow", enabled = "enabled", disabled = "disabled";
    const int mode = g_candidate_fit_content_mode.load();
    return mode == 1 ? enabled : mode == 2 ? disabled : follow;
}
bool SetConfiguredCandidateFitContentMode(const std::string &mode)
{
    if (mode != "follow" && mode != "enabled" && mode != "disabled")
        return false;
    if (!WriteConfiguredValue("appearance", "candidate_fit_content", EscapeTomlBasicString(mode)))
        return false;
    g_candidate_fit_content_mode.store(mode == "enabled" ? 1 : mode == "disabled" ? 2 : 0);
    return true;
}
bool SetConfiguredTrilingualCycleEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "trilingual_cycle", enabled ? "true" : "false"))
        return false;
    if (g_trilingual_cycle_enabled.exchange(enabled) != enabled)
        g_active_input_mode.store(ActiveInputMode::Configured);
    RefreshEffectiveTsfPreeditStyle();
    return true;
}
const std::string &GetActiveInputMode()
{
    static const std::string chinese = "chinese", japanese = "japanese";
    switch (g_active_input_mode.load())
    {
    case ActiveInputMode::Chinese:
        return chinese;
    case ActiveInputMode::Japanese:
        return japanese;
    default:
        return g_configured_input_mode_japanese.load() ? japanese : chinese;
    }
}
bool SetActiveInputMode(const std::string &mode)
{
    if (!GetConfiguredTrilingualCycleEnabled() || (mode != "chinese" && mode != "japanese"))
        return false;
    g_active_input_mode.store(mode == "japanese" ? ActiveInputMode::Japanese : ActiveInputMode::Chinese);
    RefreshEffectiveTsfPreeditStyle();
    return true;
}
bool GetConfiguredWubiFourCodeAutoCommit()
{
    return g_wubi_four_code_auto_commit.load();
}
bool SetConfiguredWubiFourCodeAutoCommit(bool enabled)
{
    if (!WriteConfiguredValue("input", "wubi_four_code_auto_commit", enabled ? "true" : "false"))
        return false;
    g_wubi_four_code_auto_commit.store(enabled);
    return true;
}
bool GetConfiguredWubiFifthCodeTopCommit()
{
    return g_wubi_fifth_code_top_commit.load();
}
bool SetConfiguredWubiFifthCodeTopCommit(bool enabled)
{
    if (!WriteConfiguredValue("input", "wubi_fifth_code_top_commit", enabled ? "true" : "false"))
        return false;
    g_wubi_fifth_code_top_commit.store(enabled);
    return true;
}
std::shared_ptr<const std::string> GetConfiguredVimModeApps()
{
    return std::atomic_load(&g_vim_mode_apps);
}
bool SetConfiguredVimModeApps(const std::string &text)
{
    if (text.size() > 65536)
        return false;
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array() || parsed.size() > 256)
        return false;
    std::set<std::string> names;
    for (const auto &item : parsed)
    {
        if (!item.is_string())
            return false;
        const auto value = item.get<std::string>();
        if (value.empty() || value.size() > 260 || value.find_first_of("/\\\r\n\0", 0, 5) != std::string::npos)
            return false;
        names.insert(NormalizeApplication(value));
    }
    const std::string canonical = nlohmann::json(names).dump();
    if (!WriteConfiguredValue("input", "vim_mode_apps", EscapeTomlBasicString(canonical)))
        return false;
    std::atomic_store(&g_vim_mode_apps, std::make_shared<const std::string>(canonical));
    return true;
}
bool GetConfiguredVimModeForApplication(const std::string &application)
{
    const auto parsed = nlohmann::json::parse(*GetConfiguredVimModeApps(), nullptr, false);
    if (!parsed.is_array() || parsed.size() > 256)
        return false;
    for (const auto &item : parsed)
        if (item.is_string() && NormalizeApplication(item.get<std::string>()) == NormalizeApplication(application))
            return true;
    return false;
}

namespace
{
std::mutex grammar_mutex;
gram::GrammarConfig grammar_config;
bool ValidGrammar(const gram::GrammarConfig &value)
{
    if (value.collocation_max_length < 1 || value.collocation_max_length > 8 || value.collocation_min_length < 1 ||
        value.collocation_min_length > value.collocation_max_length)
        return false;
    for (const auto penalty :
         {value.collocation_penalty, value.non_collocation_penalty, value.weak_collocation_penalty, value.rear_penalty})
        if (!std::isfinite(penalty) || penalty < -1000 || penalty > 0)
            return false;
    return true;
}
} // namespace
gram::GrammarConfig GetConfiguredCollocationGrammar()
{
    std::lock_guard lock(grammar_mutex);
    return grammar_config;
}
bool SetConfiguredCollocationGrammar(const gram::GrammarConfig &value)
{
    if (!ValidGrammar(value))
        return false;
    if (!WriteConfiguredValues(
            {{"association", "collocation_max_length", std::to_string(value.collocation_max_length)},
             {"association", "collocation_min_length", std::to_string(value.collocation_min_length)},
             {"association", "collocation_penalty", std::to_string(value.collocation_penalty)},
             {"association", "non_collocation_penalty", std::to_string(value.non_collocation_penalty)},
             {"association", "weak_collocation_penalty", std::to_string(value.weak_collocation_penalty)},
             {"association", "rear_penalty", std::to_string(value.rear_penalty)}}))
        return false;
    std::lock_guard lock(grammar_mutex);
    grammar_config = value;
    return true;
}
namespace ime_config_detail
{
void LoadConfiguredCollocationGrammar(const toml::table &table)
{
    gram::GrammarConfig next;
    next.collocation_max_length = table["association"]["collocation_max_length"].value_or(next.collocation_max_length);
    next.collocation_min_length = table["association"]["collocation_min_length"].value_or(next.collocation_min_length);
    next.collocation_penalty = table["association"]["collocation_penalty"].value_or(next.collocation_penalty);
    next.non_collocation_penalty =
        table["association"]["non_collocation_penalty"].value_or(next.non_collocation_penalty);
    next.weak_collocation_penalty =
        table["association"]["weak_collocation_penalty"].value_or(next.weak_collocation_penalty);
    next.rear_penalty = table["association"]["rear_penalty"].value_or(next.rear_penalty);
    std::lock_guard lock(grammar_mutex);
    grammar_config = ValidGrammar(next) ? next : gram::GrammarConfig{};
}
} // namespace ime_config_detail
