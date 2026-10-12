#include "settings/personal_library.h"
#include "engine/core/data_path.h"
#include "tests/includes/test_framework.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
using nlohmann::json;
namespace
{
struct AppLibraryBackup
{
    std::filesystem::path path = metasequoia::data_directory() / "personal-library.json";
    std::string original;
    bool existed = std::filesystem::exists(path);
    AppLibraryBackup()
    {
        if (existed)
        {
            std::ifstream f(path, std::ios::binary);
            original.assign(std::istreambuf_iterator<char>(f), {});
        }
    }
    ~AppLibraryBackup()
    {
        if (existed)
        {
            std::ofstream f(path, std::ios::binary);
            f << original;
        }
        else
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }
};
json ConfigureApp(const char *app, bool enabled = true, bool learn = true, bool frequency = true)
{
    return PersonalLibrary::Handle({{"action", "putApplication"},
                                    {"application", app},
                                    {"enabled", enabled},
                                    {"learn", learn},
                                    {"frequency", frequency}},
                                   nullptr);
}
} // namespace
TEST_CASE(app_dictionary_learning_and_ranking_do_not_cross_software)
{
    AppLibraryBackup backup;
    REQUIRE(ConfigureApp("FixtureAppA.exe").at("ok") == true);
    REQUIRE(ConfigureApp("FixtureAppB.exe").at("ok") == true);
    PersonalLibrary::RecordSelection("FixtureAppA.exe", "nihao", "你好", "zh");
    PersonalLibrary::RecordSelection("FixtureAppB.exe", "nihao", "拟好", "zh");
    std::vector<WordItem> a{{"nihao", "拟好", 10}, {"nihao", "你好", 1}}, b = a;
    PersonalLibrary::ApplyApplicationCandidates("FixtureAppA.exe", "nihao", a);
    PersonalLibrary::ApplyApplicationCandidates("FixtureAppB.exe", "nihao", b);
    REQUIRE_EQ(a.front().word, std::string("你好"));
    REQUIRE_EQ(b.front().word, std::string("拟好"));
    PersonalLibrary::RecordSelection("FixtureAppA.exe", "newterm", "软件专用新词", "zh", true);
    std::vector<WordItem> learned, unrelated;
    PersonalLibrary::ApplyApplicationCandidates("FixtureAppA.exe", "newterm", learned);
    PersonalLibrary::ApplyApplicationCandidates("FixtureAppB.exe", "newterm", unrelated);
    REQUIRE_EQ(learned.size(), size_t(1));
    REQUIRE(unrelated.empty());
    REQUIRE(learned.front().source == CandidateSource::AppDictionary);
}
TEST_CASE(app_dictionary_switches_preserve_learning_and_legacy_manual_rules)
{
    AppLibraryBackup backup;
    const char *app = "FixtureAppSwitch.exe";
    REQUIRE(ConfigureApp(app).at("ok") == true);
    REQUIRE(PersonalLibrary::Handle({{"action", "putRule"},
                                     {"id", "fixture-switch-rule"},
                                     {"name", "旧词条"},
                                     {"application", app},
                                     {"code", "oldterm"},
                                     {"text", "旧应用词条"}},
                                    nullptr)
                .at("ok") == true);
    PersonalLibrary::RecordSelection(app, "newterm", "保留学习词", "zh", true);
    REQUIRE(ConfigureApp(app, false).at("ok") == true);
    REQUIRE(PersonalLibrary::AppWords(app, "oldterm").empty());
    std::vector<WordItem> hidden;
    PersonalLibrary::ApplyApplicationCandidates(app, "newterm", hidden);
    REQUIRE(hidden.empty());
    REQUIRE(ConfigureApp(app, true, false, false).at("ok") == true);
    REQUIRE(!PersonalLibrary::AppWords(app, "oldterm").empty());
    PersonalLibrary::RecordSelection(app, "other", "不应学到", "zh", true);
    std::vector<WordItem> other;
    PersonalLibrary::ApplyApplicationCandidates(app, "other", other);
    REQUIRE(other.empty());
    PersonalLibrary::ApplyApplicationCandidates(app, "newterm", hidden);
    REQUIRE_EQ(hidden.size(), size_t(1));
}
TEST_CASE(app_dictionary_import_export_is_additive_deduplicated_and_transactional)
{
    AppLibraryBackup backup;
    const char *source = "FixtureExport.exe", *target = "FixtureImport.exe";
    REQUIRE(ConfigureApp(source).at("ok") == true);
    PersonalLibrary::RecordSelection(source, "term", "独立语句", "zh", true);
    auto exported = PersonalLibrary::Handle({{"action", "exportApplication"}, {"application", source}}, nullptr);
    REQUIRE(exported.at("ok") == true);
    json request = {{"action", "importApplication"}, {"application", target}, {"payload", exported.at("exported")}};
    REQUIRE(PersonalLibrary::Handle(request, nullptr).at("ok") == true);
    REQUIRE(PersonalLibrary::Handle(request, nullptr).at("ok") == true);
    std::vector<WordItem> candidates;
    PersonalLibrary::ApplyApplicationCandidates(target, "term", candidates);
    REQUIRE_EQ(candidates.size(), size_t(1));
    auto corrupt = json::parse(exported.at("exported").get<std::string>());
    corrupt["rules"].push_back({{"name", "损坏"}, {"code", "bad123"}, {"text", "不允许部分导入"}});
    request["payload"] = corrupt.dump();
    REQUIRE(PersonalLibrary::Handle(request, nullptr).at("ok") == false);
    auto after = PersonalLibrary::Handle({{"action", "exportApplication"}, {"application", target}}, nullptr);
    REQUIRE_EQ(json::parse(after.at("exported").get<std::string>()).at("learning").size(), size_t(1));
}
TEST_CASE(app_dictionary_candidate_preferences_never_affect_other_apps)
{
    AppLibraryBackup backup;
    const char *app = "FixturePreference.exe";
    REQUIRE(ConfigureApp(app).at("ok") == true);
    PersonalLibrary::SetCandidatePreference(app, "term", "第二词", false, "pin");
    std::vector<WordItem> local{{"term", "第一词", 10}, {"term", "第二词", 1}}, shared = local;
    PersonalLibrary::ApplyApplicationCandidates(app, "term", local);
    REQUIRE_EQ(local.front().word, std::string("第二词"));
    PersonalLibrary::ApplyApplicationCandidates("NoProfile.exe", "term", shared);
    REQUIRE_EQ(shared.front().word, std::string("第一词"));
    PersonalLibrary::SetCandidatePreference(app, "term", "第二词", false, "delete");
    PersonalLibrary::ApplyApplicationCandidates(app, "term", local);
    REQUIRE_EQ(local.size(), size_t(1));
    REQUIRE_EQ(shared.size(), size_t(2));
}
TEST_CASE(app_dictionary_learned_english_completes_prefix_and_shuangpin_uses_canonical_key)
{
    AppLibraryBackup backup;
    const char *app = "FixtureAppCode.exe";
    REQUIRE(ConfigureApp(app).at("ok") == true);
    PersonalLibrary::RecordSelection(app, "codexfixture", "codexfixture", "en", true);
    std::vector<WordItem> english, shuangpin;
    PersonalLibrary::ApplyApplicationCandidates(app, "cod", english, true);
    REQUIRE_EQ(english.size(), size_t(1));
    REQUIRE_EQ(english[0].word, std::string("codexfixture"));
    PersonalLibrary::RecordSelection(app, "shuishanshurufa", "水杉输入法", "zh", true);
    PersonalLibrary::ApplyApplicationCandidates(app, "uvujuufa", shuangpin, false, "shui'shan'shu'ru'fa");
    REQUIRE_EQ(shuangpin.size(), size_t(1));
    REQUIRE_EQ(shuangpin[0].word, std::string("水杉输入法"));
    std::vector<WordItem> unrelated;
    PersonalLibrary::ApplyApplicationCandidates("OtherSoftware.exe", "cod", unrelated, true);
    REQUIRE(unrelated.empty());
}
