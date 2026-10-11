#include "settings/personal_library.h"
#include "clipboard/clipboard_history.h"
#include "english/vocabulary_service.h"
#include "english/voice_studio.h"
#include "engine/core/data_path.h"
#include "engine/english/english_dictionary.h"
#include "utils/common_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>

namespace PersonalLibrary
{
namespace
{
using nlohmann::json;
std::atomic<uint64_t> revision{0};
std::filesystem::path Path()
{
    return metasequoia::data_directory() / "personal-library.json";
}
json Defaults()
{
    return {{"version", 1},
            {"groups", {{{"id", "phrases"}, {"name", "常用语"}}, {{"id", "quotes"}, {"name", "语录"}}}},
            {"clips", json::array()},
            {"rules", json::array()},
            {"favorites", json::array()},
            {"reviews", json::object()}};
}
class Lock
{
    HANDLE h_ = nullptr;
    bool locked_ = false;

  public:
    explicit Lock(DWORD timeout = 5000)
    {
        h_ = CreateMutexW(nullptr, FALSE, L"Local\\MetasequoiaIme.PersonalLibrary");
        if (h_)
        {
            auto result = WaitForSingleObject(h_, timeout);
            locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
        if (!locked_)
        {
            if (h_)
                CloseHandle(h_);
            throw std::runtime_error("资料库正被占用，请重试");
        }
    }
    ~Lock()
    {
        ReleaseMutex(h_);
        CloseHandle(h_);
    }
};
json Read()
{
    const auto path = Path();
    if (!std::filesystem::exists(path))
        return Defaults();
    if (std::filesystem::file_size(path) > 8 * 1024 * 1024)
        throw std::runtime_error("资料库过大，未修改原文件");
    std::ifstream file(path);
    auto b = json::parse(file);
    if (b.at("version") != 1 || !b.at("groups").is_array() || !b.at("clips").is_array() || !b.at("rules").is_array() ||
        !b.at("favorites").is_array() || !b.at("reviews").is_object())
        throw std::runtime_error("资料库格式无效，未修改原文件");
    return b;
}
void Save(const json &b)
{
    const auto content = b.dump();
    if (content.size() > 8 * 1024 * 1024)
        throw std::runtime_error("资料库已达 8 MB 上限");
    const auto path = Path();
    std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream file(temp, std::ios::binary);
        file << content;
        file.close();
        if (!file)
            throw std::runtime_error("保存资料库失败");
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("替换资料库失败，旧文件保留");
    ++revision;
}
std::string Id()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("无法生成编号");
    wchar_t s[40]{};
    StringFromGUID2(guid, s, 40);
    return wstring_to_string(s);
}
std::string Text(const json &r, const char *key, size_t max, bool empty = false)
{
    const auto x = r.value(key, std::string());
    if ((!empty && x.empty()) || x.size() > max || x.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("字段无效：") + key);
    return x;
}
void CopyText(HWND owner, const std::string &text)
{
    const auto s = string_to_wstring(text);
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h)
        throw std::runtime_error("无法分配剪贴板");
    void *p = GlobalLock(h);
    if (!p)
    {
        GlobalFree(h);
        throw std::runtime_error("无法锁定剪贴板");
    }
    memcpy(p, s.c_str(), bytes);
    GlobalUnlock(h);
    if (!OpenClipboard(owner))
    {
        GlobalFree(h);
        throw std::runtime_error("剪贴板正被占用");
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    CloseClipboard();
    if (!ok)
    {
        GlobalFree(h);
        throw std::runtime_error("复制失败");
    }
}
std::string NormalizeApp(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s.empty() || s.size() > 260 || s.find_first_of("\\/:*?\"<>|") != std::string::npos || s.size() < 5 ||
        s.substr(s.size() - 4) != ".exe")
        throw std::runtime_error("请填写程序文件名，例如 WeChat.exe、Code.exe");
    return s;
}
} // namespace

std::string ForegroundApplication()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (!pid)
        return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return {};
    wchar_t name[32768]{};
    DWORD size = 32768;
    const bool ok = QueryFullProcessImageNameW(h, 0, name, &size) != FALSE;
    CloseHandle(h);
    if (!ok)
        return {};
    return wstring_to_string(std::filesystem::path(name).filename().wstring());
}
std::vector<std::string> AppWords(const std::string &application, const std::string &code)
{
    if (application.empty() || code.empty())
        return {};
    try
    {
        const auto app = NormalizeApp(application);
        static std::mutex mutex;
        static json cached;
        static ULONGLONG refreshed = 0;
        static uint64_t seenRevision = 0;
        static std::filesystem::file_time_type writeTime{};
        static std::filesystem::path root;
        std::lock_guard guard(mutex);
        const auto now = GetTickCount64();
        if (cached.is_null() || now - refreshed > 250 || root != Path() || seenRevision != revision)
        {
            std::error_code error;
            const auto currentTime = std::filesystem::last_write_time(Path(), error);
            if (cached.is_null() || root != Path() || currentTime != writeTime || seenRevision != revision)
            {
                Lock lock(0);
                cached = Read();
                writeTime = currentTime;
                seenRevision = revision;
            }
            root = Path();
            refreshed = now;
        }
        std::vector<std::string> out;
        for (const auto &rule : cached.at("rules"))
            if (rule.value("enabled", true) && rule.value("application", "") == app && rule.value("code", "") == code)
            {
                auto text = rule.value("text", "");
                if (!text.empty() && std::find(out.begin(), out.end(), text) == out.end())
                    out.push_back(text);
                if (out.size() == 20)
                    break;
            }
        return out;
    }
    catch (...)
    {
        return {};
    }
}
nlohmann::json Handle(const nlohmann::json &r, HWND owner)
{
    json response = {{"type", "personalLibraryResponse"},
                     {"protocolVersion", 1},
                     {"requestId", r.value("requestId", "")},
                     {"ok", true},
                     {"message", ""}};
    try
    {
        const auto action = r.at("action").get<std::string>();
        Lock lock;
        auto book = Read();
        bool changed = false;
        if (action == "putGroup")
        {
            auto id = r.value("id", "");
            auto name = Text(r, "name", 160);
            auto &groups = book["groups"];
            auto it = std::find_if(groups.begin(), groups.end(), [&](const json &x) { return x.at("id") == id; });
            if (it != groups.end())
                (*it)["name"] = name;
            else
            {
                if (groups.size() >= 100)
                    throw std::runtime_error("分组限 100 个");
                groups.push_back({{"id", Id()}, {"name", name}});
            }
            changed = true;
        }
        else if (action == "removeGroup")
        {
            auto id = Text(r, "id", 100);
            auto &groups = book["groups"];
            if (groups.size() < 2)
                throw std::runtime_error("至少保留一个分组");
            groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const json &x) { return x.at("id") == id; }),
                         groups.end());
            for (auto &x : book["clips"])
                if (x.at("group") == id)
                    x["group"] = groups[0].at("id");
            changed = true;
        }
        else if (action == "putClip" || action == "putRule" || action == "favorite")
        {
            const std::string section = action == "putClip" ? "clips" : action == "putRule" ? "rules" : "favorites";
            auto id = r.value("id", "");
            if (id.empty())
                id = Id();
            if (id.size() > 100)
                throw std::runtime_error("编号过长");
            json entry = {{"id", id}, {"name", Text(r, "name", 240)}};
            if (action == "putClip")
            {
                entry["text"] = Text(r, "text", 16384);
                entry["group"] = Text(r, "group", 100);
                if (std::none_of(book["groups"].begin(), book["groups"].end(),
                                 [&](const json &g) { return g.at("id") == entry["group"]; }))
                    throw std::runtime_error("分组不存在");
            }
            if (action == "putRule")
            {
                entry["application"] = NormalizeApp(Text(r, "application", 260));
                entry["code"] = Text(r, "code", 80);
                const auto code = entry["code"].get<std::string>();
                if (code.find_first_not_of("abcdefghijklmnopqrstuvwxyz'") != std::string::npos)
                    throw std::runtime_error("编码请使用小写字母或拼音分隔符");
                entry["text"] = Text(r, "text", 2000);
                entry["enabled"] = r.value("enabled", true);
            }
            if (action == "favorite")
            {
                const auto artifact = Text(r, "artifactId", 100);
                if (artifact.size() != 38 || artifact.front() != '{' || artifact.back() != '}' ||
                    artifact.find_first_not_of("{}-0123456789ABCDEFabcdef") != std::string::npos)
                    throw std::runtime_error("语音草稿编号无效");
                const auto name = artifact + ".wav";
                if (!std::filesystem::is_regular_file(metasequoia::data_directory() / "voice-outbox" / name))
                    throw std::runtime_error("语音文件不存在");
                entry["file"] = name;
            }
            auto &items = book[section];
            auto it = std::find_if(items.begin(), items.end(), [&](const json &x) { return x.at("id") == id; });
            if (it != items.end())
                *it = entry;
            else
            {
                if (items.size() >= 2000)
                    throw std::runtime_error("单类资料限 2000 条");
                items.push_back(entry);
            }
            changed = true;
        }
        else if (action == "remove")
        {
            const auto section = Text(r, "section", 30);
            if (section != "clips" && section != "rules" && section != "favorites")
                throw std::runtime_error("不能删除此资料类型");
            auto &items = book[section];
            const auto id = Text(r, "id", 100);
            items.erase(std::remove_if(items.begin(), items.end(), [&](const json &x) { return x.at("id") == id; }),
                        items.end());
            changed = true;
        }
        else if (action == "playFavorite" || action == "copyFavorite")
        {
            const auto id = Text(r, "id", 100);
            const auto &items = book["favorites"];
            const auto it = std::find_if(items.begin(), items.end(), [&](const json &x) { return x.at("id") == id; });
            if (it == items.end())
                throw std::runtime_error("收藏不存在");
            const auto artifact = VoiceStudio::LoadSavedDraft(it->at("file").get<std::string>());
            auto result = VoiceStudio::Handle(
                {{"action", action == "playFavorite" ? "play" : "copy"}, {"artifactId", artifact}}, owner);
            if (!result.value("ok", false))
                throw std::runtime_error(result.value("message", "音频操作失败"));
            response["message"] = action == "playFavorite" ? "正在播放收藏语音" : "已复制音频文件，请手动发送";
        }
        else if (action == "copy")
        {
            CopyText(owner, Text(r, "text", 512 * 1024));
            response["message"] = "已复制，请自行粘贴；不会自动发送。";
        }
        else if (action == "study")
        {
            response["study"] = json::array();
            const auto path = metasequoia::data_directory() / "vocabulary.db";
            if (std::filesystem::exists(path))
            {
                auto store = Vocabulary::Store::Open(path);
                EnglishDictionary dictionary((metasequoia::data_directory() / "english.db").u8string());
                const auto now = Vocabulary::NowMs();
                for (const auto &x : store->Read(365, now))
                {
                    const auto key = x.language + "\n" + x.word;
                    if (book["reviews"].contains(key) && book["reviews"][key].value("due", int64_t(0)) > now)
                        continue;
                    const auto gloss = x.language == "en" ? dictionary.query_chinese_gloss(x.word) : std::string();
                    response["study"].push_back(
                        {{"word", x.word},
                         {"language", x.language},
                         {"gloss", gloss.empty() ? "离线词库暂未收录释义，可在翻译工作台查询。" : gloss}});
                    if (response["study"].size() >= 100)
                        break;
                }
            }
        }
        else if (action == "review")
        {
            const auto word = Text(r, "text", 1000), language = Text(r, "language", 30);
            const auto grade = r.value("grade", 0);
            if (grade < 0 || grade > 2)
                throw std::runtime_error("复习等级无效");
            const auto key = language + "\n" + word;
            auto &item = book["reviews"][key];
            const int old = item.is_object() ? item.value("interval", 0) : 0;
            const int days = grade == 0 ? 0 : grade == 1 ? 1 : std::min(180, std::max(2, old * 2));
            item = {{"word", word},
                    {"language", language},
                    {"interval", days},
                    {"due", Vocabulary::NowMs() + (days ? days * 86400000LL : 600000LL)}};
            changed = true;
        }
        else if (action != "status")
            throw std::runtime_error("未知资料操作");
        if (changed)
            Save(book);
        response["book"] = std::move(book);
        response["history"] = json::array();
        for (const auto &s : ClipboardHistory::Load())
            response["history"].push_back(wstring_to_string(s));
        response["application"] = ForegroundApplication();
    }
    catch (const std::exception &e)
    {
        response["ok"] = false;
        response["message"] = e.what();
    }
    return response;
}
} // namespace PersonalLibrary
