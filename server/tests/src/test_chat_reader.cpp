#include "assistant/chat_reader.h"
#include "tests/includes/test_framework.h"
#include <chrono>
#include <fstream>
using nlohmann::json;
namespace
{
int64_t Epoch()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
json Message(const char *id, const char *speaker, const char *text = "你好")
{
    return {{"id", id}, {"speaker", speaker}, {"text", text}, {"time", ""}, {"self", false}, {"source", "uia"}};
}
json Window(json messages, const char *platform = "wechat", const char *title = "同名群")
{
    return {{"window", std::string(platform) + "-process-1"},
            {"platform", platform},
            {"title", title},
            {"kind", "group"},
            {"readable", true},
            {"status", "ok"},
            {"messages", messages}};
}
json Binding(const char *key = "work-group", const char *platform = "wechat")
{
    return {{"key", key},           {"window", std::string(platform) + "-process-1"},
            {"title", "同名群"},    {"kind", "group"},
            {"platform", platform}, {"account", "工作账户"},
            {"self", "我自己"}};
}
struct Store
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("msime-chat-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dat");
    ReplyStudio::ChatReader reader{path};
    ~Store()
    {
        std::error_code e;
        std::filesystem::remove(path, e);
        auto p = path;
        p += L".tmp";
        std::filesystem::remove(p, e);
    }
    json Load(const char *key = "work-group")
    {
        return reader.Apply({{"action", "load"}, {"key", key}}, [] { return false; }).at("session");
    }
};
void RejectChat(const std::function<void()> &call)
{
    bool rejected = false;
    try
    {
        call();
    }
    catch (...)
    {
        rejected = true;
    }
    REQUIRE(rejected);
}
json Node(int parent, int type, const char *name = "", const char *id = "", const char *cls = "")
{
    return {{"parent", parent},     {"type", type}, {"name", name}, {"id", id},     {"class", cls},
            {"runtime", "[1,2,3]"}, {"x", 0},       {"y", 0},       {"width", 400}, {"height", 77}};
}
json Raw(const char *platform, json nodes)
{
    return {{"window", "process-window"},
            {"platform", platform},
            {"title", "示例群等2个会话"},
            {"complete", true},
            {"nodes", nodes}};
}
} // namespace
TEST_CASE(chat_baseline_does_not_generate_and_file_is_encrypted)
{
    Store s;
    auto rows = json::array({Message("1", "小林")});
    auto bindings = json::array({Binding()});
    auto first = s.reader.Poll(bindings, json::array({Window(rows)}), Epoch() + 1000);
    REQUIRE(first.at("events").empty());
    auto session = s.Load();
    REQUIRE(session.at("messages").size() == 1);
    REQUIRE(session["messages"][0]["historical"] == true);
    std::ifstream in(s.path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    REQUIRE(bytes.find("小林") == std::string::npos);
    auto again = s.reader.Poll(bindings, json::array({Window(rows)}), Epoch() + 2000);
    REQUIRE(again.at("events").empty());
}
TEST_CASE(chat_same_text_new_id_is_a_new_message_but_scrollback_is_not)
{
    Store s;
    auto b = json::array({Binding()});
    auto rows = json::array({Message("1", "小林")});
    s.reader.Poll(b, json::array({Window(rows)}), Epoch() + 1000);
    rows.push_back(Message("2", "小林"));
    auto second = s.reader.Poll(b, json::array({Window(rows)}), Epoch() + 2000);
    REQUIRE(second["events"].size() == 1);
    REQUIRE(second["events"][0]["incoming"].size() == 1);
    s.reader.Poll(b, json::array({Window(json::array({rows[0]}))}), Epoch() + 3000);
    auto back = s.reader.Poll(b, json::array({Window(rows)}), Epoch() + 4000);
    REQUIRE(back["events"][0]["incoming"].empty());
    REQUIRE(s.Load().at("messages").size() == 2);
}
TEST_CASE(chat_own_or_unknown_latest_message_does_not_trigger_reply)
{
    for (const char *speaker : {"我自己", ""})
    {
        Store s;
        auto b = json::array({Binding()});
        auto rows = json::array({Message("1", "小林")});
        s.reader.Poll(b, json::array({Window(rows)}), Epoch() + 1000);
        rows.push_back(Message("2", "小林", "一起吃饭吗"));
        rows.push_back(Message("3", speaker, "好的"));
        auto result = s.reader.Poll(b, json::array({Window(rows)}), Epoch() + 2000);
        REQUIRE(result["events"][0]["incoming"].empty());
        REQUIRE(s.Load().at("messages").size() == 3);
    }
}
TEST_CASE(chat_reopen_and_missing_window_establish_fresh_baseline)
{
    Store s;
    auto b = json::array({Binding()});
    auto rows = json::array({Message("1", "小林")});
    s.reader.Poll(b, json::array({Window(rows)}), Epoch() + 1000);
    s.reader.Poll(b, json::array(), Epoch() + 2000);
    rows.push_back(Message("2", "小林"));
    auto reappear = s.reader.Poll(b, json::array({Window(rows)}), Epoch() + 3000);
    REQUIRE(reappear.at("events").empty());
    ReplyStudio::ChatReader restarted(s.path);
    rows.push_back(Message("3", "小林"));
    auto result = restarted.Poll(b, json::array({Window(rows)}), Epoch() + 4000);
    REQUIRE(result.at("events").empty());
}
TEST_CASE(chat_accounts_platforms_and_keys_do_not_merge)
{
    Store s;
    auto b = json::array({Binding(), Binding("other-group", "qq")});
    auto rows = json::array({Message("1", "小林")});
    auto result = s.reader.Poll(b, json::array({Window(rows), Window(rows, "qq")}), Epoch() + 1000);
    REQUIRE(result["sessions"].size() == 2);
    auto conflict = Binding();
    conflict["account"] = "另一个账户";
    RejectChat([&] { s.reader.Poll(json::array({conflict}), json::array({Window(rows)}), Epoch() + 2000); });
    REQUIRE(s.Load()["account"] == "工作账户");
    auto bad = Binding("../escape");
    RejectChat([&] { s.reader.Poll(json::array({bad}), json::array({Window(rows)}), Epoch() + 2000); });
}
TEST_CASE(chat_torn_snapshot_and_title_switch_are_not_recorded)
{
    auto raw = Raw("wechat", json::array());
    raw["complete"] = false;
    auto parsed = ReplyStudio::ParseChatWindow(raw);
    REQUIRE(parsed["readable"] == false);
    Store s;
    auto other = Window(json::array({Message("1", "小林")}), "wechat", "另一个群");
    auto result = s.reader.Poll(json::array({Binding()}), json::array({other}), Epoch() + 1000);
    REQUIRE(result["sessions"].empty());
}
TEST_CASE(chat_qqlist_preserves_explicit_speaker_timestamp_and_original_body)
{
    auto title = Node(-1, 50020, "示例群");
    auto list = Node(-1, 50008, "消息");
    auto row = Node(1, 50007, "小林 2026/10/10 08:00:01");
    row["value"] = "第一行\n第二行";
    auto group = Node(-1, 50000);
    group["description"] = "群设置";
    auto parsed = ReplyStudio::ParseChatWindow(Raw("qq", json::array({title, list, row, group})));
    REQUIRE(parsed["readable"] == true);
    REQUIRE(parsed["kind"] == "group");
    REQUIRE(parsed["messages"][0]["speaker"] == "小林");
    REQUIRE(parsed["messages"][0]["text"] == "第一行\n第二行");
    REQUIRE(parsed["messages"][0]["time"] == "2026/10/10 08:00:01");
}
TEST_CASE(chat_wechat_nested_automation_ids_are_not_multiple_lists)
{
    auto nodes = json::array({Node(-1, 50020, "示例群", "header.current_chat_name_label"),
                              Node(-1, 50020, "(6)", "header.current_chat_count_label"),
                              Node(-1, 50008, "消息", "chat_message_list"),
                              Node(2, 50007, "内容", "chat_message_list.qt_scrollarea_viewport.chat_bubble_item_view",
                                   "mmui::ChatTextItemView"),
                              Node(2, 50007, "08:30", "", "mmui::ChatItemView")});
    auto parsed = ReplyStudio::ParseChatWindow(Raw("wechat", nodes));
    REQUIRE(parsed["readable"] == true);
    REQUIRE(parsed["messages"].size() == 1);
    REQUIRE(parsed["messages"][0]["speaker"] == "");
    nodes.push_back(Node(3, 50000, "展开"));
    auto buttons = ReplyStudio::ParseChatWindow(Raw("wechat", nodes));
    REQUIRE(buttons["messages"][0]["speaker"] == "");
}
TEST_CASE(chat_uia_profiles_for_enterprise_clients_require_labeled_chat_fields)
{
    for (const char *platform : {"wecom", "dingtalk"})
    {
        auto nodes = json::array({Node(-1, 50020, "项目群", "chat-title"), Node(-1, 50008, "消息列表", "message-list"),
                                  Node(1, 50007, "原文", "", "message-item"), Node(2, 50020, "同事", "sender"),
                                  Node(-1, 50000, "群成员")});
        auto parsed = ReplyStudio::ParseChatWindow(Raw(platform, nodes));
        REQUIRE(parsed["messages"][0]["speaker"] == "同事");
        REQUIRE(parsed["kind"] == "group");
        auto opaque = ReplyStudio::ParseChatWindow(Raw(platform, json::array({Node(-1, 50032, "客户端")})));
        REQUIRE(opaque["readable"] == false);
    }
}
TEST_CASE(chat_corrupt_store_is_not_overwritten)
{
    Store s;
    {
        std::ofstream out(s.path, std::ios::binary);
        out << "corrupt";
    }
    RejectChat([&] {
        s.reader.Poll(json::array({Binding()}), json::array({Window(json::array({Message("1", "小林")}))}),
                      Epoch() + 1000);
    });
    std::ifstream in(s.path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    REQUIRE(bytes == "corrupt");
}

TEST_CASE(chat_dingtalk_real_qt_header_and_message_structure)
{
    auto nodes = json::array({Node(-1, 50020, "项目讨论", "title_label", "QLabel"),
                              Node(-1, 50008, "消息列表", "qt.chatBubble", "im_chat::ChatBubbleWidget"),
                              Node(1, 50007, "小林在09:16发送了第一行\n第二行")});
    nodes[2]["runtime"] = "[]";
    auto parsed = ReplyStudio::ParseChatWindow(Raw("dingtalk", nodes));
    REQUIRE(parsed["readable"] == true);
    REQUIRE(parsed["messages"].size() == 1);
    REQUIRE(parsed["messages"][0]["speaker"] == "小林");
    REQUIRE(parsed["messages"][0]["time"] == "09:16");
    REQUIRE(parsed["messages"][0]["text"] == "第一行\n第二行");
    REQUIRE(parsed["messages"][0]["id"] != "[]");
    nodes[2]["name"] = "小林发送了一个文件";
    REQUIRE(ReplyStudio::ParseChatWindow(Raw("dingtalk", nodes))["messages"].empty());
}
TEST_CASE(chat_qq_combined_accessible_name_has_stable_id_and_no_duplicate_header)
{
    auto nodes = json::array({Node(-1, 50020, "示例群"), Node(-1, 50008, "消息"),
                              Node(1, 50007, "小林 2026/10/10 09:30:12 原文"),
                              Node(1, 50007, "小林 2026/10/10 09:30:12  "), Node(1, 50007, "原文")});
    nodes[2]["runtime"] = "[]";
    auto parsed = ReplyStudio::ParseChatWindow(Raw("qq", nodes));
    REQUIRE(parsed["messages"].size() == 1);
    REQUIRE(parsed["messages"][0]["text"] == "原文");
    REQUIRE(parsed["messages"][0]["id"] != "[]");
}
TEST_CASE(chat_feishu_stream_excludes_sidebar_composer_previews_and_reactions)
{
    auto nodes = json::array({Node(-1, 50030, "messenger-chat"),
                              Node(0, 50020, "项目群"),
                              Node(0, 50020, "群公告"),
                              Node(0, 50026, "", "1234567890123456789"),
                              Node(3, 50026),
                              Node(3, 50026),
                              Node(5, 50026),
                              Node(6, 50020, "小林"),
                              Node(5, 50026),
                              Node(8, 50026, "", "1234567890123456789"),
                              Node(9, 50026),
                              Node(10, 50020, "项目进展"),
                              Node(10, 50005, "https://example.com"),
                              Node(9, 50026),
                              Node(13, 50020, "链接预览不是原话"),
                              Node(0, 50026),
                              Node(15, 50020, "发送给项目群"),
                              Node(0, 50020, "点赞人"),
                              Node(-1, 50030, "messenger"),
                              Node(18, 50020, "其他会话消息")});
    auto parsed = ReplyStudio::ParseChatWindow(Raw("feishu", nodes));
    REQUIRE(parsed["readable"] == true);
    REQUIRE(parsed["messages"].size() == 1);
    REQUIRE(parsed["messages"][0]["speaker"] == "小林");
    REQUIRE(parsed["messages"][0]["text"] == "项目进展\nhttps://example.com");
    REQUIRE(parsed["messages"][0]["id"] == "1234567890123456789");
}
TEST_CASE(chat_feishu_topic_preserves_each_reply_author)
{
    auto nodes = json::array({Node(-1, 50030, "messenger-chat"), Node(0, 50020, "项目群"), Node(0, 50000, "新建话题"),
                              Node(0, 50026), Node(3, 50026), Node(4, 50020, "小林"), Node(3, 50026), Node(6, 50026),
                              Node(7, 50020, "主帖"), Node(6, 50026), Node(9, 50026), Node(10, 50020, "小周"),
                              Node(10, 50020, "楼中楼内容"), Node(9, 50020, "回复话题")});
    auto parsed = ReplyStudio::ParseChatWindow(Raw("feishu", nodes));
    REQUIRE(parsed["messages"].size() == 2);
    REQUIRE(parsed["messages"][0]["speaker"] == "小林");
    REQUIRE(parsed["messages"][0]["text"] == "主帖");
    REQUIRE(parsed["messages"][1]["speaker"] == "小周");
    REQUIRE(parsed["messages"][1]["text"] == "楼中楼内容");
}
TEST_CASE(chat_wecom_ocr_separates_sender_body_self_and_group_sidebar)
{
    auto line = [](const char *t, int x, int y, int width, int height) {
        return json{{"text", t}, {"x", x}, {"y", y}, {"width", width}, {"height", height}};
    };
    auto raw = Raw("wecom", json::array());
    raw["ocr"] = {
        {"verifiedLayout", true},
        {"width", 1000},
        {"height", 700},
        {"lines", json::array({line("项目群", 326, 30, 80, 18), line("群成员：3", 850, 130, 90, 15),
                               line("小林", 330, 100, 40, 14), line("第一行", 340, 135, 55, 15),
                               line("第二行", 340, 155, 55, 15), line("我的回复", 700, 250, 60, 15),
                               line("侧栏姓名", 858, 180, 90, 15), line("输入草稿", 350, 600, 90, 15)})},
        {"bubbles", json::array({json{{"x", 325}, {"y", 125}, {"width", 180}, {"height", 55}, {"self", false}},
                                 json{{"x", 690}, {"y", 240}, {"width", 100}, {"height", 35}, {"self", true}}})}};
    auto parsed = ReplyStudio::ParseChatWindow(raw);
    REQUIRE(parsed["readable"] == true);
    REQUIRE(parsed["kind"] == "group");
    REQUIRE(parsed["messages"].size() == 2);
    REQUIRE(parsed["messages"][0]["speaker"] == "小林");
    REQUIRE(parsed["messages"][0]["text"] == "第一行\n第二行");
    REQUIRE(parsed["messages"][1]["self"] == true);
    REQUIRE(parsed["messages"][1]["source"] == "local-ocr");
    raw["ocr"]["verifiedLayout"] = false;
    REQUIRE(ReplyStudio::ParseChatWindow(raw)["readable"] == false);
}
TEST_CASE(chat_wechat_ocr_uses_physical_coordinates_at_supported_dpi_scales)
{
    for (int dpi : {96, 120, 144, 192})
    {
        double scale = dpi / 96.0;
        auto nodes =
            json::array({Node(-1, 50020, "项目群", "current_chat_name_label"),
                         Node(-1, 50020, "3", "current_chat_count_label"), Node(-1, 50008, "消息", "chat_message_list"),
                         Node(2, 50007, "正文", "chat_bubble_item_view", "mmui::ChatTextItemView")});
        nodes[3]["x"] = int(400 * scale);
        nodes[3]["y"] = int(200 * scale);
        auto raw = Raw("wechat", nodes);
        raw["dpi"] = dpi;
        raw["labels"] =
            json::array({json{{"text", "小林"}, {"x", 470 * scale}, {"y", 207 * scale}, {"height", 12 * scale}}});
        REQUIRE(ReplyStudio::ParseChatWindow(raw)["messages"][0]["speaker"] == "小林");
        raw["labels"].push_back(raw["labels"][0]);
        REQUIRE(ReplyStudio::ParseChatWindow(raw)["messages"][0]["speaker"] == "");
    }
}
TEST_CASE(chat_restart_adds_unseen_history_without_generating_and_delete_is_isolated)
{
    Store s;
    auto b = json::array({Binding()});
    auto rows = json::array({Message("1", "小林")});
    s.reader.Poll(b, json::array({Window(rows)}), Epoch());
    ReplyStudio::ChatReader restarted(s.path);
    rows.push_back(Message("2", "小周"));
    auto result = restarted.Poll(b, json::array({Window(rows)}), Epoch());
    REQUIRE(result["events"].empty());
    REQUIRE(s.Load()["messages"].size() == 2);
    restarted.Apply({{"action", "delete"}, {"key", "work-group"}}, [] { return false; });
    REQUIRE(restarted.Apply({{"action", "overview"}}, [] { return false; })["sessions"].empty());
}
TEST_CASE(chat_retention_prunes_all_sessions_even_when_unbound)
{
    Store s;
    auto b = json::array({Binding()});
    auto rows = json::array({Message("1", "小林")});
    s.reader.Poll(b, json::array({Window(rows)}), Epoch() - 91LL * 86400000);
    s.reader.Poll(json::array(), json::array(), Epoch());
    REQUIRE(s.Load()["messages"].empty());
}

TEST_CASE(chat_child_exit_drains_complete_pipe_before_parsing)
{
    for (int i = 0; i < 3; ++i)
    {
        auto windows = ReplyStudio::CaptureChatWindows([] { return false; }, true);
        REQUIRE(windows.size() == 1);
        REQUIRE(windows[0]["title"] == "测试群");
        REQUIRE(windows[0]["messages"][0]["text"].get<std::string>().size() == 11000);
    }
    RejectChat([] { ReplyStudio::CaptureChatWindows([] { return true; }, true); });
}

TEST_CASE(chat_unrelated_same_title_snapshot_stops_before_mixing_context)
{
    Store s;
    auto b = json::array({Binding()});
    s.reader.Poll(b, json::array({Window(json::array({Message("first", "小林", "旧会话")}))}), Epoch());
    RejectChat([&] {
        s.reader.Poll(b, json::array({Window(json::array({Message("other", "小周", "另一个同名会话")}))}), Epoch());
    });
    REQUIRE(s.Load()["messages"].size() == 1);
}
