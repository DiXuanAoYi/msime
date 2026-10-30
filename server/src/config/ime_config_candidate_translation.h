#pragma once
#include <toml++/toml.h>
#include <shared_mutex>

namespace ime_config_detail
{
extern std::shared_mutex g_candidate_translation_mutex;
void LoadCandidateTranslationConfig(const toml::table &table);
void TouchCandidateTranslationRevision(); // caller holds the exclusive mutex
} // namespace ime_config_detail
