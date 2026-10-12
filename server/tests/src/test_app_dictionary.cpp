#include "settings/personal_library.h"
#include "engine/core/data_path.h"
#include "tests/includes/test_framework.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
using nlohmann::json;
namespace
{
// Never borrow the installed profile: every file and voice reference below belongs to this test directory.
struct IsolatedLibraryProfile
{
    std::optional<std::wstring> original = metasequoia::detail::wide_environment_variable(L"METASEQUOIA_IME_DATA_DIR");
    std::filesystem::path parent = std::filesystem::temp_directory_path();
    std::filesystem::path root = parent / (L"msime-domain-catalog-fixture-" + std::to_wstring(GetCurrentProcessId()) +
                                           L"-" + std::to_wstring(GetTickCount64()));
    IsolatedLibraryProfile()
    {
        REQUIRE(root.is_absolute());
        REQUIRE(!std::filesystem::exists(root));
        std::filesystem::create_directories(root / "domain-input-sources");
        REQUIRE_EQ(_wputenv_s(L"METASEQUOIA_IME_DATA_DIR", root.c_str()), 0);
    }
    ~IsolatedLibraryProfile()
    {
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", original ? original->c_str() : L"");
        // Only recursively remove the fixed-prefix child created by this fixture, never an inherited data directory.
        std::error_code error;
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().wstring().find(L"msime-domain-catalog-fixture-") == 0)
            std::filesystem::remove_all(root, error);
    }
    void WriteCatalog(const std::string &text) const
    {
        std::ofstream file(root / "domain-input-sources" / "catalog.json", std::ios::binary);
        file << text;
        REQUIRE(file.good());
    }
    std::string StoredLibrary() const
    {
        std::ifstream file(root / "personal-library.json", std::ios::binary);
        REQUIRE(file.good());
        return {std::istreambuf_iterator<char>(file), {}};
    }
};
json ValidDomainCatalog()
{
    return {{"version", 1},
            {"kind", "msime-domain-input"},
            {"books",
             {{{"id", "idioms"},
               {"name", "成语"},
               {"entries", 1},
               {"bytes", 24},
               {"license", "MIT AND Unicode-3.0"},
               {"source", "qingjian/THUOCL + Unihan"},
               {"version", std::string(40, 'a')},
               {"sha256", std::string(64, 'b')},
               {"exportable", true}}}}};
}
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

TEST_CASE(domain_input_sets_inherit_replace_preserve_and_export_with_application)
{
    AppLibraryBackup backup;
    const char *app = "FixtureDomains.exe";
    REQUIRE_EQ(PersonalLibrary::EffectiveDomainDictionaries(app, "idioms,it"), std::string("idioms,it"));
    auto request = json{{"action", "setApplicationDomains"},
                        {"application", app},
                        {"domain_mode", "replace"},
                        {"domains", json::array({"law", "medicine", "law"})}};
    REQUIRE(PersonalLibrary::Handle(request, nullptr).at("ok") == true);
    REQUIRE_EQ(PersonalLibrary::EffectiveDomainDictionaries(app, "idioms,it"), std::string("law,medicine"));
    REQUIRE_EQ(PersonalLibrary::EffectiveDomainDictionaries("OtherApp.exe", "idioms,it"), std::string("idioms,it"));
    REQUIRE(ConfigureApp(app).at("ok") == true);
    REQUIRE_EQ(PersonalLibrary::EffectiveDomainDictionaries(app, "idioms,it"), std::string("law,medicine"));
    const auto exported = PersonalLibrary::Handle({{"action", "exportApplication"}, {"application", app}}, nullptr);
    REQUIRE(json::parse(exported.at("exported").get<std::string>()).at("profile").at("domains") ==
            json::array({"law", "medicine"}));
    REQUIRE(PersonalLibrary::Handle({{"action", "importApplication"},
                                     {"application", "DomainImported.exe"},
                                     {"payload", exported.at("exported")}},
                                    nullptr)
                .at("ok") == true);
    REQUIRE_EQ(PersonalLibrary::EffectiveDomainDictionaries("DomainImported.exe", "idioms,it"),
               std::string("law,medicine"));
    request["domains"] = json::array();
    REQUIRE(PersonalLibrary::Handle(request, nullptr).at("ok") == true);
    REQUIRE(PersonalLibrary::EffectiveDomainDictionaries(app, "idioms,it").empty());
    request["domains"] = json::array({"../invalid"});
    REQUIRE(PersonalLibrary::Handle(request, nullptr).at("ok") == false);
    REQUIRE(PersonalLibrary::EffectiveDomainDictionaries(app, "idioms,it").empty());
    request["domains"] = json::array();
    request["domain_mode"] = "inherit";
    REQUIRE(PersonalLibrary::Handle(request, nullptr).at("ok") == true);
    REQUIRE_EQ(PersonalLibrary::EffectiveDomainDictionaries(app, "idioms,it"), std::string("idioms,it"));
}

TEST_CASE(optional_domain_catalog_failure_preserves_real_library_saves_and_reads)
{
    IsolatedLibraryProfile profile;
    profile.WriteCatalog("{broken-json");
    const auto requireSaved = [](const json &request) {
        const auto result = PersonalLibrary::Handle(request, nullptr);
        REQUIRE(result.at("ok") == true);
        REQUIRE(result.at("domainWarning").is_string());
        REQUIRE(!result.at("domainWarning").get<std::string>().empty());
        REQUIRE(result.at("books").empty());
        REQUIRE(result.at("book").is_object());
        REQUIRE(result.at("history").is_array());
        return result;
    };
    auto group = requireSaved({{"action", "putGroup"}, {"name", "测试分组"}});
    const auto groupId = group.at("book").at("groups").back().at("id");
    requireSaved({{"action", "putClip"},
                  {"id", "fixture-clip"},
                  {"name", "测试常用语"},
                  {"group", groupId},
                  {"text", "原有常用语仍能保存"}});
    const std::string artifact = "{00000000-0000-0000-0000-000000000017}";
    std::filesystem::create_directories(profile.root / "voice-outbox");
    {
        // Metadata persistence only: no playback, microphone, clipboard or message sending is invoked.
        std::ofstream voice(profile.root / "voice-outbox" / (artifact + ".wav"), std::ios::binary);
        voice << "RIFF-test-owned-reference";
        REQUIRE(voice.good());
    }
    requireSaved({{"action", "favorite"}, {"id", "fixture-voice"}, {"name", "测试语音收藏"}, {"artifactId", artifact}});
    requireSaved({{"action", "review"}, {"text", "context"}, {"language", "en"}, {"grade", 2}});
    requireSaved({{"action", "putApplication"}, {"application", "CatalogFixture.exe"}, {"enabled", true}});
    const auto stored = json::parse(profile.StoredLibrary());
    REQUIRE_EQ(stored.at("groups").size(), size_t(3));
    REQUIRE_EQ(stored.at("clips").size(), size_t(1));
    REQUIRE_EQ(stored.at("clips").front().at("text"), "原有常用语仍能保存");
    REQUIRE_EQ(stored.at("favorites").front().at("file"), artifact + ".wav");
    REQUIRE_EQ(stored.at("reviews").at("en\ncontext").at("interval"), 2);
    REQUIRE_EQ(stored.at("applications").front().at("application"), "catalogfixture.exe");
    REQUIRE_EQ(requireSaved({{"action", "status"}}).at("book"), stored);
    // A later read from the actual disk file must retain the same records when the optional pack is repaired.
    profile.WriteCatalog(ValidDomainCatalog().dump());
    const auto repaired = PersonalLibrary::Handle({{"action", "status"}}, nullptr);
    REQUIRE(repaired.at("ok") == true);
    REQUIRE(!repaired.contains("domainWarning"));
    REQUIRE_EQ(repaired.at("book"), stored);
    REQUIRE_EQ(repaired.at("books").size(), size_t(1));
}

TEST_CASE(domain_catalog_schema_errors_fail_before_domain_mutation_or_export)
{
    IsolatedLibraryProfile profile;
    REQUIRE(PersonalLibrary::Handle({{"action", "putGroup"}, {"name", "保留资料"}}, nullptr).at("ok") == true);
    const auto saved = profile.StoredLibrary();
    const auto valid = ValidDomainCatalog();
    std::vector<json> malformed;
    auto bad = valid;
    bad["books"] = json::array({nullptr});
    malformed.push_back(bad);
    bad = valid;
    bad["books"][0].erase("entries");
    malformed.push_back(bad);
    bad = valid;
    bad["books"][0]["bytes"] = -1;
    malformed.push_back(bad);
    bad = valid;
    bad["books"][0]["exportable"] = "true";
    malformed.push_back(bad);
    bad = valid;
    bad["books"].push_back(bad["books"][0]);
    malformed.push_back(bad);
    bad = valid;
    bad["books"][0]["id"] = "../invalid";
    malformed.push_back(bad);
    bad = valid;
    bad["books"][0]["sha256"] = "missing-digest";
    malformed.push_back(bad);
    for (const auto &catalog : malformed)
    {
        profile.WriteCatalog(catalog.dump());
        for (const auto &request : std::vector<json>{{{"action", "domainCatalog"}},
                                                     {{"action", "exportDomain"}, {"id", "idioms"}},
                                                     {{"action", "setApplicationDomains"},
                                                      {"application", "InvalidCatalog.exe"},
                                                      {"domain_mode", "replace"},
                                                      {"domains", json::array({"idioms"})}}})
        {
            const auto result = PersonalLibrary::Handle(request, nullptr);
            REQUIRE(result.at("ok") == false);
            REQUIRE(!result.contains("exported"));
            REQUIRE_EQ(profile.StoredLibrary(), saved);
        }
        const auto status = PersonalLibrary::Handle({{"action", "status"}}, nullptr);
        REQUIRE(status.at("ok") == true);
        REQUIRE(status.contains("domainWarning"));
        REQUIRE(status.at("books").empty());
    }
    profile.WriteCatalog(valid.dump());
    {
        std::ofstream file(profile.root / "domain-input-sources" / "idioms.tsv", std::ios::binary);
        file << "一心一意\tyi xin yi yi\t1\n";
        REQUIRE(file.good());
    }
    const auto exported = PersonalLibrary::Handle({{"action", "exportDomain"}, {"id", "idioms"}}, nullptr);
    REQUIRE(exported.at("ok") == true);
    REQUIRE_EQ(exported.at("filename"), "msime-domain-idioms.tsv");
    REQUIRE(exported.at("exported").get<std::string>().find("一心一意\tyi xin yi yi\t1") != std::string::npos);
    bad = valid;
    bad["books"][0]["exportable"] = false;
    profile.WriteCatalog(bad.dump());
    REQUIRE(PersonalLibrary::Handle({{"action", "exportDomain"}, {"id", "idioms"}}, nullptr).at("ok") == false);
    std::filesystem::remove(profile.root / "domain-input-sources" / "catalog.json");
    const auto legacy = PersonalLibrary::Handle({{"action", "status"}}, nullptr);
    REQUIRE(legacy.at("ok") == true);
    REQUIRE(!legacy.contains("domainWarning"));
    REQUIRE_EQ(profile.StoredLibrary(), saved);
}

TEST_CASE(optional_application_display_errors_preserve_saved_records_and_failed_save_is_honest)
{
    IsolatedLibraryProfile profile;
    REQUIRE(ConfigureApp("ValidDisplay.exe").at("ok") == true);
    const auto original = json::parse(profile.StoredLibrary());
    const std::vector<json> invalidProfiles = {
        123,
        json::array(),
        nullptr,
        {{"application", "../unsafe.exe"}},
        {{"application", "InvalidSwitch.exe"}, {"enabled", "yes"}},
        {{"application", "InvalidDomains.exe"}, {"domain_mode", "replace"}, {"domains", 42}}};
    for (const auto &invalid : invalidProfiles)
    {
        auto fixture = original;
        fixture["applications"].push_back(invalid);
        {
            std::ofstream file(profile.root / "personal-library.json", std::ios::binary);
            file << fixture.dump();
            REQUIRE(file.good());
        }
        const auto response = PersonalLibrary::Handle({{"action", "putGroup"}, {"name", "仍可保存的分组"}}, nullptr);
        REQUIRE(response.at("ok") == true);
        REQUIRE(response.at("message").get<std::string>().find("原记录仍保留") != std::string::npos);
        REQUIRE_EQ(response.at("applications").size(), size_t(1));
        REQUIRE_EQ(response.at("book").at("applications"), original.at("applications"));
        const auto stored = json::parse(profile.StoredLibrary());
        REQUIRE_EQ(stored.at("groups").size(), size_t(3));
        REQUIRE_EQ(stored.at("groups").back().at("name"), "仍可保存的分组");
        REQUIRE_EQ(stored.at("applications"), fixture.at("applications"));
        const auto status = PersonalLibrary::Handle({{"action", "status"}}, nullptr);
        REQUIRE(status.at("ok") == true);
        REQUIRE_EQ(status.at("book").at("groups"), stored.at("groups"));
        REQUIRE_EQ(profile.StoredLibrary(), stored.dump());
    }
    const auto before = profile.StoredLibrary();
    auto blockedTemp = profile.root / "personal-library.json";
    blockedTemp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    REQUIRE(std::filesystem::create_directory(blockedTemp));
    const auto failed = PersonalLibrary::Handle({{"action", "putGroup"}, {"name", "不得部分保存"}}, nullptr);
    REQUIRE(failed.at("ok") == false);
    REQUIRE_EQ(profile.StoredLibrary(), before);
    REQUIRE(failed.at("message").get<std::string>().find("保存资料库失败") != std::string::npos);
}
