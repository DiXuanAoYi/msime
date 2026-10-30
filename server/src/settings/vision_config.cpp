#include "settings/vision_config.h"
#include "config/ime_config_internal.h"
#include <mutex>
#include <set>
namespace VisionConfig
{
namespace
{
std::mutex mutex;
Config current;
} // namespace
Config Defaults()
{
    return {{"provider", "smart"},
            {"local_model", "glm-ocr"},
            {"task", "document"},
            {"max_tokens", "4096"},
            {"shiliu_appcode", ""},
            {"tencent_secret_id", ""},
            {"tencent_secret_key", ""},
            {"tencent_region", "ap-guangzhou"},
            {"baidu_api_key", ""},
            {"baidu_secret_key", ""},
            {"translation_provider", "text_model"},
            {"source_language", "auto"},
            {"target_language", "en"},
            {"capture_x", "false"},
            {"capture_y", "true"},
            {"screen_interval_ms", "2000"},
            // Quick actions (global hotkeys). Every value is a string like the rest of [vision].
            {"after_ocr_copy", "true"},
            {"after_ocr_card", "true"},
            {"line_mode", "smart"},
            {"cjk_space", "false"},
            {"table_format", "excel"},
            {"auto_swap", "true"},
            {"zh_target", "en"},
            {"overlay_style", "cover"},
            {"hold_alt_original", "true"},
            {"skip_code", "true"},
            {"live_refresh", "change"},
            {"use_glossary", "true"},
            {"enter_action", "copy"},
            {"magnifier", "true"},
            {"hide_toolbar", "true"},
            {"history_enabled", "true"},
            {"history_days", "30"},
            {"text_provider", ""}};
}
Config Get()
{
    std::lock_guard lock(mutex);
    return current.empty() ? Defaults() : current;
}
void Load(const toml::table &table)
{
    auto config = Defaults();
    for (auto &[k, v] : config)
        v = table["vision"][k].value_or(v);
    std::lock_guard lock(mutex);
    current = std::move(config);
}
bool Set(const std::string &key, const std::string &value)
{
    if (!Defaults().count(key) || value.size() > 4096)
        return false;
    if (key == "provider" &&
        !std::set<std::string>{"smart", "windows", "local", "shiliu", "tencent", "baidu"}.count(value))
        return false;
    if (key == "task" && !std::set<std::string>{"text", "document", "table", "formula", "code"}.count(value))
        return false;
    if (key == "translation_provider" &&
        !std::set<std::string>{"text_model", "tencent", "custom", "niutrans", "dictionary"}.count(value))
        return false;
    const auto one_of = [&](std::initializer_list<const char *> allowed) {
        for (const char *item : allowed)
            if (value == item)
                return true;
        return false;
    };
    if ((key == "after_ocr_copy" || key == "after_ocr_card" || key == "cjk_space" || key == "auto_swap" ||
         key == "hold_alt_original" || key == "skip_code" || key == "use_glossary" || key == "magnifier" ||
         key == "hide_toolbar" || key == "history_enabled") &&
        !one_of({"true", "false"}))
        return false;
    if (key == "line_mode" && !one_of({"smart", "keep", "one"}))
        return false;
    if (key == "table_format" && !one_of({"excel", "md", "html"}))
        return false;
    if (key == "overlay_style" && !one_of({"cover", "card"}))
        return false;
    if (key == "live_refresh" && !one_of({"change", "2s", "5s"}))
        return false;
    if (key == "enter_action" && !one_of({"copy", "save", "ocr"}))
        return false;
    if (key == "text_provider" && !one_of({"", "siliconflow", "openai", "deepseek", "groq", "aliyun", "tencent"}))
        return false;
    if (key == "history_days")
    {
        try
        {
            size_t end = 0;
            const int n = std::stoi(value, &end);
            if (end != value.size() || n < 0 || n > 3650)
                return false;
        }
        catch (...)
        {
            return false;
        }
    }
    if (key == "capture_x" || key == "capture_y")
    {
        if (value != "true" && value != "false")
            return false;
        auto c = Get();
        c[key] = value;
        if (c["capture_x"] == "false" && c["capture_y"] == "false")
            return false;
    }
    if (key == "max_tokens" || key == "screen_interval_ms")
    {
        try
        {
            size_t end = 0;
            int n = std::stoi(value, &end);
            if (end != value.size() || n < (key == "max_tokens" ? 128 : 1500) ||
                n > (key == "max_tokens" ? 8192 : 30000))
                return false;
        }
        catch (...)
        {
            return false;
        }
    }
    if (!ime_config_detail::WriteConfiguredValue("vision", key, ime_config_detail::EscapeTomlBasicString(value)))
        return false;
    std::lock_guard lock(mutex);
    if (current.empty())
        current = Defaults();
    current[key] = value;
    return true;
}
} // namespace VisionConfig
