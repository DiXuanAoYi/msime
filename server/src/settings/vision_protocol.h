#pragma once
#include "settings/vision_config.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>
namespace Vision
{
std::string Base64(const std::string &bytes);
std::string Unbase64(const std::string &text);
void ReconstructCodeIndentation(nlohmann::json &result);
nlohmann::json NormalizeOcr(const std::string &provider, const nlohmann::json &body, const std::string &task);
nlohmann::json RecognizeRemote(const VisionConfig::Config &config, const std::string &image, const std::string &task,
                               const std::function<bool()> &cancelled);
std::string Translate(const std::string &text, const std::string &source, const std::string &target,
                      const std::function<bool()> &cancelled);
std::vector<std::string> ParseBlockTranslations(const std::string &content, size_t count);
std::vector<std::string> TranslateBlocks(const std::vector<std::string> &texts, const std::string &source,
                                         const std::string &target, const std::function<bool()> &cancelled);
} // namespace Vision
