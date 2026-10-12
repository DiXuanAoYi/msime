#include "cloud/ai_translation.h"
#include "cloud/cloud_translation.h"
#include "english/english_ime.h"
#include "engine/english/english_dictionary.h"
#include "config/ime_config.h"
#include <Windows.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include "ai/ai_task.h"

int RunCandidateTranslationFixtureProbe(const char *endpoint, const char *reportPath)
{
    if (!AiAssistant::IsLoopbackEndpoint(endpoint) ||
        !GetEnvironmentVariableW(L"METASEQUOIA_IME_CONFIG_DIR", nullptr, 0) ||
        !GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", nullptr, 0))
        return 2;
    wchar_t directory[32768]{};
    GetEnvironmentVariableW(L"METASEQUOIA_IME_CONFIG_DIR", directory, 32768);
    const std::filesystem::path root(directory);
    std::filesystem::create_directories(root);
    std::filesystem::copy_file(MSIME_DEFAULT_CONFIG_PATH, root / L"config.default.toml",
                               std::filesystem::copy_options::overwrite_existing);
    // Owned synthetic credentials only. This probe never reads the installed config or any user service file.
    std::ofstream(root / L"config.toml") << "[general]\ncandidate_translation_cloud = false\n";
    InitImeConfig();
    if (!SetConfiguredCandidateTranslationString("ai_provider", "local") ||
        !SetConfiguredCandidateTranslationString("endpoint", endpoint) ||
        !SetConfiguredCandidateTranslationString("model", "fixture-a") ||
        !SetConfiguredCandidateTranslationString("provider", "ai"))
        return 2;
    auto config = GetConfiguredCandidateTranslationSnapshot();
    const auto thai = AiTranslation::TextTranslateBatch(config, {"你好"}, "zh", "th");
    const auto italian = AiTranslation::TextTranslateBatch(config, {"你好"}, "zh", "it");
    const auto db = (root / L"english.db").u8string();
    EnglishDictionary::ensure_schema(db);
    EnglishDictionary::upsert_gloss(db, false, "hello", "你好 · 本地英文释义");
    auto personal = std::make_shared<EnglishIme::PersonalGlossary>();
    personal->Parse("th\t本地\t个人泰语释义");
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<EnglishIme::TranslationResult> local;
    std::vector<std::string> cloudGlosses;
    bool scopeValid = true;
    CloudTranslation::Start(db, [&](auto results, uint64_t, const std::string &scope) {
        std::lock_guard lock(mutex);
        scopeValid = scopeValid && scope == CloudTranslation::ConfigurationScope();
        for (const auto &item : results)
            cloudGlosses.push_back(item.gloss);
        ready.notify_all();
    });
    EnglishIme::Start(db, {}, [&](auto results, uint64_t generation) {
        std::vector<EnglishIme::TranslationQuery> misses;
        {
            std::lock_guard lock(mutex);
            local = results;
            for (const auto &item : results)
                if (item.gloss.empty())
                    misses.push_back({item.key, item.direction});
        }
        CloudTranslation::RequestMisses(std::move(misses), generation);
        ready.notify_all();
    });
    SetConfiguredCandidateTranslationCloudEnabled(true);
    SetConfiguredTencentTmtString("target_language", "th");
    EnglishIme::RequestTranslations({{"本地", EnglishIme::TranslationDirection::ChineseToEnglish},
                                     {"hello", EnglishIme::TranslationDirection::EnglishToChinese},
                                     {"云端词", EnglishIme::TranslationDirection::ChineseToEnglish}},
                                    "th", personal);
    auto waitCount = [&](size_t count) {
        std::unique_lock lock(mutex);
        return ready.wait_for(lock, std::chrono::seconds(5), [&] { return cloudGlosses.size() >= count; });
    };
    const bool first = waitCount(1);
    bool localPriority = false;
    {
        std::lock_guard lock(mutex);
        localPriority =
            local.size() == 3 && local[0].gloss == "个人泰语释义" && local[1].gloss == "你好 · 本地英文释义";
    }
    SetConfiguredCandidateTranslationString("model", "fixture-b");
    SetConfiguredTencentTmtString("target_language", "it");
    EnglishIme::RequestTranslations({{"云端词", EnglishIme::TranslationDirection::ChineseToEnglish}}, "it");
    const bool second = waitCount(2);
    SetConfiguredCandidateTranslationString("model", "fixture-slow");
    EnglishIme::RequestTranslations({{"迟回词", EnglishIme::TranslationDirection::ChineseToEnglish}}, "it");
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    SetConfiguredCandidateTranslationString("model", "fixture-b");
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    bool rejectedLate;
    {
        std::lock_guard lock(mutex);
        rejectedLate = cloudGlosses.size() == 2;
    }
    const bool noWrongCache =
        CloudTranslation::LookupCache("迟回词", EnglishIme::TranslationDirection::ChineseToEnglish).empty();
    EnglishIme::RequestTranslations({{"迟回词", EnglishIme::TranslationDirection::ChineseToEnglish}}, "it");
    const bool third = waitCount(3);
    CloudTranslation::Stop();
    EnglishIme::Stop();
    EnglishDictionary dictionary(db);
    const bool noDatabaseWrite = dictionary.query_english_gloss("云端词").empty();
    const bool aligned =
        thai.size() == 1 && thai[0] == "fixture-a:th:你好" && italian.size() == 1 && italian[0] == "fixture-a:it:你好";
    const bool workerResults = cloudGlosses.size() == 3 && cloudGlosses[0] == "fixture-a:th:云端词" &&
                               cloudGlosses[1] == "fixture-b:it:云端词" && cloudGlosses[2] == "fixture-b:it:迟回词";
    const bool ok = aligned && first && second && third && localPriority && rejectedLate && noWrongCache &&
                    noDatabaseWrite && workerResults && scopeValid;
    nlohmann::json report = {{"ok", ok},
                             {"directThIt", aligned},
                             {"localPersonalFirst", localPriority},
                             {"lateScopeRejected", rejectedLate},
                             {"noWrongScopeCache", noWrongCache},
                             {"englishDbPreserved", noDatabaseWrite},
                             {"workerResults", workerResults},
                             {"callbackScopesCurrent", scopeValid},
                             {"cloudGlosses", cloudGlosses},
                             {"source", "owned synthetic loopback HTTP fixture, not a real cloud provider or model"}};
    std::ofstream output(std::filesystem::u8path(reportPath));
    output << report.dump(2);
    return ok && output ? 0 : 1;
}
