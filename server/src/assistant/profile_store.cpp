#include "profile_store.h"
#include "modes.h"
#include "utils/common_utils.h"
#include "utils/protected_file.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace ReplyStudio
{
using nlohmann::json;
namespace
{
constexpr wchar_t kLockName[] = L"Local\\MetasequoiaIme.ReplyProfile";
constexpr wchar_t kDescription[] = L"水杉回复资料";
constexpr size_t kMaxContacts = 200;
constexpr size_t kMaxMessages = 400;

struct BuiltinTone
{
    const char *id, *example;
    bool on;
};
// Panel order. The first five are on by default; the sharper ones are opt-in.
const BuiltinTone kBuiltinTones[] = {{"formal", "收到，我今晚完成初稿，明早 8 点前发您。", true},
                                     {"high_eq", "谢谢信任～今晚有点紧，我先做框架，明早发您过目？", true},
                                     {"gentle_no", "今晚排不开，明天上午我可以接着做。", true},
                                     {"conditional_yes", "可以，麻烦 8 点前把数据发我，我 12 点前出初稿。", true},
                                     {"humor", "今晚和 PPT 有个约会，8 点前交货～", true},
                                     {"love_advisor", "这几天冷落你了，周六整天留给你好不好？", false},
                                     {"comeback", "具体哪一步有问题？我们对一下。", false},
                                     {"sharp_tongue", "这建议真新鲜，上次您也是这么说的。", false},
                                     {"flirt", "今天的晚霞很好看，可惜你不在旁边。", false},
                                     {"follow_up", "想确认下合同进度，周五前能给到吗？", false},
                                     {"repair", "是我没核对清楚，今天下班前改好重发。", false}};

std::filesystem::path Path()
{
    return std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"reply-profile.dat";
}

json EmptyDocument()
{
    return {{"version", 1},
            {"tones", json::array()},
            {"persona",
             {{"fillers", json::array()},
              {"emoji", "some"},
              {"punct", "normal"},
              {"address", "contact"},
              {"banned", json::array()},
              {"about", ""}}},
            {"contacts", json::array()}};
}

json ReadDocument()
{
    auto document = ProtectedFile::Read(Path(), kDescription);
    if (!document)
        return EmptyDocument();
    if (!document->is_object() || document->value("version", 0) != 1)
        throw std::runtime_error("回复资料版本无效。");
    auto base = EmptyDocument();
    for (auto &[key, value] : base.items())
        if (!document->contains(key) || document->at(key).type() != value.type())
            (*document)[key] = value;
    return *document;
}

bool SafeId(const std::string &id)
{
    return !id.empty() && id.size() <= 40 &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
               std::string::npos;
}

std::string Clip(const json &value, size_t limit)
{
    if (!value.is_string())
        return {};
    auto text = value.get<std::string>();
    if (text.find('\0') != std::string::npos)
        return {};
    if (text.size() > limit)
    {
        text.resize(limit);
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80)
            text.pop_back();
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xc0)
            text.pop_back();
    }
    return text;
}

json WordList(const json &value, size_t maxItems, size_t maxBytes)
{
    json out = json::array();
    if (!value.is_array())
        return out;
    std::set<std::string> seen;
    for (const auto &item : value)
    {
        auto word = Clip(item, maxBytes);
        if (!word.empty() && seen.insert(word).second && out.size() < maxItems)
            out.push_back(word);
    }
    return out;
}

const BuiltinTone *FindBuiltin(const std::string &id)
{
    for (const auto &tone : kBuiltinTones)
        if (id == tone.id)
            return &tone;
    return nullptr;
}

json BuiltinToneJson(const BuiltinTone &tone, bool on)
{
    const auto *mode = FindMode(tone.id);
    return {{"id", tone.id},
            {"name", mode ? std::string(mode->name) : std::string(tone.id)},
            {"desc", mode ? std::string(mode->description) : std::string()},
            {"example", tone.example},
            {"on", on},
            {"custom", false}};
}

json MergeTones(const json &stored)
{
    json out = json::array();
    std::set<std::string> seen;
    for (const auto &row : stored)
    {
        const auto id = row.value("id", "");
        if (!SafeId(id) || !seen.insert(id).second)
            continue;
        if (row.value("custom", false))
        {
            out.push_back({{"id", id},
                           {"name", Clip(row.value("name", ""), 60)},
                           {"desc", Clip(row.value("desc", ""), 600)},
                           {"example", Clip(row.value("example", ""), 300)},
                           {"on", row.value("on", true)},
                           {"custom", true}});
        }
        else if (const auto *builtin = FindBuiltin(id))
            out.push_back(BuiltinToneJson(*builtin, row.value("on", builtin->on)));
    }
    for (const auto &builtin : kBuiltinTones)
        if (!seen.count(builtin.id))
            out.push_back(BuiltinToneJson(builtin, builtin.on));
    return out;
}

json NormalizePersona(const json &input)
{
    auto persona = EmptyDocument().at("persona");
    if (!input.is_object())
        return persona;
    persona["fillers"] = WordList(input.value("fillers", json::array()), 10, 36);
    persona["banned"] = WordList(input.value("banned", json::array()), 20, 36);
    const auto pick = [&](const char *key, std::initializer_list<const char *> allowed) {
        const auto value = input.value(key, "");
        for (const char *item : allowed)
            if (value == item)
                persona[key] = value;
    };
    pick("emoji", {"none", "some", "many"});
    pick("punct", {"normal", "less", "none"});
    pick("address", {"contact", "ni", "nin"});
    persona["about"] = Clip(input.value("about", ""), 600);
    return persona;
}

json SummaryRow(const json &contact)
{
    auto row = contact;
    row["messageCount"] = contact.value("messages", json::array()).size();
    row.erase("messages");
    return row;
}

int64_t RetentionMs(const json &contact)
{
    const auto days = contact.value("retentionDays", 90);
    return int64_t(days) * 86400000;
}

void Prune(json &contacts, int64_t now)
{
    for (auto &contact : contacts)
    {
        auto &messages = contact["messages"];
        const auto limit = RetentionMs(contact);
        messages.erase(std::remove_if(messages.begin(), messages.end(),
                                      [&](const json &m) { return now - m.value("at", now) > limit; }),
                       messages.end());
    }
}

json NormalizeContact(const json &input, const json *existing, int64_t now)
{
    json contact = existing ? *existing : json::object();
    const auto name = Clip(input.value("name", ""), 120);
    if (name.empty())
        throw std::runtime_error("请填写联系人名称。");
    if (!existing)
    {
        const auto id = input.value("id", "");
        if (id.empty() || id.size() > 200)
            throw std::runtime_error("联系人标识无效。");
        contact["id"] = id;
        contact["platform"] = Clip(input.value("platform", "手动添加"), 40);
        contact["messages"] = json::array();
    }
    contact["name"] = name;
    contact["relation"] = Clip(input.value("relation", ""), 40);
    const auto tone = input.value("tone", "");
    contact["tone"] = tone.empty() || SafeId(tone) ? tone : "";
    const auto address = input.value("address", "");
    contact["address"] = address == "您" || address == "你" ? address : "";
    const auto length = input.value("length", "");
    contact["length"] = length == "short" || length == "medium" || length == "long" ? length : "";
    contact["notes"] = Clip(input.value("notes", ""), 600);
    contact["summary"] = Clip(input.value("summary", ""), 1500);
    const auto days = input.value("retentionDays", 90);
    contact["retentionDays"] = days == 30 || days == 90 || days == 365 ? days : 90;
    contact["updated"] = now;
    return contact;
}

int64_t Now()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((value.QuadPart - 116444736000000000ULL) / 10000ULL);
}
} // namespace

std::filesystem::path ProfilePath()
{
    return Path();
}

std::string ContactKey(const std::string &platform, const std::string &title)
{
    return platform + "|" + title;
}

json LoadTones()
{
    ProtectedFile::NamedLock lock(kLockName);
    if (!lock)
        throw std::runtime_error("回复资料正忙，请重试。");
    return MergeTones(ReadDocument().at("tones"));
}

json LoadPersona()
{
    ProtectedFile::NamedLock lock(kLockName);
    if (!lock)
        throw std::runtime_error("回复资料正忙，请重试。");
    return NormalizePersona(ReadDocument().at("persona"));
}

std::string PersonaPrompt(const json &persona, const std::string &contactAddress)
{
    std::string text;
    const auto add = [&](const std::string &part) {
        if (!part.empty())
            text += (text.empty() ? "" : "；") + part;
    };
    const auto join = [](const json &words) {
        std::string out;
        for (const auto &word : words)
            out += (out.empty() ? "" : "、") + word.get<std::string>();
        return out;
    };
    if (!persona.value("fillers", json::array()).empty())
        add("偶尔使用的语气词：" + join(persona.at("fillers")));
    const auto emoji = persona.value("emoji", "some");
    add(emoji == "none" ? "不使用表情" : emoji == "many" ? "常用表情符号" : "偶尔用一个表情");
    const auto punct = persona.value("punct", "normal");
    if (punct == "less")
        add("句末少用句号");
    else if (punct == "none")
        add("基本不用标点，用空格断句");
    const auto address = persona.value("address", "contact");
    if (address == "ni")
        add("称呼对方一律用“你”");
    else if (address == "nin")
        add("称呼对方一律用“您”");
    else if (!contactAddress.empty())
        add("称呼对方用“" + contactAddress + "”");
    if (!persona.value("banned", json::array()).empty())
        add("不要出现这些词：" + join(persona.at("banned")));
    if (!persona.value("about", "").empty())
        add("关于用户：" + persona.at("about").get<std::string>());
    return text;
}

json FindContact(const std::string &key)
{
    ProtectedFile::NamedLock lock(kLockName);
    if (!lock)
        return nullptr;
    const auto document = ReadDocument();
    for (const auto &contact : document.at("contacts"))
        if (contact.value("id", "") == key)
            return SummaryRow(contact);
    return nullptr;
}

json ObserveChat(const json &window, int64_t now)
{
    const auto platform = window.value("platform", ""), title = window.value("title", "");
    if (platform.empty() || title.empty() || !window.value("readable", false))
        return nullptr;
    ProtectedFile::NamedLock lock(kLockName);
    if (!lock)
        throw std::runtime_error("回复资料正忙，请重试。");
    auto document = ReadDocument();
    auto &contacts = document["contacts"];
    const auto key = ContactKey(platform, title);
    auto found =
        std::find_if(contacts.begin(), contacts.end(), [&](const json &c) { return c.value("id", "") == key; });
    if (found == contacts.end())
    {
        if (contacts.size() >= kMaxContacts)
            return nullptr;
        contacts.push_back({{"id", key},
                            {"name", title},
                            {"platform", platform},
                            {"relation", window.value("kind", "") == "group" ? "群聊" : ""},
                            {"tone", ""},
                            {"address", ""},
                            {"length", ""},
                            {"notes", ""},
                            {"summary", ""},
                            {"retentionDays", 90},
                            {"updated", now},
                            {"messages", json::array()}});
        found = contacts.end() - 1;
    }
    auto &stored = (*found)["messages"];
    for (const auto &message : window.value("messages", json::array()))
    {
        const auto text = message.value("text", "");
        if (text.empty())
            continue;
        const bool known = std::any_of(stored.begin(), stored.end(), [&](const json &old) {
            return old.value("id", "") == message.value("id", "") && old.value("text", "") == text &&
                   old.value("speaker", "") == message.value("speaker", "");
        });
        if (known)
            continue;
        stored.push_back({{"id", message.value("id", "")},
                          {"speaker", message.value("speaker", "")},
                          {"self", message.value("self", false)},
                          {"text", text},
                          {"at", now}});
    }
    while (stored.size() > kMaxMessages)
        stored.erase(stored.begin());
    (*found)["updated"] = now;
    Prune(contacts, now);
    ProtectedFile::Write(Path(), document, kDescription);
    return SummaryRow(*found);
}

json RecentMessages(const std::string &key, size_t limit)
{
    ProtectedFile::NamedLock lock(kLockName);
    if (!lock)
        return json::array();
    const auto document = ReadDocument();
    for (const auto &contact : document.at("contacts"))
    {
        if (contact.value("id", "") != key)
            continue;
        const auto &messages = contact.at("messages");
        json out = json::array();
        const size_t start = messages.size() > limit ? messages.size() - limit : 0;
        for (size_t i = start; i < messages.size(); ++i)
            out.push_back(messages[i]);
        return out;
    }
    return json::array();
}

std::string RelationshipPrompt(const json &contact)
{
    if (contact.is_null())
        return {};
    std::string text;
    const auto add = [&](const std::string &part) {
        if (!part.empty())
            text += (text.empty() ? "" : "；") + part;
    };
    add("对方：" + contact.value("name", ""));
    if (!contact.value("relation", "").empty())
        add("关系：" + contact.at("relation").get<std::string>());
    if (!contact.value("address", "").empty())
        add("称呼对方为“" + contact.at("address").get<std::string>() + "”");
    if (!contact.value("notes", "").empty())
        add("备注：" + contact.at("notes").get<std::string>());
    if (!contact.value("summary", "").empty())
        add("过往摘要：" + contact.at("summary").get<std::string>());
    return text;
}

json HandleProfileRequest(const json &request)
{
    json response = {
        {"type", "replyProfileResponse"},        {"protocolVersion", 1}, {"requestId", request.value("requestId", "")},
        {"action", request.value("action", "")}, {"ok", true},           {"message", ""}};
    try
    {
        const auto action = request.value("action", "");
        ProtectedFile::NamedLock lock(kLockName);
        if (!lock)
            throw std::runtime_error("回复资料正忙，请重试。");
        auto document = ReadDocument();
        bool changed = false;
        const auto now = Now();
        if (action == "putTones")
        {
            const auto &rows = request.at("tones");
            if (!rows.is_array() || rows.size() > 40)
                throw std::runtime_error("语气列表无效。");
            json stored = json::array();
            for (const auto &row : rows)
            {
                if (!row.is_object() || !SafeId(row.value("id", "")))
                    throw std::runtime_error("语气标识无效。");
                json item = {{"id", row.at("id")}, {"on", row.value("on", true)}};
                if (row.value("custom", false))
                {
                    if (Clip(row.value("name", ""), 60).empty())
                        throw std::runtime_error("请填写语气名称。");
                    item["custom"] = true;
                    item["name"] = Clip(row.value("name", ""), 60);
                    item["desc"] = Clip(row.value("desc", ""), 600);
                    item["example"] = Clip(row.value("example", ""), 300);
                }
                stored.push_back(item);
            }
            document["tones"] = stored;
            changed = true;
        }
        else if (action == "putPersona")
        {
            document["persona"] = NormalizePersona(request.at("persona"));
            changed = true;
        }
        else if (action == "putContact")
        {
            const auto &input = request.at("contact");
            auto &contacts = document["contacts"];
            const auto id = input.value("id", "");
            auto found =
                std::find_if(contacts.begin(), contacts.end(), [&](const json &c) { return c.value("id", "") == id; });
            if (found == contacts.end())
            {
                if (contacts.size() >= kMaxContacts)
                    throw std::runtime_error("最多保存 200 位联系人，请先删除不需要的。");
                contacts.push_back(NormalizeContact(input, nullptr, now));
                response["id"] = contacts.back().at("id");
            }
            else
            {
                *found = NormalizeContact(input, &*found, now);
                response["id"] = id;
            }
            changed = true;
        }
        else if (action == "deleteContact" || action == "clearRecords")
        {
            auto &contacts = document["contacts"];
            if (action == "deleteContact")
            {
                const auto id = request.value("id", "");
                contacts.erase(std::remove_if(contacts.begin(), contacts.end(),
                                              [&](const json &c) { return c.value("id", "") == id; }),
                               contacts.end());
            }
            else
                for (auto &contact : contacts)
                    contact["messages"] = json::array();
            changed = true;
        }
        else if (action == "messages" || action == "export")
        {
            const auto id = request.value("id", "");
            json rows = json::array();
            for (const auto &contact : document.at("contacts"))
                if (contact.value("id", "") == id)
                {
                    if (action == "export")
                        response["exported"] = contact.dump(2);
                    else
                        rows = contact.at("messages");
                }
            if (action == "messages")
                response["messages"] = rows;
        }
        else if (action != "get")
            throw std::runtime_error("未知的回复资料操作。");

        if (changed)
        {
            Prune(document["contacts"], now);
            ProtectedFile::Write(Path(), document, kDescription);
        }
        response["tones"] = MergeTones(document.at("tones"));
        response["persona"] = NormalizePersona(document.at("persona"));
        response["contacts"] = json::array();
        size_t records = 0;
        for (const auto &contact : document.at("contacts"))
        {
            records += contact.at("messages").size();
            response["contacts"].push_back(SummaryRow(contact));
        }
        std::sort(response["contacts"].begin(), response["contacts"].end(),
                  [](const json &a, const json &b) { return a.value("updated", 0LL) > b.value("updated", 0LL); });
        response["recordCount"] = records;
    }
    catch (const std::exception &e)
    {
        response["ok"] = false;
        response["message"] = e.what();
    }
    return response;
}
} // namespace ReplyStudio
