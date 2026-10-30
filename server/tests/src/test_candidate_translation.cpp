#include "tests/includes/test_framework.h"
#include "cloud/ai_translation.h"
#include "cloud/cloud_translation.h"
#include "config/credential_protection.h"
#include "settings/config_update_dispatch.h"
#include "settings/neural_models.h"
#include "settings/webview_reply.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>

namespace
{
class IsolatedTranslationConfig
{
    std::map<std::wstring, std::optional<std::wstring>> previous_;

  public:
    std::filesystem::path root;
    IsolatedTranslationConfig()
    {
        root = std::filesystem::temp_directory_path() /
               (L"msime-candidate-translation-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        for (const auto name : {L"METASEQUOIA_IME_DATA_DIR", L"METASEQUOIA_IME_CONFIG_DIR", L"LOCALAPPDATA"})
        {
            wchar_t buffer[32768]{};
            const auto size = GetEnvironmentVariableW(name, buffer, 32768);
            previous_[name] = size ? std::optional<std::wstring>(std::wstring(buffer, size)) : std::nullopt;
            SetEnvironmentVariableW(name, root.c_str());
            _wputenv_s(name, root.c_str()); // engine data_directory uses the CRT environment snapshot
        }
        std::filesystem::copy_file(MSIME_DEFAULT_CONFIG_PATH, root / L"config.default.toml",
                                   std::filesystem::copy_options::overwrite_existing);
        std::ofstream(root / L"config.toml")
            << "[general]\ncandidate_translation_cloud = false\n"
               "[custom_translation]\nenabled = true\nendpoint = \"https://old.example.test/translate\"\napi_key = "
               "\"synthetic-old-key\"\n"
               "[niutrans]\nenabled = true\napp_id = \"synthetic-id\"\napikey = \"synthetic-niu-key\"\n";
        InitImeConfig();
    }
    ~IsolatedTranslationConfig()
    {
        for (const auto &[key, value] : previous_)
        {
            SetEnvironmentVariableW(key.c_str(), value ? value->c_str() : nullptr);
            _wputenv_s(key.c_str(), value ? value->c_str() : L"");
        }
        // The runner is required to pin DATA_DIR and CONFIG_DIR. Never reload installed user configuration here.
        if (previous_[L"METASEQUOIA_IME_DATA_DIR"] && previous_[L"METASEQUOIA_IME_CONFIG_DIR"])
            try
            {
                InitImeConfig();
            }
            catch (...)
            {
            }
    }
};
} // namespace
TEST_CASE(candidate_translation_legacy_combination_has_one_effective_provider)
{
    for (int flags = 0; flags < 8; ++flags)
    {
        CandidateTranslationConfig c;
        c.tencent.enabled = flags & 1;
        c.custom.enabled = flags & 2;
        c.niutrans.enabled = flags & 4;
        REQUIRE_EQ(CloudTranslation::EffectiveProvider(c), std::string(flags & 4   ? "niutrans"
                                                                       : flags & 2 ? "custom"
                                                                       : flags & 1 ? "tencent"
                                                                                   : "dictionary"));
        c.provider = "ai";
        REQUIRE_EQ(CloudTranslation::EffectiveProvider(c), std::string("ai"));
        c.provider = "dictionary";
        REQUIRE_EQ(CloudTranslation::EffectiveProvider(c), std::string("dictionary"));
    }
}
TEST_CASE(candidate_translation_remote_requires_real_credentials_and_local_has_exact_boundary)
{
    CandidateTranslationConfig c;
    REQUIRE(!AiTranslation::IsUsableConfig(c));
    c.token = "<YOUR_AI_TOKEN>";
    REQUIRE(!AiTranslation::IsUsableConfig(c));
    c.token = "FAKESECRET_example";
    REQUIRE(!AiTranslation::IsUsableConfig(c));
    c.ai_provider = "local";
    c.endpoint = "http://127.0.0.1:18769/v1/chat/completions";
    c.token = "";
    REQUIRE(AiTranslation::IsUsableConfig(c));
    c.endpoint = "http://127.0.0.1.example.test/v1/chat/completions";
    REQUIRE(!AiTranslation::IsUsableConfig(c));
}
TEST_CASE(candidate_translation_strict_alignment_prevents_corrupted_glosses)
{
    const auto aligned = AiTranslation::ParseTranslations(R"({"translations":["a","b"]})", 2);
    REQUIRE_EQ(aligned.at(1), std::string("b"));
    for (const auto &bad : {R"({"translations":["a"]})", R"({"translations":["a",3]})",
                            R"({"translations":["a","b","c"]})", "Here is a translation"})
    {
        const auto rejected = AiTranslation::ParseTranslations(bad, 2);
        REQUIRE_EQ(rejected.at(0), std::string());
    }
    CandidateTranslationConfig c;
    const auto body = AiTranslation::BuildRequest(c, {"你好"}, "zh", "th");
    REQUIRE(body["thinking"]["type"] == "disabled");
    REQUIRE(body["messages"][0]["content"].get<std::string>().find("to th") != std::string::npos);
    REQUIRE(nlohmann::json::parse(body["messages"][1]["content"].get<std::string>()).is_array());
}
TEST_CASE(candidate_translation_preserves_upgrade_flags_isolates_slots_and_encrypts_credentials)
{
    IsolatedTranslationConfig fixture;
    const auto before = GetConfiguredCandidateTranslationSnapshot();
    REQUIRE_EQ(before.provider, std::string("legacy"));
    REQUIRE(before.custom.enabled);
    REQUIRE(before.niutrans.enabled);
    REQUIRE(!GetConfiguredCandidateTranslationCloudEnabled());
    REQUIRE(SetConfiguredCandidateTranslationString("provider", "ai"));
    REQUIRE(SetConfiguredCandidateTranslationString("token", "synthetic-translation-ai-key"));
    REQUIRE(SetConfiguredCandidateTranslationString("model", "custom-model-a"));
    const auto first = GetConfiguredCandidateTranslationSnapshot();
    REQUIRE(first.revision > before.revision);
    REQUIRE(SetConfiguredCandidateTranslationString("ai_provider", "custom"));
    REQUIRE(GetConfiguredCandidateTranslationSnapshot().token.empty());
    REQUIRE(SetConfiguredCandidateTranslationString("endpoint", "https://custom.example.test/v1/chat/completions"));
    REQUIRE(SetConfiguredCandidateTranslationString("token", "synthetic-custom-key"));
    REQUIRE(SetConfiguredCandidateTranslationString("ai_provider", "deepseek"));
    const auto restored = GetConfiguredCandidateTranslationSnapshot();
    REQUIRE_EQ(restored.token, std::string("synthetic-translation-ai-key"));
    REQUIRE_EQ(restored.model, std::string("custom-model-a"));
    REQUIRE(restored.custom.enabled);
    REQUIRE(restored.niutrans.enabled);
    REQUIRE(!GetConfiguredCandidateTranslationCloudEnabled());
    REQUIRE(GetConfiguredAiAssistantSnapshot().token.empty());
    REQUIRE(GetConfiguredVoiceInput().polish_token.empty());
    const auto stored = toml::parse_file((fixture.root / L"config.toml").string());
    REQUIRE(CredentialProtection::IsProtected(stored["candidate_translation"]["token"].value_or(std::string())));
    REQUIRE(CredentialProtection::IsProtected(stored["candidate_translation"]["token_custom"].value_or(std::string())));
    REQUIRE(SetConfiguredTencentTmtString("target_language", "th"));
    REQUIRE(SetConfiguredTencentTmtString("target_language", "it"));
    const auto revision = GetConfiguredCandidateTranslationSnapshot().revision;
    InitImeConfig();
    REQUIRE_EQ(GetConfiguredCandidateTranslationSnapshot().tencent.target_language, std::string("it"));
    REQUIRE_EQ(GetConfiguredCandidateTranslationSnapshot().revision, revision);
    REQUIRE(SettingsConfig::ApplyConfigUpdate({{"path", "candidate_translation.provider"}, {"value", "dictionary"}}));
    REQUIRE_EQ(GetConfiguredCandidateTranslationSnapshot().provider, std::string("dictionary"));
}
TEST_CASE(neural_status_without_models_is_read_only_and_has_valid_native_reply)
{
    IsolatedTranslationConfig fixture;
    const auto models = fixture.root / L"models" / L"neural";
    REQUIRE(!std::filesystem::exists(models));
    const auto response = NeuralModels::Handle({{"requestId", "native-status-fixture"}, {"action", "status"}});
    REQUIRE(response["ok"] == true);
    REQUIRE(response["models"].size() == 3);
    REQUIRE(response["models"][0]["state"] == "absent");
    const auto serialized = SettingsWebView::SerializeReply(response);
    REQUIRE(!serialized.empty());
    REQUIRE(!std::filesystem::exists(models));
    // These existing open-object contracts are consumed by the same native validator.
    const auto library = SettingsWebView::SerializeReply({{"type", "personalLibraryResponse"},
                                                          {"requestId", "fixture"},
                                                          {"action", "catalog"},
                                                          {"ok", true},
                                                          {"message", ""},
                                                          {"book", nlohmann::json::object()}});
    REQUIRE(!library.empty());
}
