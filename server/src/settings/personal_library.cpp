#include "settings/personal_library.h"
#include "config/ime_config.h"
#include "clipboard/clipboard_history.h"
#include "english/vocabulary_service.h"
#include "english/voice_studio.h"
#include "engine/core/data_path.h"
#include "engine/english/english_dictionary.h"
#include "engine/domain_dictionary/domain_dictionary.h"
#include "utils/common_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <map>
#include <memory>
#include <iterator>
#include <set>
#include <stdexcept>
#include <sstream>

namespace PersonalLibrary
{
namespace
{
using nlohmann::json;
std::atomic<uint64_t> revision{0};
thread_local std::string inputApplication;
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
            {"reviews", json::object()},
            {"applications", json::array()},
            {"appLearning", json::array()}};
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
    // Extend v1 in place: retain every existing phrase, voice reference and review.
    if (!b.contains("applications"))
    {
        b["applications"] = json::array();
        std::set<std::string> apps;
        for (const auto &rule : b["rules"])
            if (rule.contains("application") && rule["application"].is_string())
                apps.insert(rule["application"].get<std::string>());
        for (const auto &app : apps)
            b["applications"].push_back(
                {{"application", app}, {"enabled", true}, {"learn", true}, {"frequency", true}});
    }
    if (!b.contains("appLearning"))
        b["appLearning"] = json::array();
    if (!b["applications"].is_array() || !b["appLearning"].is_array())
        throw std::runtime_error("应用词库格式无效，未修改原文件");
    const auto defaultGroups = Defaults().at("groups");
    for (const auto &group : defaultGroups)
        if (std::none_of(b["groups"].begin(), b["groups"].end(),
                         [&](const json &existing) { return existing.at("id") == group.at("id"); }))
            b["groups"].push_back(group);
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
uint32_t CopyText(HWND owner, const std::string &text)
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
    const auto sequence = GetClipboardSequenceNumber();
    if (!ok)
    {
        GlobalFree(h);
        throw std::runtime_error("复制失败");
    }
    return sequence;
}
std::string NormalizeApp(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s.empty() || s.size() > 260 || s.find_first_of("\\/:*?\"<>|") != std::string::npos || s.size() < 5 ||
        s.substr(s.size() - 4) != ".exe")
        throw std::runtime_error("请填写程序文件名，例如 WeChat.exe、Code.exe");
    return s;
}

json ValidateDomains(const json &domains)
{
    if (!domains.is_array() || domains.size() > domain_dictionary::ids().size())
        throw std::runtime_error("领域词库集合无效");
    std::string csv;
    for (const auto &value : domains)
    {
        if (!value.is_string())
            throw std::runtime_error("领域词库编号无效");
        const auto id = value.get<std::string>();
        if (std::find(domain_dictionary::ids().begin(), domain_dictionary::ids().end(), id) ==
            domain_dictionary::ids().end())
            throw std::runtime_error("未知领域词库编号");
        if (!csv.empty())
            csv += ',';
        csv += id;
    }
    json result = json::array();
    std::istringstream input(domain_dictionary::normalize_enabled(csv));
    std::string id;
    while (std::getline(input, id, ','))
        result.push_back(id);
    return result;
}

json DomainBooks()
{
    const auto path = metasequoia::data_directory() / "domain-input-sources" / "catalog.json";
    if (!std::filesystem::exists(path))
        return json::array();
    if (std::filesystem::file_size(path) > 64 * 1024)
        throw std::runtime_error("领域词库清单过大");
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("领域词库清单读取失败");
    auto catalog = json::parse(file);
    if (catalog.at("version") != 1 || catalog.at("kind") != "msime-domain-input" || !catalog.at("books").is_array())
        throw std::runtime_error("领域词库清单无效");
    const auto &books = catalog.at("books");
    if (books.size() > domain_dictionary::ids().size())
        throw std::runtime_error("领域词库清单条目过多");
    std::set<std::string> seen;
    for (const auto &book : books)
    {
        if (!book.is_object())
            throw std::runtime_error("领域词库清单条目无效");
        const auto id = Text(book, "id", 40);
        if (std::find(domain_dictionary::ids().begin(), domain_dictionary::ids().end(), id) ==
                domain_dictionary::ids().end() ||
            !seen.insert(id).second)
            throw std::runtime_error("领域词库清单编号无效或重复");
        Text(book, "name", 240);
        Text(book, "license", 240);
        Text(book, "source", 2048);
        Text(book, "version", 100);
        const auto hash = Text(book, "sha256", 64);
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
            !book.at("exportable").is_boolean() || !book.at("entries").is_number_integer() ||
            !book.at("bytes").is_number_integer() || book.at("entries").get<int64_t>() <= 0 ||
            book.at("entries").get<int64_t>() > 1000000 || book.at("bytes").get<int64_t>() <= 0 ||
            book.at("bytes").get<int64_t>() > 4 * 1024 * 1024)
            throw std::runtime_error("领域词库清单统计或校验信息无效");
    }
    return books;
}
} // namespace

namespace
{
std::shared_ptr<const json> CachedBook()
{
    static std::mutex mutex;
    static std::shared_ptr<const json> cached;
    static ULONGLONG refreshed = 0;
    static uint64_t seenRevision = 0;
    static std::filesystem::file_time_type writeTime{};
    static std::filesystem::path root;
    std::lock_guard guard(mutex);
    const auto now = GetTickCount64();
    if (!cached || now - refreshed > 250 || root != Path() || seenRevision != revision)
    {
        std::error_code error;
        const auto currentTime = std::filesystem::last_write_time(Path(), error);
        if (!cached || root != Path() || currentTime != writeTime || seenRevision != revision)
        {
            Lock lock(0);
            cached = std::make_shared<const json>(Read());
            writeTime = currentTime;
            seenRevision = revision;
        }
        root = Path();
        refreshed = now;
    }
    return cached;
}
ApplicationPolicy PolicyIn(const json &book, const std::string &app)
{
    for (const auto &profile : book.at("applications"))
        if (profile.value("application", "") == app)
            return {profile.value("enabled", true), profile.value("learn", true), profile.value("frequency", true)};
    return {};
}
std::string Code(std::string code)
{
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return code;
}
json ValidLearning(const json &row, const std::string &app)
{
    const auto code = Code(Text(row, "code", 80));
    if (code.find_first_not_of("abcdefghijklmnopqrstuvwxyz' -") != std::string::npos)
        throw std::runtime_error("学习编码无效");
    const auto language = row.value("language", "zh");
    if (language != "zh" && language != "en")
        throw std::runtime_error("学习语言无效");
    const auto rawCount = row.value("frequency", json(1));
    if ((!rawCount.is_number_unsigned() && !rawCount.is_number_integer()) || rawCount.get<int64_t>() < 0)
        throw std::runtime_error("使用次数必须是非负整数");
    auto count = rawCount.get<uint64_t>();
    return {{"id", row.value("id", Id())},
            {"application", app},
            {"code", code},
            {"text", Text(row, "text", 2000)},
            {"language", language},
            {"frequency", std::min<uint64_t>(count, 1000000000)},
            {"insert", row.value("insert", false)},
            {"enabled", row.value("enabled", true)},
            {"deleted", row.value("deleted", false)},
            {"pinned", row.value("pinned", false)},
            {"position", std::clamp(row.value("position", 0), 0, 9)}};
}
} // namespace
std::string ProcessApplication(uint32_t process_id)
{
    if (!process_id)
        return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
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
std::string ForegroundApplication()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return ProcessApplication(pid);
}
void BindInputApplication(std::string application)
{
    inputApplication = std::move(application);
}
const std::string &InputApplication()
{
    return inputApplication;
}
ApplicationPolicy Policy(const std::string &application)
{
    if (application.empty())
        return {};
    try
    {
        return PolicyIn(*CachedBook(), NormalizeApp(application));
    }
    catch (...)
    {
        return {true, false, false};
    }
}

std::string EffectiveDomainDictionaries(const std::string &application, const std::string &global_csv)
{
    const auto global = domain_dictionary::normalize_enabled(global_csv);
    if (application.empty())
        return global;
    try
    {
        const auto book = CachedBook();
        const auto app = NormalizeApp(application);
        for (const auto &profile : book->at("applications"))
            if (profile.value("application", "") == app && profile.value("domain_mode", "inherit") == "replace")
            {
                const auto selected = ValidateDomains(profile.value("domains", json::array()));
                std::string csv;
                for (const auto &id : selected)
                {
                    if (!csv.empty())
                        csv += ',';
                    csv += id.get<std::string>();
                }
                return csv;
            }
        return global;
    }
    catch (...)
    {
        // A corrupt explicit profile must not enable unintended input packs.
        return {};
    }
}
void RecordSelection(const std::string &application, const std::string &code, const std::string &word,
                     const std::string &language, bool insert)
{
    if (application.empty() || code.empty() || word.empty())
        return;
    Lock lock;
    auto book = Read();
    const auto app = NormalizeApp(application);
    const auto policy = PolicyIn(book, app);
    if (!policy.isolated || (!policy.learn && insert) || (!policy.frequency && !insert))
        return;
    auto &items = book["appLearning"];
    const auto normalized = Code(code);
    auto it = std::find_if(items.begin(), items.end(), [&](const json &x) {
        return x.value("application", "") == app && x.value("code", "") == normalized && x.value("text", "") == word &&
               x.value("language", "zh") == language;
    });
    if (it == items.end())
    {
        if (items.size() >= 10000)
            throw std::runtime_error("应用学习记录已达 10000 条，可先导出并整理");
        items.push_back(
            ValidLearning({{"code", normalized}, {"text", word}, {"language", language}, {"insert", insert}}, app));
    }
    else
    {
        if (policy.frequency)
            (*it)["frequency"] = std::min<uint64_t>(1000000000, it->value("frequency", uint64_t(0)) + 1);
        if (insert && policy.learn)
            (*it)["insert"] = true;
    }
    Save(book);
}
std::vector<Phrase> Phrases(size_t limit)
{
    std::vector<Phrase> out;
    try
    {
        const auto book = CachedBook();
        std::map<std::string, std::string> names;
        for (const auto &group : book->at("groups"))
            names[group.value("id", "")] = group.value("name", "");
        for (const auto &clip : book->at("clips"))
        {
            if (out.size() >= limit)
                break;
            const auto text = clip.value("text", "");
            if (!text.empty())
                out.push_back({names[clip.value("group", "")], clip.value("name", ""), text});
        }
    }
    catch (const std::exception &)
    {
        // An unreadable book simply shows no phrases; the clipboard tab keeps working.
    }
    return out;
}
std::vector<std::string> AppWords(const std::string &application, const std::string &code)
{
    if (application.empty() || code.empty())
        return {};
    try
    {
        const auto app = NormalizeApp(application);
        const auto snapshot = CachedBook();
        const auto &book = *snapshot;
        for (const auto &profile : book.at("applications"))
            if (profile.value("application", "") == app && !profile.value("enabled", true))
                return {};
        std::vector<std::string> out;
        for (const auto &rule : book.at("rules"))
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
void ApplyApplicationCandidates(const std::string &application, const std::string &code,
                                std::vector<WordItem> &candidates, bool english, const std::string &canonical_code)
{
    if (application.empty() || code.empty())
        return;
    try
    {
        const auto app = NormalizeApp(application);
        const auto snapshot = CachedBook();
        const auto &book = *snapshot;
        const auto policy = PolicyIn(book, app);
        if (!policy.isolated)
            return;
        const auto normalized = Code(code);
        auto canonical = Code(canonical_code);
        canonical.erase(std::remove(canonical.begin(), canonical.end(), '\''), canonical.end());
        const auto language = english ? "en" : "zh";
        std::map<std::string, uint64_t> counts;
        std::set<std::string> hidden, pinned;
        std::map<std::string, int> fixed;
        for (const auto &item : book.at("appLearning"))
            if (item.value("application", "") == app &&
                (item.value("code", "") == normalized ||
                 (item.value("insert", false) &&
                  (english ? item.value("code", "").find(normalized) == 0
                           : (!canonical.empty() && item.value("code", "") == canonical)))) &&
                item.value("language", "zh") == language && item.value("enabled", true))
            {
                const auto word = item.value("text", "");
                if (item.value("deleted", false))
                {
                    hidden.insert(word);
                    continue;
                }
                if (item.value("pinned", false))
                    pinned.insert(word);
                if (item.value("position", 0) > 0)
                    fixed[word] = item.value("position", 0);
                counts[word] = item.value("frequency", uint64_t(0));
                if (item.value("insert", false) && candidates.size() < 200 &&
                    std::none_of(candidates.begin(), candidates.end(),
                                 [&](const WordItem &x) { return x.word == word; }))
                    candidates.emplace_back(code, word, 1, CandidateSource::AppDictionary);
            }
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const WordItem &x) { return hidden.count(x.word) != 0; }),
                         candidates.end());
        if (policy.frequency || !pinned.empty())
            std::stable_sort(candidates.begin(), candidates.end(), [&](const WordItem &a, const WordItem &b) {
                if (pinned.count(a.word) != pinned.count(b.word))
                    return pinned.count(a.word) != 0;
                if (!policy.frequency)
                    return false;
                const auto ca = counts.find(a.word), cb = counts.find(b.word);
                return (ca == counts.end() ? 0 : ca->second) > (cb == counts.end() ? 0 : cb->second);
            });
        for (const auto &[word, position] : fixed)
        {
            auto it =
                std::find_if(candidates.begin(), candidates.end(), [&](const WordItem &x) { return x.word == word; });
            if (it == candidates.end())
                continue;
            WordItem item = *it;
            candidates.erase(it);
            candidates.insert(candidates.begin() + std::min<size_t>(position - 1, candidates.size()), std::move(item));
        }
    }
    catch (...)
    { /* An unavailable overlay never writes into the global dictionary. */
    }
}
void SetCandidatePreference(const std::string &application, const std::string &code, const std::string &word,
                            bool english, const std::string &action, int position)
{
    if (action != "pin" && action != "delete" && action != "fix" && action != "clear")
        throw std::runtime_error("未知应用候选操作");
    Lock lock;
    auto book = Read();
    const auto app = NormalizeApp(application);
    if (!PolicyIn(book, app).isolated)
        return;
    auto item = ValidLearning({{"code", code}, {"text", word}, {"language", english ? "en" : "zh"}}, app);
    auto &rows = book["appLearning"];
    auto it = std::find_if(rows.begin(), rows.end(), [&](const json &x) {
        return x.at("application") == app && x.at("code") == item.at("code") && x.at("text") == word &&
               x.value("language", "zh") == item.at("language");
    });
    if (it == rows.end())
    {
        if (rows.size() >= 10000)
            throw std::runtime_error("应用记录已达上限");
        rows.push_back(item);
        it = std::prev(rows.end());
    }
    if (action == "delete")
        (*it)["deleted"] = true;
    else if (action == "pin")
    {
        (*it)["deleted"] = false;
        (*it)["pinned"] = true;
    }
    else if (action == "fix")
    {
        if (position < 1 || position > 9)
            throw std::runtime_error("固定位置须为 1–9");
        (*it)["position"] = position;
    }
    else
    {
        (*it)["position"] = 0;
        (*it)["pinned"] = false;
    }
    Save(book);
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
        if (action == "quoteCatalog")
        {
            const auto path = metasequoia::data_directory() / "domain-input-sources" / "poetry.tsv";
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 4 * 1024 * 1024)
                throw std::runtime_error("青简诗词名句词典缺失或过大，请修复安装包；自定义语录仍可使用");
            std::ifstream file(path, std::ios::binary);
            auto entries = json::array();
            std::set<std::string> seen;
            std::string line;
            size_t index = 0;
            while (std::getline(file, line))
            {
                const auto tab = line.find('\t');
                if (line.empty() || line.front() == '#' || tab == std::string::npos || tab == 0)
                    continue;
                auto text = line.substr(0, tab);
                if (!seen.insert(text).second)
                    continue;
                entries.push_back(
                    {{"id", "poetry-" + std::to_string(++index)}, {"name", "青简 · 诗词名句"}, {"text", text}});
            }
            if (file.bad())
                throw std::runtime_error("诗词词典读取失败");
            response["quoteEntries"] = std::move(entries);
            return response;
        }
        if (action == "history" || action == "editHistory" || action == "pinHistory")
        {
            if (action == "pinHistory" &&
                !ClipboardHistory::SetPinned(string_to_wstring(Text(r, "text", 16384)), r.value("enabled", false)))
                throw std::runtime_error("固定失败：记录已变化或磁盘无法保存；请刷新后重试");
            if (action == "editHistory" &&
                !ClipboardHistory::UpdateEntryText(string_to_wstring(Text(r, "previousText", 16384)),
                                                   string_to_wstring(Text(r, "text", 16384))))
                throw std::runtime_error(
                    "编辑失败：记录已变化、内容为空或超过 4000 字，或磁盘无法保存；原记录未被覆盖");
            response["history"] = json::array();
            response["historyEntries"] = json::array();
            for (const auto &entry : ClipboardHistory::LoadEntries())
            {
                const auto text = wstring_to_string(entry.text);
                response["history"].push_back(text);
                response["historyEntries"].push_back({{"text", text},
                                                      {"copies", entry.copies},
                                                      {"pinned", entry.pinned},
                                                      {"at", entry.at},
                                                      {"app", wstring_to_string(entry.app)}});
            }
            return response;
        }
        std::optional<std::wstring> pending_history_remove;
        bool pending_history_clear = false;
        Lock lock;
        json books = json::array();
        try
        {
            books = DomainBooks();
        }
        catch (const std::exception &)
        {
            // Optional pack metadata must not turn an already-saved phrase, voice favorite or review into failure.
            // Domain operations still fail before any personal-library mutation or export.
            if (action == "domainCatalog" || action == "exportDomain" || action == "setApplicationDomains")
                throw std::runtime_error("领域输入词库清单无效，请修复安装包");
            response["domainWarning"] = "领域输入词库清单无法加载，请修复安装包；常用语、语音收藏及复习资料仍可使用。";
        }
        auto book = Read();
        bool changed = false;
        if (action == "setApplicationDomains")
        {
            const auto app = NormalizeApp(Text(r, "application", 260));
            const auto mode = r.value("domain_mode", "inherit");
            if (mode != "inherit" && mode != "replace")
                throw std::runtime_error("软件领域模式无效");
            const auto domains = ValidateDomains(r.value("domains", json::array()));
            auto &profiles = book["applications"];
            auto it = std::find_if(profiles.begin(), profiles.end(),
                                   [&](const json &x) { return x.at("application") == app; });
            if (it == profiles.end())
            {
                if (profiles.size() >= 200)
                    throw std::runtime_error("独立软件词库最多 200 个");
                profiles.push_back({{"application", app}, {"enabled", false}, {"learn", true}, {"frequency", true}});
                it = profiles.end() - 1;
            }
            (*it)["domain_mode"] = mode;
            (*it)["domains"] = domains;
            changed = true;
            response["message"] = "已自动保存此软件的领域输入词库集合";
        }
        else if (action == "domainCatalog")
        {
            // Read-only action; status is refreshed below through the same response contract.
        }
        else if (action == "exportDomain")
        {
            const auto id = Text(r, "id", 40);
            if (std::find(domain_dictionary::ids().begin(), domain_dictionary::ids().end(), id) ==
                domain_dictionary::ids().end())
                throw std::runtime_error("未知领域词库编号");
            const auto item = std::find_if(books.begin(), books.end(), [&](const json &x) { return x.at("id") == id; });
            if (item == books.end())
                throw std::runtime_error("领域输入词库未安装，请修复安装包");
            if (!item->at("exportable").get<bool>())
                throw std::runtime_error("此领域词库不支持导出");
            const auto path = metasequoia::data_directory() / "domain-input-sources" / (id + ".tsv");
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 4 * 1024 * 1024)
                throw std::runtime_error("领域输入词库源文件缺失或过大");
            std::ifstream file(path, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (!file || body.find('\0') != std::string::npos)
                throw std::runtime_error("领域输入词库读取失败");
            response["exported"] = "# 知语领域输入词库：" + item->at("name").get<std::string>() +
                                   "\n# 字段：词条<TAB>空格分隔拼音<TAB>词频\n# 来源：" +
                                   item->at("source").get<std::string>() + "\n# 许可：" +
                                   item->at("license").get<std::string>() + "；请随再分发保留许可与来源\n" + body;
            response["filename"] = "msime-domain-" + id + ".tsv";
            response["message"] = "已导出完整输入词条与拼音，原词库保持不变";
        }
        else if (action == "putApplication")
        {
            const auto app = NormalizeApp(Text(r, "application", 260));
            auto &profiles = book["applications"];
            auto it = std::find_if(profiles.begin(), profiles.end(),
                                   [&](const json &x) { return x.at("application") == app; });
            json profile = it == profiles.end() ? json::object() : *it;
            profile["application"] = app;
            profile["enabled"] = r.value("enabled", true);
            profile["learn"] = r.value("learn", true);
            profile["frequency"] = r.value("frequency", true);
            if (it != profiles.end())
                *it = profile;
            else
            {
                if (profiles.size() >= 200)
                    throw std::runtime_error("独立软件词库最多 200 个");
                profiles.push_back(profile);
            }
            changed = true;
        }
        else if (action == "exportApplication")
        {
            const auto app = NormalizeApp(Text(r, "application", 260));
            json exported = {{"version", 1},
                             {"kind", "msime-application-dictionary"},
                             {"application", app},
                             {"rules", json::array()},
                             {"learning", json::array()}};
            for (const auto &x : book["applications"])
                if (x.at("application") == app)
                    exported["profile"] = x;
            for (const auto &x : book["rules"])
                if (x.at("application") == app)
                    exported["rules"].push_back(x);
            for (const auto &x : book["appLearning"])
                if (x.at("application") == app)
                    exported["learning"].push_back(x);
            response["exported"] = exported.dump(2);
            response["message"] = "已导出此软件的词条和学习记录";
        }
        else if (action == "importApplication")
        {
            const auto source = json::parse(Text(r, "payload", 4 * 1024 * 1024));
            if (source.value("version", 0) != 1 || source.value("kind", "") != "msime-application-dictionary" ||
                !source.at("rules").is_array() || !source.at("learning").is_array())
                throw std::runtime_error("请选择知语应用词库 JSON 文件");
            const auto app = NormalizeApp(Text(r, "application", 260));
            size_t added = 0;
            for (auto row : source.at("rules"))
            {
                const auto code = Text(row, "code", 80);
                if (code.find_first_not_of("abcdefghijklmnopqrstuvwxyz'") != std::string::npos)
                    throw std::runtime_error("导入词条编码无效");
                json item = {{"id", Id()},
                             {"application", app},
                             {"name", Text(row, "name", 240)},
                             {"code", code},
                             {"text", Text(row, "text", 2000)},
                             {"enabled", row.value("enabled", true)}};
                if (std::none_of(book["rules"].begin(), book["rules"].end(), [&](const json &x) {
                        return x.at("application") == app && x.at("code") == code && x.at("text") == item.at("text");
                    }))
                {
                    book["rules"].push_back(item);
                    ++added;
                }
            }
            for (const auto &row : source.at("learning"))
            {
                auto item = ValidLearning(row, app);
                item["id"] = Id();
                auto &items = book["appLearning"];
                const auto it = std::find_if(items.begin(), items.end(), [&](const json &x) {
                    return x.at("application") == app && x.at("code") == item.at("code") &&
                           x.at("text") == item.at("text") && x.value("language", "zh") == item.at("language");
                });
                if (it == items.end())
                {
                    items.push_back(item);
                    ++added;
                }
            }
            if (book["rules"].size() > 2000 || book["appLearning"].size() > 10000)
                throw std::runtime_error("导入后记录超过上限，原资料保持不变");
            // Import is additive. Never change existing switches or existing global dictionaries.
            if (std::none_of(book["applications"].begin(), book["applications"].end(),
                             [&](const json &x) { return x.at("application") == app; }))
            {
                if (book["applications"].size() >= 200)
                    throw std::runtime_error("独立软件词库最多 200 个");
                json profile = {{"application", app}, {"enabled", true}, {"learn", true}, {"frequency", true}};
                if (source.contains("profile") && source.at("profile").is_object())
                {
                    const auto mode = source.at("profile").value("domain_mode", "inherit");
                    if (mode != "inherit" && mode != "replace")
                        throw std::runtime_error("导入软件领域模式无效");
                    profile["domain_mode"] = mode;
                    profile["domains"] = ValidateDomains(source.at("profile").value("domains", json::array()));
                }
                book["applications"].push_back(std::move(profile));
            }
            changed = true;
            response["message"] = "已合并 " + std::to_string(added) + " 条应用记录，原有内容保留";
        }
        else if (action == "removeLearning")
        {
            const auto app = NormalizeApp(Text(r, "application", 260)), id = Text(r, "id", 100);
            auto &items = book["appLearning"];
            items.erase(std::remove_if(items.begin(), items.end(),
                                       [&](const json &x) { return x.at("application") == app && x.at("id") == id; }),
                        items.end());
            changed = true;
        }
        else if (action == "putGroup")
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
                groups.push_back({{"id", Id()},
                                  {"name", name},
                                  {"kind", r.value("section", "clips") == "quotes" ? "quotes" : "clips"}});
            }
            changed = true;
        }
        else if (action == "removeGroup")
        {
            if (r.value("id", "") == "phrases" || r.value("id", "") == "quotes")
                throw std::runtime_error("常用语和语录为固定分类，请删除或转移自定义分组");
            auto id = Text(r, "id", 100);
            auto &groups = book["groups"];
            if (groups.size() < 2)
                throw std::runtime_error("至少保留一个分组");
            const auto removed =
                std::find_if(groups.begin(), groups.end(), [&](const json &x) { return x.at("id") == id; });
            const std::string destination =
                removed != groups.end() && removed->value("kind", "clips") == "quotes" ? "quotes" : "phrases";
            groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const json &x) { return x.at("id") == id; }),
                         groups.end());
            for (auto &x : book["clips"])
                if (x.at("group") == id)
                    x["group"] = destination;
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
                if (std::none_of(book["applications"].begin(), book["applications"].end(),
                                 [&](const json &x) { return x.at("application") == entry["application"]; }))
                {
                    if (book["applications"].size() >= 200)
                        throw std::runtime_error("独立软件词库最多 200 个");
                    book["applications"].push_back({{"application", entry["application"]},
                                                    {"enabled", true},
                                                    {"learn", true},
                                                    {"frequency", true}});
                }
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
        else if (action == "removeHistory")
        {
            pending_history_remove = string_to_wstring(Text(r, "text", 512 * 1024));
            response["message"] = "已删除所选复制记录；常用语和其他资料保留";
        }
        else if (action == "clearHistory")
        {
            pending_history_clear = true;
            response["message"] = "已清空复制记录；常用语、分组和其他资料保留";
        }
        else if (action == "copy")
        {
            const auto text = Text(r, "text", 512 * 1024);
            const auto sequence = CopyText(owner, text);
            // Capture synchronously so the returned count is current. The shared
            // sequence claim prevents the monitor from counting this event again.
            ClipboardHistory::AddText(string_to_wstring(text), sequence);
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
        response["books"] = std::move(books);
        response["enabled_domains"] = json::array();
        std::istringstream domains(*GetConfiguredEnabledDomainDictionaries());
        std::string domain;
        while (std::getline(domains, domain, ','))
            response["enabled_domains"].push_back(domain);
        auto displayBook = book;
        auto &displayProfiles = displayBook["applications"];
        const auto originalProfiles = displayProfiles.size();
        displayProfiles.erase(std::remove_if(displayProfiles.begin(), displayProfiles.end(),
                                             [](const json &profile) {
                                                 try
                                                 {
                                                     if (!profile.is_object())
                                                         return true;
                                                     NormalizeApp(Text(profile, "application", 260));
                                                     for (const auto *key : {"enabled", "learn", "frequency"})
                                                         if (profile.contains(key) && !profile.at(key).is_boolean())
                                                             return true;
                                                     const auto mode = profile.value("domain_mode", "inherit");
                                                     if (mode != "inherit" && mode != "replace")
                                                         return true;
                                                     ValidateDomains(profile.value("domains", json::array()));
                                                     return false;
                                                 }
                                                 catch (const std::exception &)
                                                 {
                                                     return true;
                                                 }
                                             }),
                              displayProfiles.end());
        if (displayProfiles.size() != originalProfiles)
        {
            const auto message = response["message"].get<std::string>();
            response["message"] =
                message + (message.empty() ? "" : "；") + "部分软件词库配置格式无效，已从本次展示略过；原记录仍保留。";
        }
        response["applications"] = displayProfiles;
        for (auto &profile : response["applications"])
        {
            if (!profile.contains("domain_mode"))
                profile["domain_mode"] = "inherit";
            if (!profile.contains("domains"))
                profile["domains"] = json::array();
        }
        response["book"] = std::move(displayBook);
        response["history"] = json::array();
        response["historyEntries"] = json::array();
        for (const auto &entry : ClipboardHistory::LoadEntries())
            if (!pending_history_clear && (!pending_history_remove || entry.text != *pending_history_remove))
            {
                const auto text = wstring_to_string(entry.text);
                response["history"].push_back(text);
                response["historyEntries"].push_back({{"text", text},
                                                      {"copies", entry.copies},
                                                      {"pinned", entry.pinned},
                                                      {"at", entry.at},
                                                      {"app", wstring_to_string(entry.app)}});
            }
        response["application"] = ForegroundApplication();
        // Prepare every response field before persistence. Optional display metadata must not make a completed save
        // appear to fail, and filtered display profiles must never erase their original records from disk.
        if (changed)
            Save(book);
        if (pending_history_remove && !ClipboardHistory::RemoveText(*pending_history_remove))
            throw std::runtime_error("删除复制记录失败，或该记录已经不存在；请刷新后重试");
        if (pending_history_clear && !ClipboardHistory::TryClear())
            throw std::runtime_error("清空复制记录失败；未报告为成功，请刷新检查");
    }
    catch (const std::exception &e)
    {
        response["ok"] = false;
        response["message"] = e.what();
    }
    return response;
}
} // namespace PersonalLibrary
