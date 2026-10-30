#include "assistant/profile_store.h"
#include "clipboard/clipboard_history.h"
#include "config/ime_config.h"
#include "engine/core/data_path.h"
#include "settings/feature_config.h"
#include "settings/vision_service.h"
#include "tests/includes/test_framework.h"
#include "utils/hotkey_combo.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <toml++/toml.h>

using nlohmann::json;

namespace
{
// A private data directory: nothing here may touch the installed profile, its config or its reply memory.
struct QuickProfile
{
    std::optional<std::wstring> original = metasequoia::detail::wide_environment_variable(L"METASEQUOIA_IME_DATA_DIR");
    std::filesystem::path parent = [] {
        auto path = std::filesystem::temp_directory_path();
        return path.filename().empty() ? path.parent_path() : path;
    }();
    std::filesystem::path root = parent / (L"msime-quick-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                           std::to_wstring(GetTickCount64()));
    bool initialized = false;
    // initConfig points the config writer at this directory too; without it a setter would write to
    // whichever config file an earlier test left selected.
    explicit QuickProfile(const std::string &config = {}, bool initConfig = false)
    {
        REQUIRE(root.is_absolute());
        REQUIRE(!std::filesystem::exists(root));
        std::filesystem::create_directories(root);
        REQUIRE_EQ(_wputenv_s(L"METASEQUOIA_IME_DATA_DIR", root.c_str()), 0);
        std::ofstream(root / "config.toml") << config;
        if (initConfig)
        {
            InitImeConfig();
            initialized = true;
        }
    }
    ~QuickProfile()
    {
        _wputenv_s(L"METASEQUOIA_IME_DATA_DIR", original ? original->c_str() : L"");
        if (initialized)
            InitImeConfig(); // environment first, so the writer goes back to the real location
        std::error_code error;
        if (root.parent_path() == parent && root.filename().wstring().find(L"msime-quick-fixture-") == 0)
            std::filesystem::remove_all(root, error);
    }
};

json Profile(const json &request)
{
    auto copy = request;
    copy["requestId"] = "quick-test";
    return ReplyStudio::HandleProfileRequest(copy);
}

const json *FindById(const json &rows, const std::string &id)
{
    for (const auto &row : rows)
        if (row.value("id", "") == id)
            return &row;
    return nullptr;
}
} // namespace

TEST_CASE(hotkey_text_round_trips_and_rejects_keys_that_would_steal_typing)
{
    const auto combo = HotkeyCombo::Parse("ctrl+alt+x");
    REQUIRE(combo.has_value());
    REQUIRE_EQ(combo->modifiers, UINT(MOD_CONTROL | MOD_ALT));
    REQUIRE_EQ(combo->vk, UINT('X'));
    REQUIRE_EQ(HotkeyCombo::Format(*combo), std::string("Ctrl+Alt+X"));
    REQUIRE_EQ(HotkeyCombo::Normalize("Alt+Ctrl+o"), std::string("Ctrl+Alt+O"));
    REQUIRE_EQ(HotkeyCombo::Normalize("Shift+Alt+Ctrl+t"), std::string("Ctrl+Alt+Shift+T"));
    REQUIRE(HotkeyCombo::Parse("Ctrl+F9").has_value());
    REQUIRE(HotkeyCombo::Parse("Win+Alt+F12").has_value());
    // A bare letter or Shift+letter would make the key unusable for typing everywhere.
    REQUIRE(!HotkeyCombo::Parse("X").has_value());
    REQUIRE(!HotkeyCombo::Parse("Shift+X").has_value());
    REQUIRE(!HotkeyCombo::Parse("Ctrl+Alt").has_value());
    REQUIRE(!HotkeyCombo::Parse("Ctrl+Alt+Nonsense").has_value());
    REQUIRE(HotkeyCombo::IsValid("")); // an empty value means "no hotkey"
}

TEST_CASE(feature_config_validates_values_and_rejects_duplicate_hotkeys)
{
    QuickProfile profile("[hotkeys]\n", true);
    FeatureConfig::Load(toml::table{});
    REQUIRE_EQ(FeatureConfig::Get("hotkeys", "reply"), std::string("Ctrl+Alt+R"));
    REQUIRE_EQ(FeatureConfig::Get("reply", "count"), std::string("3"));
    REQUIRE_EQ(FeatureConfig::Get("clipboard", "sort"), std::string("recent"));
    REQUIRE(FeatureConfig::GetBool("hotkeys", "enabled"));
    REQUIRE(!FeatureConfig::GetBool("reply", "reader_enabled")); // reading other apps' chats is opt-in

    const auto before = FeatureConfig::Revision();
    REQUIRE(!FeatureConfig::Set("reply", "count", "4"));
    REQUIRE(!FeatureConfig::Set("reply", "unknown_key", "1"));
    REQUIRE(!FeatureConfig::Set("nope", "count", "3"));
    REQUIRE(!FeatureConfig::Set("hotkeys", "ocr", "not a hotkey"));
    REQUIRE(!FeatureConfig::Set("clipboard", "max_items", "5"));
    REQUIRE(!FeatureConfig::Set("clipboard", "exclude_apps", "bad\"quote"));
    REQUIRE_EQ(FeatureConfig::Revision(), before); // a refused write changes nothing
    REQUIRE_EQ(FeatureConfig::Get("reply", "count"), std::string("3"));

    REQUIRE(FeatureConfig::Set("reply", "count", "5"));
    REQUIRE_EQ(FeatureConfig::Get("reply", "count"), std::string("5"));
    REQUIRE(FeatureConfig::Revision() > before);
    // Another action already owns Ctrl+Alt+O (ocr).
    REQUIRE(!FeatureConfig::Set("hotkeys", "shot", "Ctrl+Alt+O"));
    REQUIRE(FeatureConfig::Set("hotkeys", "ocr", "alt+ctrl+k"));
    REQUIRE_EQ(FeatureConfig::Get("hotkeys", "ocr"), std::string("Ctrl+Alt+K"));
    REQUIRE(FeatureConfig::Set("hotkeys", "shot", "Ctrl+Alt+O")); // ocr let go of it
    REQUIRE(FeatureConfig::Set("hotkeys", "shot", ""));           // clearing is always allowed

    std::ifstream written(profile.root / "config.toml");
    const std::string text((std::istreambuf_iterator<char>(written)), {});
    REQUIRE(text.find("count = \"5\"") != std::string::npos);
    FeatureConfig::Load(toml::table{});
}

TEST_CASE(feature_config_load_keeps_defaults_for_hand_edited_garbage)
{
    FeatureConfig::Load(toml::parse("[reply]\ncount = \"9\"\nlength = \"long\"\n[hotkeys]\nreply = \"zzz\"\n"));
    REQUIRE_EQ(FeatureConfig::Get("reply", "count"), std::string("3"));
    REQUIRE_EQ(FeatureConfig::Get("reply", "length"), std::string("long"));
    REQUIRE_EQ(FeatureConfig::Get("hotkeys", "reply"), std::string("Ctrl+Alt+R"));
    FeatureConfig::Load(toml::table{});
}

TEST_CASE(reply_profile_keeps_tone_order_switches_and_custom_tones)
{
    QuickProfile profile;
    auto reply = Profile({{"action", "get"}});
    REQUIRE(reply.at("ok") == true);
    const auto *formal = FindById(reply.at("tones"), "formal");
    REQUIRE(formal != nullptr);
    REQUIRE(formal->at("on") == true);
    REQUIRE(FindById(reply.at("tones"), "sharp_tongue")->at("on") == false); // spicy tones are opt-in

    reply = Profile({{"action", "putTones"},
                     {"tones", json::array({{{"id", "humor"}, {"on", true}},
                                            {{"id", "custom-1"},
                                             {"on", true},
                                             {"custom", true},
                                             {"name", "客气催款"},
                                             {"desc", "先感谢合作，再说明逾期天数"}},
                                            {{"id", "formal"}, {"on", false}}})}});
    REQUIRE(reply.at("ok") == true);
    REQUIRE_EQ(reply.at("tones").at(0).at("id").get<std::string>(), std::string("humor"));
    REQUIRE_EQ(reply.at("tones").at(1).at("id").get<std::string>(), std::string("custom-1"));
    REQUIRE(reply.at("tones").at(1).at("custom") == true);
    REQUIRE(FindById(reply.at("tones"), "formal")->at("on") == false);
    // Tones that were never mentioned come back at the end instead of vanishing.
    REQUIRE(FindById(reply.at("tones"), "high_eq") != nullptr);

    const auto stored = ReplyStudio::LoadTones();
    REQUIRE_EQ(stored.at(1).at("name").get<std::string>(), std::string("客气催款"));

    REQUIRE(Profile({{"action", "putTones"}, {"tones", json::array({{{"id", "../etc"}, {"on", true}}})}}).at("ok") ==
            false);
    REQUIRE(Profile({{"action", "putTones"},
                     {"tones", json::array({{{"id", "c2"}, {"custom", true}, {"name", ""}, {"desc", "x"}}})}})
                .at("ok") == false);
    // The failed saves left the stored list alone.
    REQUIRE_EQ(ReplyStudio::LoadTones().at(1).at("id").get<std::string>(), std::string("custom-1"));
}

TEST_CASE(reply_persona_is_normalized_and_turned_into_one_sentence)
{
    QuickProfile profile;
    const auto reply = Profile({{"action", "putPersona"},
                                {"persona",
                                 {{"fillers", json::array({"嗯嗯", "嗯嗯", "哈哈"})},
                                  {"emoji", "bogus"},
                                  {"punct", "less"},
                                  {"address", "nin"},
                                  {"banned", json::array({"亲"})},
                                  {"about", "产品经理，说话直接"}}}});
    REQUIRE(reply.at("ok") == true);
    const auto persona = reply.at("persona");
    REQUIRE_EQ(persona.at("fillers").size(), size_t(2));                     // duplicates collapse
    REQUIRE_EQ(persona.at("emoji").get<std::string>(), std::string("some")); // unknown value falls back
    const auto prompt = ReplyStudio::PersonaPrompt(ReplyStudio::LoadPersona(), "您");
    REQUIRE(prompt.find("嗯嗯") != std::string::npos);
    REQUIRE(prompt.find("亲") != std::string::npos);
    REQUIRE(prompt.find("产品经理") != std::string::npos);
    REQUIRE(prompt.find("句末少用句号") != std::string::npos);
}

TEST_CASE(reply_contacts_are_created_from_chats_deduplicated_and_deletable)
{
    QuickProfile profile;
    const json window = {
        {"platform", "微信"},
        {"title", "林经理"},
        {"kind", "person"},
        {"readable", true},
        {"messages", json::array({{{"id", "1"}, {"speaker", "林经理"}, {"text", "明天几点开会？"}, {"self", false}},
                                  {{"id", "2"}, {"speaker", "我"}, {"text", "上午十点"}, {"self", true}}})}};
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto contact = ReplyStudio::ObserveChat(window, now);
    REQUIRE(contact.is_object());
    REQUIRE_EQ(contact.at("id").get<std::string>(), ReplyStudio::ContactKey("微信", "林经理"));
    REQUIRE_EQ(contact.at("messageCount").get<size_t>(), size_t(2));
    // Seeing the same screen again must not double the history.
    ReplyStudio::ObserveChat(window, now + 1000);
    REQUIRE_EQ(ReplyStudio::RecentMessages(contact.at("id"), 10).size(), size_t(2));
    REQUIRE_EQ(ReplyStudio::RecentMessages(contact.at("id"), 1).size(), size_t(1));
    REQUIRE(
        ReplyStudio::ObserveChat({{"platform", "微信"}, {"title", "x"}, {"readable", false}}, now + 2000).is_null());

    auto saved = Profile({{"action", "putContact"},
                          {"contact",
                           {{"id", contact.at("id")},
                            {"name", "林经理"},
                            {"relation", "直属领导"},
                            {"address", "您"},
                            {"length", "short"},
                            {"tone", "formal"},
                            {"retentionDays", 30}}}});
    REQUIRE(saved.at("ok") == true);
    const auto found = ReplyStudio::FindContact(contact.at("id"));
    REQUIRE_EQ(found.at("relation").get<std::string>(), std::string("直属领导"));
    REQUIRE_EQ(found.at("messageCount").get<size_t>(), size_t(2)); // saving the form keeps the history
    REQUIRE(ReplyStudio::RelationshipPrompt(found).find("直属领导") != std::string::npos);
    REQUIRE(ReplyStudio::RelationshipPrompt(found).find("您") != std::string::npos);

    // Out-of-range choices are dropped, not stored.
    saved = Profile({{"action", "putContact"},
                     {"contact",
                      {{"id", contact.at("id")},
                       {"name", "林经理"},
                       {"address", "老板"},
                       {"retentionDays", 7},
                       {"tone", "../x"}}}});
    REQUIRE(saved.at("ok") == true);
    const auto cleaned = ReplyStudio::FindContact(contact.at("id"));
    REQUIRE_EQ(cleaned.at("address").get<std::string>(), std::string(""));
    REQUIRE_EQ(cleaned.at("retentionDays").get<int>(), 90);
    REQUIRE_EQ(cleaned.at("tone").get<std::string>(), std::string(""));

    REQUIRE(Profile({{"action", "putContact"}, {"contact", {{"id", "x"}, {"name", ""}}}}).at("ok") == false);
    const auto exported = Profile({{"action", "export"}, {"id", contact.at("id")}});
    REQUIRE(json::parse(exported.at("exported").get<std::string>()).at("messages").size() == 2);

    auto cleared = Profile({{"action", "clearRecords"}});
    REQUIRE(cleared.at("ok") == true);
    REQUIRE_EQ(cleared.at("recordCount").get<size_t>(), size_t(0));
    REQUIRE_EQ(cleared.at("contacts").size(), size_t(1)); // notes survive, only chat text goes
    auto deleted = Profile({{"action", "deleteContact"}, {"id", contact.at("id")}});
    REQUIRE_EQ(deleted.at("contacts").size(), size_t(0));
    REQUIRE(Profile({{"action", "bogus"}}).at("ok") == false);
}

TEST_CASE(reply_profile_refuses_to_overwrite_a_file_it_cannot_decrypt)
{
    QuickProfile profile;
    REQUIRE(Profile({{"action", "putPersona"}, {"persona", {{"about", "保留"}}}}).at("ok") == true);
    {
        std::ofstream broken(ReplyStudio::ProfilePath(), std::ios::binary | std::ios::trunc);
        broken << "not an encrypted document";
    }
    REQUIRE(Profile({{"action", "putPersona"}, {"persona", {{"about", "覆盖"}}}}).at("ok") == false);
    std::ifstream file(ReplyStudio::ProfilePath(), std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(file), {}), std::string("not an encrypted document"));
}

TEST_CASE(clipboard_recent_sort_puts_pinned_first_and_sorting_by_copies_is_optional)
{
    QuickProfile recent("[utility]\nclipboard_history = true\n[clipboard]\nsort = \"recent\"\n");
    REQUIRE(ClipboardHistory::AddText(L"很久以前"));
    REQUIRE(ClipboardHistory::AddText(L"很久以前"));
    REQUIRE(ClipboardHistory::AddText(L"中间"));
    REQUIRE(ClipboardHistory::AddText(L"最新"));
    auto entries = ClipboardHistory::LoadEntries();
    REQUIRE_EQ(entries.size(), size_t(3));
    REQUIRE_EQ(entries.front().text, std::wstring(L"最新")); // not the most-copied one
    REQUIRE(ClipboardHistory::SetPinned(L"很久以前", true));
    entries = ClipboardHistory::LoadEntries();
    REQUIRE_EQ(entries.front().text, std::wstring(L"很久以前"));
    REQUIRE(entries.front().pinned);
    REQUIRE_EQ(entries[1].text, std::wstring(L"最新"));
    REQUIRE(entries.front().at > 0);
    REQUIRE(!ClipboardHistory::SetPinned(L"不存在", true));
}

TEST_CASE(clipboard_retention_drops_old_unpinned_records_but_keeps_pinned_ones)
{
    QuickProfile profile("[utility]\nclipboard_history = true\n[clipboard]\nretention_days = \"7\"\n");
    const auto old = 1000LL; // 1970: far older than any retention window
    {
        std::ofstream file(ClipboardHistory::StorePath());
        file << json::array({{{"text", "过期"}, {"copies", 1}, {"at", old}},
                             {{"text", "过期但固定"}, {"copies", 1}, {"at", old}, {"pinned", true}}})
                    .dump();
    }
    REQUIRE(ClipboardHistory::AddText(L"现在"));
    const auto entries = ClipboardHistory::LoadEntries();
    REQUIRE_EQ(entries.size(), size_t(2));
    REQUIRE_EQ(entries.front().text, std::wstring(L"过期但固定"));
    REQUIRE_EQ(entries.back().text, std::wstring(L"现在"));
}

TEST_CASE(ocr_line_merging_joins_wrapped_paragraphs_but_not_lists)
{
    const std::string wrapped = "这是一段被屏幕\n折成两行的文字。\n第二段开始\n继续写下去。";
    const auto smart = Vision::MergeOcrLines(wrapped, "smart");
    REQUIRE(smart.find("屏幕折成") != std::string::npos);       // CJK joins without a space
    REQUIRE(smart.find("文字。\n第二段") != std::string::npos); // a finished sentence keeps its line break
    REQUIRE_EQ(Vision::MergeOcrLines(wrapped, "keep"), wrapped);
    const auto one = Vision::MergeOcrLines("Hello\nworld", "one");
    REQUIRE_EQ(one, std::string("Hello world")); // Latin words get a space back
    const auto list = Vision::MergeOcrLines("Steps:\n1. open\n2. close", "smart");
    REQUIRE(list.find("1. open\n2. close") != std::string::npos);
    REQUIRE_EQ(Vision::MergeOcrLines("", "smart"), std::string(""));
}
