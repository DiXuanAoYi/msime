#pragma once
#include <functional>
#include <string>
#include <nlohmann/json.hpp>
namespace Vision
{
std::string SubtitleTime(double seconds);
void RunSubtitles(bool playback, bool translate, const std::string &source, const std::string &target,
                  const std::function<bool()> &cancelled, const std::function<bool()> &finished,
                  const std::function<void(const nlohmann::json &, const std::string &)> &update);
} // namespace Vision
