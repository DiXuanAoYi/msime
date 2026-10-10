#include "english/english_ime.h"
#include "engine/english/english_dictionary.h"
#include "tests/includes/test_framework.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>

namespace
{
struct TranslationFixture
{
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("msime-glossary-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<EnglishIme::TranslationResult> results;
    uint64_t generation = 0;
    bool received = false;

    TranslationFixture()
    {
        std::filesystem::create_directories(directory);
        const auto path = (directory / "english.db").u8string();
        REQUIRE(EnglishDictionary::upsert_gloss(path, true, "开发", "built-in development"));
        REQUIRE(EnglishDictionary::upsert_gloss(path, false, "apple", "苹果"));
        EnglishIme::Start(path, {}, [this](auto values, uint64_t current) {
            std::lock_guard lock(mutex);
            results = std::move(values);
            generation = current;
            received = true;
            ready.notify_one();
        });
    }

    ~TranslationFixture()
    {
        EnglishIme::Stop();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    void Request(const std::string &language, std::shared_ptr<const EnglishIme::PersonalGlossary> glossary)
    {
        {
            std::lock_guard lock(mutex);
            received = false;
        }
        EnglishIme::RequestTranslations({{"开发", EnglishIme::TranslationDirection::ChineseToEnglish},
                                         {"apple", EnglishIme::TranslationDirection::EnglishToChinese}},
                                        language, std::move(glossary));
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, std::chrono::seconds(5), [this] { return received; }));
        REQUIRE_EQ(results.size(), size_t{2});
    }
};
} // namespace

TEST_CASE(personal_glossary_overrides_real_dictionary_and_language_switch_isolated)
{
    TranslationFixture fixture;
    auto personal = std::make_shared<EnglishIme::PersonalGlossary>();
    REQUIRE(personal->Parse("en\t开发\tpersonal development\nja\t开发\t開発\nes\t开发\tdesarrollo"));
    fixture.Request("en", personal);
    REQUIRE_EQ(fixture.results[0].gloss, std::string("personal development"));
    REQUIRE_EQ(fixture.results[1].gloss, std::string("苹果"));
    const auto old_generation = fixture.generation;
    fixture.Request("ja", personal);
    REQUIRE_EQ(fixture.results[0].gloss, std::string("開発"));
    REQUIRE_EQ(fixture.results[1].gloss, std::string("苹果"));
    REQUIRE(!EnglishIme::IsTranslationCurrent(old_generation));
    fixture.Request("es", personal);
    REQUIRE_EQ(fixture.results[0].gloss, std::string("desarrollo"));
    fixture.Request("fr", personal);
    REQUIRE(fixture.results[0].gloss.empty());
    REQUIRE_EQ(fixture.results[1].gloss, std::string("苹果"));
    fixture.Request("en", {});
    REQUIRE_EQ(fixture.results[0].gloss, std::string("built-in development"));
    const auto last_generation = fixture.generation;
    EnglishIme::ClearTranslations();
    REQUIRE(!EnglishIme::IsTranslationCurrent(last_generation));
}

TEST_CASE(personal_glossary_replacement_removes_previous_translation)
{
    TranslationFixture fixture;
    auto personal = std::make_shared<EnglishIme::PersonalGlossary>();
    REQUIRE(personal->Parse("ja\t开发\t開発"));
    fixture.Request("ja", personal);
    REQUIRE_EQ(fixture.results[0].gloss, std::string("開発"));
    fixture.Request("ja", std::make_shared<const EnglishIme::PersonalGlossary>());
    REQUIRE(fixture.results[0].gloss.empty());
}
