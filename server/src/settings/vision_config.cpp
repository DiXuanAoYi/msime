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
    return {{"provider", "windows"},
            {"local_model", "glm-ocr"},
            {"task", "document"},
            {"max_tokens", "4096"},
            {"shiliu_appcode", ""},
            {"tencent_secret_id", ""},
            {"tencent_secret_key", ""},
            {"tencent_region", "ap-guangzhou"},
            {"baidu_api_key", ""},
            {"baidu_secret_key", ""},
            {"translation_provider", "dictionary"},
            {"source_language", "auto"},
            {"target_language", "en"},
            {"capture_x", "false"},
            {"capture_y", "true"},
            {"screen_interval_ms", "2000"}};
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
    if (key == "provider" && !std::set<std::string>{"windows", "local", "shiliu", "tencent", "baidu"}.count(value))
        return false;
    if (key == "task" && !std::set<std::string>{"text", "document", "table", "formula", "code"}.count(value))
        return false;
    if (key == "translation_provider" &&
        !std::set<std::string>{"text_model", "tencent", "custom", "niutrans", "dictionary"}.count(value))
        return false;
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
