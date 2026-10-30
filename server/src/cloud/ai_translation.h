#pragma once
#include "config/ime_config.h"
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace AiTranslation
{
bool IsUsableConfig(const CandidateTranslationConfig &config);
nlohmann::json BuildRequest(const CandidateTranslationConfig &config, const std::vector<std::string> &texts,
                            const std::string &source, const std::string &target);
std::vector<std::string> ParseTranslations(const std::string &content, size_t expected);
std::vector<std::string> TextTranslateBatch(const CandidateTranslationConfig &config,
                                            const std::vector<std::string> &texts, const std::string &source,
                                            const std::string &target, const std::function<bool()> &cancelled = {});
} // namespace AiTranslation
