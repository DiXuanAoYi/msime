#include "settings/feature_config.h"
#include "config/ime_config_internal.h"
#include "utils/hotkey_combo.h"
#include <atomic>
#include <mutex>
#include <set>
namespace FeatureConfig
{
namespace
{
constexpr const char *kHotkeyActions[] = {"shot", "ocr", "translate", "selection", "live", "reply", "rewrite", "clip"};

std::mutex g_mutex;
std::map<std::string, Values> g_current;
std::atomic<uint64_t> g_revision{1};

const std::map<std::string, Values> &AllDefaults()
{
    static const std::map<std::string, Values> defaults = {{"hotkeys",
                                                            {{"enabled", "true"},
                                                             {"shot", "Ctrl+Alt+X"},
                                                             {"ocr", "Ctrl+Alt+O"},
                                                             {"translate", "Ctrl+Alt+T"},
                                                             {"selection", "Ctrl+Alt+Y"},
                                                             {"live", "Ctrl+Alt+Shift+T"},
                                                             {"reply", "Ctrl+Alt+R"},
                                                             {"rewrite", "Ctrl+Alt+G"},
                                                             {"clip", "Ctrl+Alt+V"}}},
                                                           {"reply",
                                                            {{"count", "3"},
                                                             {"length", "medium"},
                                                             {"source", "auto"},
                                                             {"context", "6"},
                                                             {"fill_mode", "insert"},
                                                             {"confirm_payload", "false"},
                                                             {"provider", ""},
                                                             {"reader_enabled", "false"}}},
                                                           {"clipboard",
                                                            {{"sort", "recent"},
                                                             {"max_items", "200"},
                                                             {"retention_days", "30"},
                                                             {"respect_privacy_flag", "true"},
                                                             {"exclude_apps", "KeePass.exe;1Password.exe"},
                                                             {"pause_on_lock", "true"}}}};
    return defaults;
}

bool IsBool(const std::string &value)
{
    return value == "true" || value == "false";
}

bool InRange(const std::string &value, int low, int high)
{
    try
    {
        size_t end = 0;
        const int n = std::stoi(value, &end);
        return end == value.size() && n >= low && n <= high;
    }
    catch (...)
    {
        return false;
    }
}

bool OneOf(const std::string &value, std::initializer_list<const char *> allowed)
{
    for (const char *item : allowed)
        if (value == item)
            return true;
    return false;
}

// Two actions must never share a combination: RegisterHotKey would fail for the second one.
bool HotkeyConflict(const std::string &key, const std::string &value)
{
    if (value.empty())
        return false;
    const auto normalized = HotkeyCombo::Normalize(value);
    std::lock_guard lock(g_mutex);
    const auto found = g_current.find("hotkeys");
    const auto existing = found == g_current.end() ? AllDefaults().at("hotkeys") : found->second;
    for (const auto &[name, current] : existing)
        if (name != key && name != "enabled" && !current.empty() && HotkeyCombo::Normalize(current) == normalized)
            return true;
    return false;
}

bool Valid(const std::string &section, const std::string &key, const std::string &value)
{
    if (value.size() > 1024)
        return false;
    if (section == "hotkeys")
    {
        if (key == "enabled")
            return IsBool(value);
        return HotkeyCombo::IsValid(value);
    }
    if (section == "reply")
    {
        if (key == "count")
            return OneOf(value, {"3", "5"});
        if (key == "length")
            return OneOf(value, {"short", "medium", "long"});
        if (key == "source")
            return OneOf(value, {"auto", "selection", "clipboard"});
        if (key == "context")
            return OneOf(value, {"0", "6", "20"});
        if (key == "fill_mode")
            return OneOf(value, {"insert", "copy"});
        if (key == "confirm_payload" || key == "reader_enabled")
            return IsBool(value);
        if (key == "provider")
            return OneOf(value, {"", "siliconflow", "openai", "deepseek", "groq", "aliyun", "tencent"});
    }
    if (section == "clipboard")
    {
        if (key == "sort")
            return OneOf(value, {"recent", "frequent"});
        if (key == "max_items")
            return InRange(value, 20, 500);
        if (key == "retention_days")
            return InRange(value, 0, 3650);
        if (key == "respect_privacy_flag" || key == "pause_on_lock")
            return IsBool(value);
        if (key == "exclude_apps")
            return value.find_first_of("\r\n\"\\") == std::string::npos;
    }
    return false;
}
} // namespace

Values Defaults(const std::string &section)
{
    const auto found = AllDefaults().find(section);
    return found == AllDefaults().end() ? Values{} : found->second;
}

Values Get(const std::string &section)
{
    std::lock_guard lock(g_mutex);
    const auto found = g_current.find(section);
    return found == g_current.end() ? Defaults(section) : found->second;
}

std::string Get(const std::string &section, const std::string &key)
{
    const auto values = Get(section);
    const auto found = values.find(key);
    return found == values.end() ? std::string() : found->second;
}

bool GetBool(const std::string &section, const std::string &key)
{
    return Get(section, key) == "true";
}

int GetInt(const std::string &section, const std::string &key)
{
    try
    {
        return std::stoi(Get(section, key));
    }
    catch (...)
    {
        return 0;
    }
}

void Load(const toml::table &table)
{
    std::map<std::string, Values> next;
    for (const auto &[section, defaults] : AllDefaults())
    {
        auto values = defaults;
        for (auto &[key, value] : values)
        {
            const auto text = table[section][key].value_or(value);
            // A hand-edited file may hold something Set would have rejected; keep the default then.
            if (Valid(section, key, text))
                value = text;
        }
        next[section] = std::move(values);
    }
    {
        std::lock_guard lock(g_mutex);
        g_current = std::move(next);
    }
    ++g_revision;
}

bool Set(const std::string &section, const std::string &key, const std::string &value)
{
    const auto defaults = AllDefaults().find(section);
    if (defaults == AllDefaults().end() || !defaults->second.count(key) || !Valid(section, key, value) ||
        (section == "hotkeys" && key != "enabled" && HotkeyConflict(key, value)))
        return false;
    const std::string stored =
        section == "hotkeys" && key != "enabled" && !value.empty() ? HotkeyCombo::Normalize(value) : value;
    if (!ime_config_detail::WriteConfiguredValue(section, key, ime_config_detail::EscapeTomlBasicString(stored)))
        return false;
    {
        std::lock_guard lock(g_mutex);
        auto &values = g_current[section];
        if (values.empty())
            values = Defaults(section);
        values[key] = stored;
    }
    ++g_revision;
    return true;
}

uint64_t Revision()
{
    return g_revision.load();
}

const char *const *HotkeyActions(size_t &count)
{
    count = sizeof(kHotkeyActions) / sizeof(kHotkeyActions[0]);
    return kHotkeyActions;
}
} // namespace FeatureConfig
