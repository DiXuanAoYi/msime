#include "chat_reader.h"
#include <Windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <regex>
#include <stdexcept>
#pragma comment(lib, "crypt32.lib")
namespace ReplyStudio
{
using nlohmann::json;
namespace
{
std::string Lower(std::string s)
{
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return s;
}
bool Has(const std::string &s, std::initializer_list<const char *> parts)
{
    for (auto p : parts)
        if (s.find(p) != std::string::npos)
            return true;
    return false;
}
bool Descendant(const json &nodes, int child, int parent)
{
    for (size_t count = 0; child >= 0 && static_cast<size_t>(child) < nodes.size() && count < 64; ++count)
    {
        if (child == parent)
            return true;
        child = nodes[child].value("parent", -1);
    }
    return false;
}
bool Same(const json &a, const json &b)
{
    return a.value("id", "") == b.value("id", "") && a.value("text", "") == b.value("text", "") &&
           a.value("speaker", "") == b.value("speaker", "");
}
struct Lock
{
    HANDLE handle = CreateMutexW(nullptr, FALSE, L"Local\\MetasequoiaIme.ChatRecords");
    Lock()
    {
        auto r = handle ? WaitForSingleObject(handle, 3000) : WAIT_FAILED;
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED)
        {
            if (handle)
                CloseHandle(handle);
            throw std::runtime_error("聊天记录正忙。");
        }
    }
    ~Lock()
    {
        ReleaseMutex(handle);
        CloseHandle(handle);
    }
};
std::string Crypt(const std::string &s, bool encrypt)
{
    DATA_BLOB in{static_cast<DWORD>(s.size()), reinterpret_cast<BYTE *>(const_cast<char *>(s.data()))}, out{};
    bool ok = encrypt
                  ? CryptProtectData(&in, L"水杉聊天记录", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                  : CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok)
        throw std::runtime_error("聊天记录无法解密，原文件保留。");
    std::string result(reinterpret_cast<char *>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}
json Read(const std::filesystem::path &path)
{
    if (!std::filesystem::exists(path))
        return {{"version", 1}, {"sessions", json::object()}};
    if (std::filesystem::file_size(path) > 32 * 1024 * 1024)
        throw std::runtime_error("聊天记录已超出 32 MB 上限，请导出并删除旧会话。");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("无法读取聊天记录。");
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    auto doc = json::parse(Crypt(bytes, false));
    if (doc.at("version") != 1 || !doc.at("sessions").is_object())
        throw std::runtime_error("聊天记录版本错误。");
    return doc;
}
void Write(const std::filesystem::path &path, const json &doc)
{
    auto raw = doc.dump();
    if (raw.size() > 30 * 1024 * 1024)
        throw std::runtime_error("聊天记录已满，请先导出并删除旧会话；未覆盖原记录。");
    auto bytes = Crypt(raw, true);
    std::filesystem::create_directories(path.parent_path());
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), bytes.size());
        out.flush();
        if (!out)
            throw std::runtime_error("保存失败，原聊天记录保留。");
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("替换聊天记录失败。");
}
void Key(const std::string &s)
{
    if (s.empty() || s.size() > 64 ||
        s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
        throw std::runtime_error("会话编号只能使用字母、数字、短横线或下划线（最多 64 字符）。");
}
json Sessions(const json &doc)
{
    json out = json::array();
    for (const auto &[id, s] : doc.at("sessions").items())
        out.push_back({{"key", id},
                       {"title", s.at("title")},
                       {"account", s.at("account")},
                       {"platform", s.at("platform")},
                       {"kind", s.at("kind")},
                       {"count", s.at("messages").size()}});
    return out;
}
bool Prune(json &doc, int64_t now)
{
    bool changed = false;
    for (auto &session : doc["sessions"])
    {
        auto &rows = session["messages"];
        auto old = rows.size();
        rows.erase(std::remove_if(
                       rows.begin(), rows.end(),
                       [&](const json &row) { return now - row.at("observedAt").get<int64_t>() > 90LL * 86400000; }),
                   rows.end());
        changed |= old != rows.size();
    }
    return changed;
}
int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace
json ParseChatWindow(const json &w)
{
    json out = {{"window", w.at("window")},
                {"platform", w.at("platform")},
                {"title", ""},
                {"kind", "unknown"},
                {"status", "当前版本未暴露可识别的聊天控件"},
                {"messages", json::array()},
                {"readable", false}};
    if (!w.value("complete", false))
    {
        out["status"] = "窗口最小化、读取超时或会话在读取中切换";
        return out;
    }
    const auto &nodes = w.at("nodes");
    if ((w.at("platform") == "wecom" || w.at("platform") == "qq") && w.contains("ocr"))
    {
        const auto &ocr = w.at("ocr");
        out["status"] = "当前画面本地 OCR：核对标题与发言人；布局无法确认时暂停";
        if (!ocr.value("verifiedLayout", false))
            return out;
        std::vector<json> titleLines;
        for (const auto &line : ocr.at("lines"))
        {
            double x = line.at("x"), y = line.at("y");
            auto text = line.value("text", "");
            if (x >= ocr.value("left", 308.0) + 10 && x < ocr.value("right", 900.0) - 50 && y >= 20 && y < 55 &&
                !text.empty())
                titleLines.push_back(line);
            if ((text.find("群成员") != std::string::npos ||
                 (text.find("群") != std::string::npos && text.find("成员") != std::string::npos) ||
                 text == "群公告") &&
                x > ocr.value("width", 0.0) * 0.64)
                out["kind"] = "group";
        }
        if (titleLines.empty())
            return out;
        std::sort(titleLines.begin(), titleLines.end(),
                  [](const json &a, const json &b) { return a.at("x").get<double>() < b.at("x").get<double>(); });
        std::string titleText;
        double end = titleLines.front().at("x");
        double baseline = titleLines.front().at("y");
        for (const auto &line : titleLines)
        {
            if (std::abs(line.at("y").get<double>() - baseline) > 8 || line.at("x").get<double>() > end + 35)
                return out;
            titleText += line.value("text", "");
            end = line.at("x").get<double>() + line.at("width").get<double>();
        }
        out["title"] = titleText;
        if (w.at("platform") == "qq" && std::regex_search(titleText, std::regex(R"((\(|（)[0-9]+(\)|）))")))
            out["kind"] = "group";
        if (titleText == "我的客服" || titleText == "企业微信团队" || titleText == "文件传输助手")
        {
            out["status"] = "服务 / 文件传输会话不参与自动预回复";
            return out;
        }
        std::map<std::string, int> repeats;
        auto bubbles = ocr.at("bubbles");
        std::sort(bubbles.begin(), bubbles.end(),
                  [](const json &a, const json &b) { return a.at("y").get<double>() < b.at("y").get<double>(); });
        for (const auto &bubble : bubbles)
        {
            double x = bubble.at("x"), y = bubble.at("y"), right = x + bubble.at("width").get<double>(),
                   bottom = y + bubble.at("height").get<double>();
            std::string body, speaker;
            bool self = bubble.value("self", false);
            int names = 0;
            for (const auto &line : ocr.at("lines"))
            {
                double lx = line.at("x"), ly = line.at("y"), rw = lx + line.at("width").get<double>(),
                       bt = ly + line.at("height").get<double>();
                auto text = line.value("text", "");
                if (lx >= x - 2 && rw <= right + 2 && ly >= y - 1 && bt <= bottom + 2)
                {
                    if (!body.empty())
                        body += '\n';
                    body += text;
                }
                else if (!self && lx >= x - 2 && lx < x + 80 && bt <= y && bt >= y - 28 && ly >= 70 &&
                         text != "管理员" && text != "群主")
                {
                    speaker = text;
                    ++names;
                }
            }
            if (body.empty() || body.size() > 12000 || body.find("条新消息") != std::string::npos)
                continue;
            if (self)
                speaker = "本人";
            else if (names != 1 || speaker.size() > 160)
                speaker.clear();
            // Without an explicit sender label do not guess a person from the contact title.
            std::string seed = speaker + "\n" + body;
            uint64_t hash = 14695981039346656037ULL;
            for (unsigned char c : seed)
            {
                hash ^= c;
                hash *= 1099511628211ULL;
            }
            auto id = "ocr-" + std::to_string(hash) + "-" + std::to_string(++repeats[seed]);
            out["messages"].push_back({{"id", id},
                                       {"speaker", speaker},
                                       {"text", body},
                                       {"time", ""},
                                       {"self", self},
                                       {"source", "local-ocr"}});
        }
        out["readable"] = true;
        return out;
    }
    // Feishu 7.x topic view exposes message/reply groups, but no CSS classes.
    // Only accept the observed topic structure, never flatten all document text.
    if (w.at("platform") == "feishu")
    {
        int document = -1;
        bool topics = false;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].value("type", 0) == 50030 && nodes[i].value("name", "") == "messenger-chat")
                document = static_cast<int>(i);
        if (document >= 0)
        {
            std::vector<std::vector<int>> children(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                int p = nodes[i].value("parent", -1);
                if (p >= 0 && static_cast<size_t>(p) < nodes.size())
                    children[p].push_back(static_cast<int>(i));
            }
            auto texts = [&](int parent) {
                std::vector<int> found;
                for (size_t i = parent; i < nodes.size(); ++i)
                    if (Descendant(nodes, static_cast<int>(i), parent) && nodes[i].value("type", 0) == 50020 &&
                        !nodes[i].value("name", "").empty())
                        found.push_back(static_cast<int>(i));
                return found;
            };
            for (size_t i = document + 1; i < nodes.size(); ++i)
                if (Descendant(nodes, static_cast<int>(i), document))
                {
                    auto name = nodes[i].value("name", "");
                    if (out["title"] == "" && nodes[i].value("type", 0) == 50020 && !name.empty())
                        out["title"] = name;
                    if (name == "群公告")
                        out["kind"] = "group";
                    if (name == "新建话题")
                        topics = true;
                }
            if (topics)
            {
                std::map<int, json> messages;
                auto add = [&](int container, const std::string &speaker, const std::vector<int> &body) {
                    std::string text;
                    for (int i : body)
                    {
                        if (!text.empty())
                            text += '\n';
                        text += nodes[i].value("name", "");
                    }
                    if (!text.empty() && text.size() <= 12000 && speaker.size() <= 160)
                        messages[container] = {{"id", nodes[container].value("runtime", "")},
                                               {"speaker", speaker},
                                               {"text", text},
                                               {"time", ""},
                                               {"self", false},
                                               {"source", "uia"}};
                };
                for (size_t i = document + 1; i < nodes.size(); ++i)
                {
                    if (!Descendant(nodes, static_cast<int>(i), document) || children[i].size() != 2)
                        continue;
                    bool nestedMessage = false;
                    for (const auto &[parentId, message] : messages)
                        if (Descendant(nodes, static_cast<int>(i), parentId))
                            nestedMessage = true;
                    if (nestedMessage)
                        continue;
                    const auto &parts = children[i];
                    auto author = texts(parts[0]);
                    if (author.size() != 1 || children[parts[1]].empty())
                        continue;
                    const auto &content = children[parts[1]];
                    auto body = texts(content[0]);
                    bool replyPanel = false;
                    for (int c : content)
                        for (int t : texts(c))
                            if (nodes[t].value("name", "") == "回复话题")
                                replyPanel = true;
                    if (!replyPanel || body.empty())
                        continue;
                    add(static_cast<int>(i), nodes[author[0]].value("name", ""), body);
                    for (size_t j = 1; j < content.size(); ++j)
                        for (int reply : children[content[j]])
                        {
                            const auto &parts2 = children[reply];
                            if (parts2.size() < 2 || nodes[parts2[0]].value("type", 0) != 50020)
                                continue;
                            auto row = texts(reply);
                            if (row.size() < 2)
                                continue;
                            auto speaker = nodes[row[0]].value("name", "");
                            row.erase(row.begin());
                            add(reply, speaker, row);
                        }
                }
                for (const auto &[id, m] : messages)
                    out["messages"].push_back(m);
                out["readable"] = !messages.empty();
                out["status"] = "飞书话题视图：按主帖 / 楼中楼发言人读取；仅当前加载文本";
                return out;
            }
            // Normal message stream: outer and body containers share a stable
            // numeric message ID. The author is a separate sibling of the body.
            for (size_t i = document + 1; i < nodes.size(); ++i)
            {
                auto id = nodes[i].value("id", "");
                if (id.size() < 12 || id.find_first_not_of("0123456789") != std::string::npos ||
                    !Descendant(nodes, int(i), document))
                    continue;
                bool nested = false;
                for (int p = nodes[i].value("parent", -1), depth = 0; p >= 0 && depth < 64;
                     ++depth, p = nodes[p].value("parent", -1))
                    if (nodes[p].value("id", "") == id)
                    {
                        nested = true;
                        break;
                    }
                if (nested || children[i].size() != 2)
                    continue;
                int content = children[i][1];
                if (children[content].size() < 2)
                    continue;
                auto author = texts(children[content][0]);
                if (author.size() != 1)
                    continue;
                std::string speaker = nodes[author[0]].value("name", "");
                if (speaker.empty() || speaker.size() > 160)
                    continue;
                int body = -1;
                for (size_t j = i + 1; j < nodes.size(); ++j)
                    if (Descendant(nodes, int(j), children[content][1]) && nodes[j].value("id", "") == id)
                    {
                        body = int(j);
                        break;
                    }
                if (body < 0 || children[body].empty())
                    continue;
                // First child contains the written body. Link previews/reactions are siblings.
                body = children[body][0];
                std::string text;
                for (size_t j = body; j < nodes.size(); ++j)
                    if (Descendant(nodes, int(j), body) &&
                        (nodes[j].value("type", 0) == 50020 || nodes[j].value("type", 0) == 50005))
                    {
                        auto part = nodes[j].value("name", "");
                        if (part.empty())
                            continue;
                        if (!text.empty())
                            text += '\n';
                        text += part;
                    }
                if (text.empty() || text.size() > 12000)
                    continue;
                out["messages"].push_back({{"id", id},
                                           {"speaker", speaker},
                                           {"text", text},
                                           {"time", ""},
                                           {"self", false},
                                           {"source", "uia"}});
            }
            if (!out["messages"].empty())
            {
                out["readable"] = true;
                out["status"] = "飞书普通消息流：按消息 ID 和发言人读取，仅当前加载文本";
                return out;
            }
            out["status"] = "飞书聊天已识别，此消息布局尚未验证，已暂停记录";
            return out;
        }
    }
    int list = -1, title = -1;
    bool group = false, service = false;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto &n = nodes[i];
        auto id = Lower(n.value("id", "")), cls = Lower(n.value("class", "")), name = n.value("name", "");
        if (Has(id, {"current_chat_name_label", "chat-title", "chat_title", "conversation-title"}) ||
            Has(cls, {"chat-header__name", "conversation-title", "chat-title"}) ||
            (w.at("platform") == "dingtalk" && id == "title_label" && !n.value("offscreen", false)))
            title = static_cast<int>(i);
        if (Has(id, {"current_chat_count_label"}) && !name.empty())
            group = true;
        if (name == "群成员" || name == "群设置" || name == "群聊信息" || n.value("description", "") == "群设置")
            group = true;
        if (w.at("platform") == "qq" && title < 0 && n.value("type", 0) == 50020 && !name.empty() &&
            w.value("title", "").rfind(name, 0) == 0)
            title = static_cast<int>(i);
        if (Has(id, {"brand_profile_button"}))
            service = true;
        if ((n.value("type", 0) == 50008 || n.value("type", 0) == 50026 || n.value("type", 0) == 50030) &&
            (id == "chat_message_list" || Has(id, {"message-list", "message_list", "chat-history"}) ||
             Has(cls, {"message-list", "chat-history", "msg-list"}) ||
             (n.value("type", 0) == 50008 && (name == "消息" || name == "消息列表" || name == "聊天记录"))))
        {
            if (list != -1)
            {
                out["status"] = "发现多个消息列表，请关闭聊天侧栏后重试";
                return out;
            }
            list = static_cast<int>(i);
        }
    }
    if (title >= 0)
        out["title"] = nodes[title].value("name", "");
    if (service)
    {
        out["status"] = "公众号 / 服务通知不参与自动预回复";
        return out;
    }
    if (list < 0 || title < 0 || out["title"] == "")
        return out;
    if (group)
        out["kind"] = "group";
    else if (w.at("platform") == "wechat")
        out["kind"] = "person";
    std::vector<int> rows;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto &n = nodes[i];
        auto cls = Lower(n.value("class", "")), id = Lower(n.value("id", ""));
        if (static_cast<int>(i) == list || !Descendant(nodes, static_cast<int>(i), list))
            continue;
        if (n.value("type", 0) != 50007 && !Has(cls, {"message-item", "msg-item", "chat-message"}) &&
            !Has(id, {"chat_bubble_item_view"}))
            continue;
        bool nested = false;
        for (int r : rows)
            if (Descendant(nodes, static_cast<int>(i), r))
                nested = true;
        if (!nested)
            rows.push_back(static_cast<int>(i));
    }
    std::string timestamp;
    std::map<std::string, int> contentIds;
    std::set<std::string> qqDuplicates;
    for (int r : rows)
    {
        const auto &n = nodes[r];
        auto text = n.value("name", ""), id = n.value("id", ""), cls = Lower(n.value("class", ""));
        if (text.size() > 12000)
            continue;
        std::string speaker;
        std::vector<std::string> bodies;
        if (w.at("platform") == "dingtalk")
        {
            static const std::regex header(R"(^(.+)在([0-9]{1,2}:[0-9]{2})发送了([\s\S]+)$)");
            std::smatch match;
            if (std::regex_match(text, match, header))
            {
                speaker = match[1].str();
                timestamp = match[2].str();
                text = match[3].str();
            }
            else if (nodes[list].value("class", "") == "im_chat::ChatBubbleWidget")
                continue; // Attachments/system rows do not have verified text semantics.
        }
        if (w.at("platform") == "qq")
        {
            static const std::regex header(R"(^(.+) ([0-9]{4}/[0-9]{1,2}/[0-9]{1,2} [0-9]{1,2}:[0-9]{2}:[0-9]{2})$)");
            std::smatch match;
            if (std::regex_match(text, match, header))
            {
                speaker = match[1].str();
                timestamp = match[2].str();
                text = n.value("value", "");
            }
            else
            {
                static const std::regex combined(
                    R"(^(.+?) ([0-9]{4}/[0-9]{1,2}/[0-9]{1,2} [0-9]{1,2}:[0-9]{2}:[0-9]{2}) ([\s\S]+)$)");
                if (!std::regex_match(text, match, combined))
                    continue;
                speaker = match[1].str();
                timestamp = match[2].str();
                text = match[3].str();
            }
        }
        for (size_t i = r + 1; i < nodes.size(); ++i)
        {
            if (!Descendant(nodes, static_cast<int>(i), r))
                continue;
            auto name = nodes[i].value("name", ""),
                 marker = Lower(nodes[i].value("id", "") + " " + nodes[i].value("class", ""));
            if (name.empty())
                continue;
            if (Has(marker, {"sender", "nickname", "avatar", "head_pic"}) && name.size() <= 160)
                speaker = name;
            else if (nodes[i].value("type", 0) == 50020 && (bodies.empty() || bodies.back() != name))
                bodies.push_back(name);
        }
        if (text.empty())
            for (const auto &s : bodies)
            {
                if (!text.empty())
                    text += '\n';
                text += s;
            }
        if (text.empty() || text.size() > 12000 || text.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        // Bare timestamp/system list entries have no bubble marker or named speaker.
        if (speaker.empty() && id.empty() && (cls.empty() || cls == "mmui::chatitemview"))
        {
            timestamp = text;
            continue;
        }
        bool self = Has(cls, {"message--self", "message-outgoing", "msg-item-self"});
        if (w.at("platform") == "wechat" && n.value("directionEvidence", "") == "wechat-green-right-text-bubble")
            self = true;
        // Some clients expose an unnamed avatar. Its position must belong to this
        // message row inside the verified chat list, not a sidebar/profile image.
        if (w.at("platform") == "wechat" || w.at("platform") == "wecom")
        {
            const double left = nodes[list].value("x", 0), width = nodes[list].value("width", 0);
            for (size_t i = r + 1; i < nodes.size(); ++i)
            {
                if (!Descendant(nodes, static_cast<int>(i), r) || nodes[i].value("offscreen", false))
                    continue;
                const auto marker = Lower(nodes[i].value("id", "") + " " + nodes[i].value("class", ""));
                if (width > 100 && Has(marker, {"avatar", "head_pic"}) && nodes[i].value("x", 0) > left + width * 0.8)
                    self = true;
            }
        }
        if (self)
            speaker = "本人";
        std::string source = self ? "uia+outgoing-direction" : "uia";
        if (!self && speaker.empty() && w.at("platform") == "wechat" && group && w.contains("labels"))
        {
            // Group sender sits above the bubble, inside this row. A bare/private bubble
            // is never assigned to the contact solely from its horizontal position.
            double scale = w.value("dpi", 96) / 96.0;
            std::vector<std::string> labels;
            for (const auto &line : w.at("labels"))
            {
                double dx = line.at("x").get<double>() - n.value("x", 0),
                       dy = line.at("y").get<double>() - n.value("y", 0);
                auto label = line.value("text", "");
                if (dx >= 35 * scale && dx < 170 * scale && dy >= 0 && dy < 20 * scale &&
                    line.at("height").get<double>() < 22 * scale && !label.empty() && label.size() <= 160 &&
                    text.find(label) == std::string::npos)
                    labels.push_back(label);
            }
            if (labels.size() == 1)
            {
                speaker = labels[0];
                source = "uia+local-ocr-speaker";
            }
        }
        // The selected session's accessibility summary includes the latest sender in
        // native text. Use it only when the full latest body (not an ellipsis) agrees,
        // and its timestamp agrees with the loaded final row. Never use sidebar text
        // as a message or apply this identity to older bubbles.
        if (!self && w.at("platform") == "wechat" && group && r == rows.back() && text.size() >= 3 &&
            text.find("…") == std::string::npos && text.find("...") == std::string::npos)
        {
            const auto chatTitle = out["title"].get<std::string>();
            std::vector<std::string> summaries;
            for (const auto &session : nodes)
            {
                if (session.value("id", "") != "session_item_" + chatTitle)
                    continue;
                auto summary = session.value("name", "");
                summary = std::regex_replace(summary, std::regex(R"(\s+)"), " ");
                while (!summary.empty() && summary.back() == ' ')
                    summary.pop_back();
                if (summary.rfind(chatTitle + " ", 0) != 0)
                    continue;
                summary.erase(0, chatTitle.size() + 1);
                if (summary.rfind("已置顶 ", 0) == 0)
                    summary.erase(0, std::string("已置顶 ").size());
                if (summary.rfind("[", 0) == 0)
                {
                    auto end = summary.find("条] ");
                    if (end == std::string::npos || end > 12)
                        continue;
                    summary.erase(0, end + std::string("条] ").size());
                }
                auto delimiter = summary.find(": ");
                if (delimiter == std::string::npos || delimiter == 0 || delimiter > 160)
                    continue;
                auto author = summary.substr(0, delimiter);
                auto remainder = summary.substr(delimiter + 2);
                if (remainder.rfind(text + " ", 0) != 0)
                    continue;
                auto suffix = remainder.substr(text.size() + 1);
                if (timestamp.empty() || suffix.rfind(timestamp, 0) != 0)
                    continue;
                suffix.erase(0, timestamp.size());
                if (!suffix.empty() && suffix != " 消息免打扰")
                    continue;
                summaries.push_back(author);
            }
            if (summaries.size() == 1)
            {
                speaker = summaries[0];
                source = "uia+verified-session-sender";
            }
        }
        if (w.at("platform") == "qq" && !qqDuplicates.insert(speaker + "\n" + timestamp + "\n" + text).second)
            continue;
        auto runtime = n.value("runtime", "");
        if ((runtime.empty() || runtime == "[]") && !speaker.empty() &&
            (w.at("platform") == "qq" || w.at("platform") == "dingtalk"))
        {
            std::string seed = speaker + "\n" + timestamp + "\n" + text;
            uint64_t hash = 14695981039346656037ULL;
            for (unsigned char c : seed)
            {
                hash ^= c;
                hash *= 1099511628211ULL;
            }
            runtime = "content-" + std::to_string(hash) + "-" + std::to_string(++contentIds[seed]);
        }
        out["messages"].push_back({{"id", runtime},
                                   {"speaker", speaker},
                                   {"text", text},
                                   {"time", timestamp},
                                   {"self", self},
                                   {"source", source}});
        if (out["messages"].size() >= 200)
            break;
    }
    out["readable"] = true;
    out["status"] = out["messages"].empty() ? "已识别会话，暂无可读文本消息"
                                            : "已识别可读消息；未暴露的发言人标记为未知，不触发预回复";
    return out;
}
size_t ChatAppendOffset(const json &before, const json &after)
{
    if (before.empty() || after.empty())
        return after.size();
    for (size_t count = std::min(before.size(), after.size()); count > 0; --count)
    {
        bool match = true;
        for (size_t j = 0; j < count; ++j)
            if (!Same(before[before.size() - count + j], after[j]))
            {
                match = false;
                break;
            }
        if (match)
            return count;
    }
    // No proven overlap: scrolling, recycled controls, restart, or a gap. Rebaseline, never guess a new message.
    return after.size();
}
json CaptureChatWindows(const std::function<bool()> &cancelled, bool selfTest)
{
    wchar_t own[32768]{};
    GetModuleFileNameW(nullptr, own, 32768);
    auto exe = std::filesystem::path(own).parent_path() / L"MetasequoiaImeChatCapture.exe";
    if (!std::filesystem::exists(exe))
        throw std::runtime_error("缺少聊天读取组件，请重新安装完整安装包。");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    if (!CreatePipe(&reader, &writer, &sa, 0))
        throw std::runtime_error("无法创建聊天读取通道。");
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOEXW start{};
    start.StartupInfo.cb = sizeof(start);
    start.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    start.StartupInfo.wShowWindow = SW_HIDE;
    HANDLE null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, nullptr);
    start.StartupInfo.hStdOutput = writer;
    start.StartupInfo.hStdError = null;
    start.StartupInfo.hStdInput = null;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    start.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE handles[] = {writer, null};
    PROCESS_INFORMATION process{};
    bool attributes = InitializeProcThreadAttributeList(start.lpAttributeList, 1, 0, &bytes) &&
                      UpdateProcThreadAttribute(start.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                                sizeof(handles), nullptr, nullptr);
    auto command = L"\"" + exe.wstring() + L"\"";
    if (selfTest)
        command += L" --self-test-pipe";
    bool created = attributes && CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                                CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                                exe.parent_path().c_str(), &start.StartupInfo, &process);
    if (attributes)
        DeleteProcThreadAttributeList(start.lpAttributeList);
    CloseHandle(writer);
    if (null != INVALID_HANDLE_VALUE)
        CloseHandle(null);
    if (!created)
    {
        CloseHandle(reader);
        throw std::runtime_error("无法启动聊天读取组件。");
    }
    std::string result;
    bool aborted = false;
    auto deadline = GetTickCount64() + 8500;
    for (;;)
    {
        DWORD available = 0;
        PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr);
        if (available)
        {
            char buffer[16384];
            DWORD read = 0;
            if (ReadFile(reader, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr))
                result.append(buffer, read);
        }
        else if (WaitForSingleObject(process.hProcess, 25) == WAIT_OBJECT_0)
        {
            // The child can write its final bytes and exit between Peek and Wait.
            // Drain the pipe AFTER exit before considering the JSON complete.
            DWORD remaining = 0;
            if (!PeekNamedPipe(reader, nullptr, 0, nullptr, &remaining, nullptr) || remaining == 0)
                break;
        }
        if (cancelled() || GetTickCount64() > deadline || result.size() > 4 * 1024 * 1024)
        {
            aborted = true;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
    }
    DWORD exit = 1;
    GetExitCodeProcess(process.hProcess, &exit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(reader);
    if (aborted || exit != 0)
        throw std::runtime_error("聊天读取已取消或超时；记录保留，下次扫描重试。");
    auto raw = json::parse(result);
    json out = json::array();
    for (const auto &w : raw.at("windows"))
        out.push_back(ParseChatWindow(w));
    return out;
}
json ChatReader::Poll(const json &bindings, const json &windows, int64_t now)
{
    if (!bindings.is_array() || bindings.size() > 20)
        throw std::runtime_error("最多同时绑定 20 个聊天窗口。");
    Lock lock;
    auto doc = Read(path_);
    auto &sessions = doc["sessions"];
    json events = json::array();
    auto nextBaselines = baselines_;
    std::set<std::string> active;
    bool changed = Prune(doc, now);
    for (const auto &b : bindings)
    {
        auto key = b.at("key").get<std::string>();
        Key(key);
        if (!active.insert(key).second)
            throw std::runtime_error("会话编号重复。");
        for (auto f : {"account", "title", "window", "platform", "self"})
            if (!b.contains(f) || !b[f].is_string() || b[f].get<std::string>().empty() ||
                b[f].get<std::string>().size() > 200)
                throw std::runtime_error("请填写账户标识和自己的聊天昵称，并绑定可识别的窗口。");
        if (b.at("kind") != "person" && b.at("kind") != "group")
            throw std::runtime_error("请确认联系人或群聊类型。");
        auto found = std::find_if(windows.begin(), windows.end(), [&](const json &w) {
            return w.at("window") == b.at("window") && w.at("platform") == b.at("platform") &&
                   w.at("title") == b.at("title") && w.value("readable", false);
        });
        if (found == windows.end())
        {
            nextBaselines.erase(key);
            continue;
        }
        if (sessions.contains(key))
        {
            for (auto f : {"account", "platform", "title", "kind"})
                if (sessions[key].at(f) != b.at(f))
                    throw std::runtime_error("会话编号已属于其他账户或聊天，请使用独立编号。");
        }
        else
        {
            if (sessions.size() >= 100)
                throw std::runtime_error("最多保存 100 个聊天会话，请导出并删除旧会话。");
            sessions[key] = {{"key", key},
                             {"title", b.at("title")},
                             {"kind", b.at("kind")},
                             {"platform", b.at("platform")},
                             {"account", b.at("account")},
                             {"messages", json::array()}};
            changed = true;
        }
        auto &session = sessions[key];
        auto &stored = session["messages"];
        const auto &messages = found->at("messages");
        bool baseline = stored.empty() || !nextBaselines.count(key) || nextBaselines[key].at("binding") != b;
        size_t offset = baseline ? messages.size() : ChatAppendOffset(nextBaselines[key]["messages"], messages);
        if (!baseline && !messages.empty())
        {
            const auto &previous = nextBaselines[key]["messages"];
            bool common = std::any_of(messages.begin(), messages.end(), [&](const json &m) {
                return std::any_of(previous.begin(), previous.end(), [&](const json &old) { return Same(old, m); });
            });
            bool known = std::all_of(messages.begin(), messages.end(), [&](const json &m) {
                return std::any_of(stored.begin(), stored.end(), [&](const json &old) { return Same(old, m); });
            });
            if (!common && !known)
                throw std::runtime_error("消息列表发生断层，已停止以避免混入同名聊天。请核对账户和联系人后重新绑定。");
        }
        if (baseline)
            offset = 0;
        json incoming = json::array();
        for (size_t i = offset; i < messages.size(); ++i)
        {
            auto row = messages[i];
            if (std::any_of(stored.begin(), stored.end(), [&](const json &old) { return Same(old, row); }))
                continue;
            row["observedAt"] = now;
            row["historical"] = baseline;
            row["self"] = row.value("self", false) || row.value("speaker", "") == b.at("self");
            stored.push_back(row);
            changed = true;
            if (!baseline && !row.value("self", false) && !row.value("speaker", "").empty() &&
                row.value("id", "") != "[]" && !row.value("id", "").empty())
                incoming.push_back(row);
        }
        if (!messages.empty() &&
            (messages.back().value("self", false) || messages.back().value("speaker", "") == b.at("self") ||
             messages.back().value("speaker", "").empty()))
            incoming.clear();
        // Retain by message age, never silently truncate by count.
        auto old = stored.size();
        stored.erase(
            std::remove_if(stored.begin(), stored.end(),
                           [&](const json &m) { return now - m.at("observedAt").get<int64_t>() > 90LL * 86400000; }),
            stored.end());
        changed |= old != stored.size();
        if (stored.size() > 10000)
            throw std::runtime_error("此会话已到 10000 条上限，请先导出并删除旧记录。");
        nextBaselines[key] = {{"binding", b}, {"messages", messages}};
        if (!baseline && offset < messages.size())
            events.push_back(
                {{"key", key}, {"title", b.at("title")}, {"incoming", incoming}, {"revision", stored.size()}});
    }
    for (auto it = nextBaselines.begin(); it != nextBaselines.end();)
        if (!active.count(it->first))
            it = nextBaselines.erase(it);
        else
            ++it;
    if (changed)
        Write(path_, doc);
    baselines_ = std::move(nextBaselines);
    return {{"windows", windows}, {"sessions", Sessions(doc)}, {"events", events}};
}
json ChatReader::Apply(const json &r, const std::function<bool()> &cancelled)
{
    auto action = r.value("action", "");
    if (action == "scan")
        return {{"windows", CaptureChatWindows(cancelled)}};
    if (action == "poll")
    {
        auto windows = CaptureChatWindows(cancelled);
        if (cancelled())
            throw std::runtime_error("已停止读取。");
        return Poll(r.at("bindings"), windows, Now());
    }
    Lock lock;
    auto doc = Read(path_);
    if (Prune(doc, Now()))
        Write(path_, doc);
    if (action == "load" || action == "delete")
    {
        auto key = r.at("key").get<std::string>();
        Key(key);
        if (action == "delete")
        {
            doc["sessions"].erase(key);
            baselines_.erase(key);
            Write(path_, doc);
        }
        else
        {
            if (!doc["sessions"].contains(key))
                throw std::runtime_error("会话不存在。");
            return {{"session", doc["sessions"][key]}};
        }
    }
    else if (action == "reset")
        baselines_.clear();
    else if (action != "overview")
        throw std::runtime_error("未知聊天读取操作。");
    return {{"sessions", Sessions(doc)}};
}
} // namespace ReplyStudio
