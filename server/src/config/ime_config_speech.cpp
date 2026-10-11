#include "config/ime_config_internal.h"
#include "english/speech_service.h"
#include <set>
#include <cmath>

namespace Speech
{
namespace
{
std::mutex mutex;
Config current;
} // namespace
Config Defaults()
{
    return {{"enabled", "true"},
            {"provider", "sapi"},
            {"rate", "1.0"},
            {"volume", "100"},
            {"sapi_voice_zh", ""},
            {"sapi_voice_en", ""},
            {"azure_key", ""},
            {"azure_region", "eastasia"},
            {"azure_voice_zh", "zh-CN-XiaoxiaoNeural"},
            {"azure_voice_en", "en-US-JennyNeural"},
            {"volcengine_app_id", ""},
            {"volcengine_key", ""},
            {"volcengine_resource", "seed-tts-2.0"},
            {"volcengine_voice_zh", "zh_female_vv_uranus_bigtts"},
            {"volcengine_voice_en", "en_female_amanda_mars_bigtts"},
            {"tencent_secret_id", ""},
            {"tencent_secret_key", ""},
            {"tencent_region", "ap-guangzhou"},
            {"tencent_voice_zh", "101001"},
            {"tencent_voice_en", "101050"},
            {"elevenlabs_key", ""},
            {"elevenlabs_model", "eleven_flash_v2_5"},
            {"elevenlabs_voice_zh", ""},
            {"elevenlabs_voice_en", ""},
            {"local_endpoint", "http://127.0.0.1:18767/v1/audio/speech"},
            {"local_model", "kokoro"},
            {"local_voice_zh", "zf_xiaobei"},
            {"local_voice_en", "af_heart"}};
}
Config GetConfig()
{
    std::lock_guard lock(mutex);
    return current.empty() ? Defaults() : current;
}
bool ActivateLocalModel(const std::string &model, bool resetVoices)
{
    Config changes{
        {"provider", "local"}, {"local_endpoint", "http://127.0.0.1:18767/v1/audio/speech"}, {"local_model", model}};
    if (resetVoices)
    {
        changes["local_voice_zh"] = model == "edge-tts"    ? "zh-CN-XiaoxiaoNeural"
                                    : model == "kokoro"    ? "zf_xiaobei"
                                    : model == "moss-nano" ? "default"
                                                           : "Vivian";
        changes["local_voice_en"] = model == "edge-tts"    ? "en-US-JennyNeural"
                                    : model == "kokoro"    ? "af_heart"
                                    : model == "moss-nano" ? "default"
                                                           : "Ryan";
    }
    std::vector<ime_config_detail::ConfigValueUpdate> updates;
    for (const auto &[key, value] : changes)
        updates.push_back({"speech", key, ime_config_detail::EscapeTomlBasicString(value)});
    // The existing cross-process writer atomically replaces the entire file.
    if (!ime_config_detail::WriteConfiguredValues(updates))
        return false;
    std::lock_guard lock(mutex);
    if (current.empty())
        current = Defaults();
    for (const auto &[key, value] : changes)
        current[key] = value;
    return true;
}
void LoadConfig(const toml::table &table)
{
    auto next = Defaults();
    for (auto &[key, value] : next)
        value = table["speech"][key].value_or(value);
    std::lock_guard lock(mutex);
    current = std::move(next);
}
bool SetConfig(const std::string &key, const std::string &value)
{
    if (!Defaults().count(key) || value.size() > 2000 || value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return false;
    if (key == "provider" &&
        !std::set<std::string>{"sapi", "local", "azure", "volcengine", "tencent", "elevenlabs"}.count(value))
        return false;
    if (key == "enabled" && value != "true" && value != "false")
        return false;
    if (key == "rate" || key == "volume")
    {
        try
        {
            size_t end = 0;
            double number = std::stod(value, &end);
            if (end != value.size() || !std::isfinite(number) || number < (key == "rate" ? 0.5 : 0) ||
                number > (key == "rate" ? 2.0 : 100))
                return false;
        }
        catch (...)
        {
            return false;
        }
    }
    if (!ime_config_detail::WriteConfiguredValue("speech", key, ime_config_detail::EscapeTomlBasicString(value)))
        return false;
    std::lock_guard lock(mutex);
    if (current.empty())
        current = Defaults();
    current[key] = value;
    return true;
}
} // namespace Speech
