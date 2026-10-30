#pragma once
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace PromptLibrary
{
inline bool Validate(const std::string &raw)
{
    if (raw.empty())
        return true;
    if (raw.size() > 512 * 1024)
        return false;
    try
    {
        const auto value = nlohmann::json::parse(raw);
        if (value.at("version") != 1 || !value.at("items").is_array() || value.at("items").empty() ||
            value.at("items").size() > 128)
            return false;
        std::set<std::string> ids;
        for (const auto &item : value.at("items"))
        {
            const auto id = item.at("id").get<std::string>();
            const auto name = item.at("name").get<std::string>();
            const auto prompt = item.at("prompt").get<std::string>();
            if (id.empty() || id.size() > 100 || name.empty() || name.size() > 240 || prompt.empty() ||
                prompt.size() > 16384 || !ids.insert(id).second)
                return false;
        }
        return ids.count(value.at("selected").get<std::string>()) != 0;
    }
    catch (...)
    {
        return false;
    }
}
inline std::string Active(const std::string &raw)
{
    if (raw.empty() || !Validate(raw))
        return {};
    const auto value = nlohmann::json::parse(raw);
    for (const auto &item : value.at("items"))
        if (item.at("id") == value.at("selected"))
            return item.at("prompt").get<std::string>();
    return {};
}
} // namespace PromptLibrary
