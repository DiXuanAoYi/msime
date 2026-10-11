#pragma once
#include "settings/vision_config.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
namespace Vision
{
std::string Base64(const std::string &bytes);
std::string Unbase64(const std::string &text);
nlohmann::json NormalizeOcr(const std::string &provider, const nlohmann::json &body, const std::string &task);
nlohmann::json RecognizeRemote(const VisionConfig::Config &config, const std::string &image, const std::string &task,
                               const std::function<bool()> &cancelled);
std::string Translate(const std::string &text, const std::string &source, const std::string &target,
                      const std::function<bool()> &cancelled);
} // namespace Vision
