#pragma once

#include "english/english_ime.h"
#include "config/ime_config.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace CloudTranslation
{
using ApplyCallback = std::function<void(std::vector<EnglishIme::TranslationResult> results, uint64_t generation,
                                         const std::string &configuration_scope)>;

void Start(const std::string &db_path, ApplyCallback apply_callback);
void Stop();
void RequestMisses(std::vector<EnglishIme::TranslationQuery> queries, uint64_t generation);
void Clear();
// One effective provider; explicit selection does not modify legacy flags or any other AI/workbench settings.
std::string EffectiveProvider(const CandidateTranslationConfig &config);
std::string ConfigurationScope();
std::string LookupCache(const std::string &key, EnglishIme::TranslationDirection direction);
} // namespace CloudTranslation
