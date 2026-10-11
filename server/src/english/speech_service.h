#pragma once
#include <nlohmann/json.hpp>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <toml++/toml.h>

namespace Speech
{
using Config = std::map<std::string, std::string>;
Config Defaults();
Config GetConfig();
void LoadConfig(const toml::table &table);
bool SetConfig(const std::string &key, const std::string &value);
// Only an explicit click or preview queues speech. No input/commit/send APIs.
void Speak(std::string text, std::string language);
void Stop();
nlohmann::json Handle(const nlohmann::json &request);
struct Request
{
    std::string url, body;
    std::vector<std::string> headers;
};
Request BuildRequest(const Config &config, const std::string &text, const std::string &language, int64_t timestamp);
std::string DecodeAudio(const std::string &provider, const std::string &body);
} // namespace Speech
