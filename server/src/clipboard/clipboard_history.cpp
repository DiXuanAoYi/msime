#include "clipboard/clipboard_history.h"
#include "utils/common_utils.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <optional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <chrono>
#include <sstream>

namespace
{
constexpr wchar_t kConfigMutexName[] = L"Local\\MetasequoiaIme.ConfigFile";
constexpr wchar_t kStoreMutexName[] = L"Local\\MetasequoiaIme.ClipboardHistory";
constexpr wchar_t kMonitorClassName[] = L"MetasequoiaIme.ClipboardListener";
constexpr UINT_PTR kCaptureTimerId = 1;
constexpr UINT kCaptureDebounceMs = 80;

class NamedMutex
{
  public:
    explicit NamedMutex(const wchar_t *name)
    {
        handle_ = CreateMutexW(nullptr, FALSE, name);
        if (handle_)
        {
            const DWORD result = WaitForSingleObject(handle_, 5000);
            locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }

    ~NamedMutex()
    {
        if (locked_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }

    explicit operator bool() const
    {
        return locked_;
    }

  private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

std::filesystem::path LocalAppDataDir()
{
    const std::wstring path = CommonUtils::get_ime_data_path_w();
    if (path.empty())
        return {};
    return std::filesystem::path(path);
}

std::filesystem::path ConfigPath()
{
    return LocalAppDataDir() / L"config.toml";
}

std::string WideToUtf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string &text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string TrimCopy(std::string value)
{
    const size_t first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

std::string Unquote(std::string value)
{
    value = TrimCopy(std::move(value));
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
        return value.substr(1, value.size() - 2);
    return value;
}

bool ParseBoolToken(std::string value)
{
    value = Unquote(std::move(value));
    for (char &ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value == "true" || value == "1" || value == "yes" || value == "on";
}

std::optional<std::string> ReadConfigString(const char *section, const char *key)
{
    const auto path = ConfigPath();
    if (path.empty())
        return std::nullopt;
    std::ifstream input(path);
    if (!input)
        return std::nullopt;
    const std::string header = std::string("[") + section + "]";
    bool inSection = false;
    for (std::string line; std::getline(input, line);)
    {
        const std::string trimmed = TrimCopy(line);
        if (!trimmed.empty() && trimmed.front() == '[')
        {
            inSection = trimmed == header;
            continue;
        }
        if (!inSection || trimmed.empty() || trimmed.front() == '#')
            continue;
        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos || TrimCopy(trimmed.substr(0, equals)) != key)
            continue;
        std::string value = TrimCopy(trimmed.substr(equals + 1));
        if (!value.empty() && (value.front() == '"' || value.front() == '\''))
        {
            const size_t close = value.find(value.front(), 1);
            return value.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        }
        const size_t comment = value.find('#');
        if (comment != std::string::npos)
            value.resize(comment);
        return TrimCopy(value);
    }
    return std::nullopt;
}

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring Lower(std::wstring text)
{
    for (auto &ch : text)
        ch = static_cast<wchar_t>(towlower(ch));
    return text;
}

std::wstring ProcessFileName(HWND window)
{
    if (!window)
        return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid)
        return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size))
    {
        name.assign(buffer, size);
        const auto slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            name.erase(0, slash + 1);
    }
    CloseHandle(process);
    return name;
}

// Windows documents these registered formats for password managers and other sensitive sources.
bool ClipboardAsksNotToBeRecorded()
{
    static const UINT excluded = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (excluded && IsClipboardFormatAvailable(excluded))
        return true;
    if (history && IsClipboardFormatAvailable(history))
    {
        if (HANDLE data = GetClipboardData(history))
            if (const auto *value = static_cast<const DWORD *>(GlobalLock(data)))
            {
                const bool refuse = GlobalSize(data) >= sizeof(DWORD) && *value == 0;
                GlobalUnlock(data);
                return refuse;
            }
    }
    return false;
}

bool WorkstationLocked()
{
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!desktop)
        return true;
    CloseDesktop(desktop);
    return false;
}

void ApplyLimits(std::vector<ClipboardHistory::Entry> &items, const ClipboardHistory::Settings &settings,
                 bool dropExpired)
{
    const int64_t now = NowMs();
    if (dropExpired && settings.retentionDays > 0)
    {
        const int64_t span = int64_t(settings.retentionDays) * 86400000;
        items.erase(
            std::remove_if(items.begin(), items.end(),
                           [&](const auto &entry) { return !entry.pinned && entry.at > 0 && now - entry.at > span; }),
            items.end());
    }
    size_t kept = 0;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const auto &entry) { return !entry.pinned && ++kept > settings.maxItems; }),
                items.end());
}

// Store newest-first for the existing retention policy; sort only the read projection.
std::optional<std::vector<ClipboardHistory::Entry>> ReadStoreUnlocked()
{
    const auto path = ClipboardHistory::StorePath();
    if (path.empty())
        return std::nullopt;
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        if (error)
            return std::nullopt;
        return std::vector<ClipboardHistory::Entry>{};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    try
    {
        auto data = nlohmann::json::parse(input);
        if (!data.is_array())
            return std::nullopt;
        std::vector<ClipboardHistory::Entry> entries;
        for (const auto &item : data)
        {
            std::string text;
            uint32_t copies = 1;
            bool pinned = false;
            int64_t at = 0;
            std::wstring app;
            if (item.is_string())
                text = item.get<std::string>();
            else
            {
                if (!item.is_object() || !item.contains("text") || !item.at("text").is_string() ||
                    !item.contains("copies") || !item.at("copies").is_number_integer())
                    return std::nullopt;
                const auto count = item.at("copies").get<int64_t>();
                if (count < 1 || count > ClipboardHistory::kMaxCopies)
                    return std::nullopt;
                copies = static_cast<uint32_t>(count);
                text = item.at("text").get<std::string>();
                if (item.contains("pinned") && item.at("pinned").is_boolean())
                    pinned = item.at("pinned").get<bool>();
                if (item.contains("at") && item.at("at").is_number_integer())
                    at = item.at("at").get<int64_t>();
                if (item.contains("app") && item.at("app").is_string())
                    app = Utf8ToWide(item.at("app").get<std::string>());
            }
            auto wide = Utf8ToWide(text);
            if (wide.empty() || text.find('\0') != std::string::npos)
                return std::nullopt;
            const auto duplicate =
                std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.text == wide; });
            if (duplicate == entries.end())
                entries.push_back({std::move(wide), copies, pinned, at, std::move(app)});
            else
            {
                duplicate->copies = static_cast<uint32_t>(
                    std::min<uint64_t>(ClipboardHistory::kMaxCopies, uint64_t(duplicate->copies) + copies));
                duplicate->pinned = duplicate->pinned || pinned;
            }
        }
        if (entries.size() > ClipboardHistory::kMaxItems)
            entries.resize(ClipboardHistory::kMaxItems);
        return entries;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

class CapturedSequence
{
  public:
    explicit CapturedSequence(const std::filesystem::path &path)
    {
        auto identity = std::filesystem::absolute(path).lexically_normal().wstring();
        uint64_t hash = 14695981039346656037ull;
        for (auto ch : identity)
        {
            hash ^= static_cast<uint16_t>(towlower(ch));
            hash *= 1099511628211ull;
        }
        const auto name = L"Local\\MetasequoiaIme.ClipboardCapturedSequence.v1." + std::to_wstring(hash);
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(uint32_t), name.c_str());
        if (mapping_)
            value_ = static_cast<uint32_t *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(uint32_t)));
    }
    ~CapturedSequence()
    {
        if (value_)
            UnmapViewOfFile(value_);
        if (mapping_)
            CloseHandle(mapping_);
    }
    uint32_t *value() const
    {
        return value_;
    }

  private:
    HANDLE mapping_ = nullptr;
    uint32_t *value_ = nullptr;
};

void TraceCopySequence(uint32_t requested, uint32_t previous, uint32_t copies)
{
    wchar_t enabled[2];
    if (!GetEnvironmentVariableW(L"MSIME_CLIPBOARD_COUNT_TRACE", enabled, 2))
        return;
    std::ofstream output(LocalAppDataDir() / L"clipboard-count-trace.log", std::ios::app);
    output << GetTickCount64() << " pid=" << GetCurrentProcessId() << " requested=" << requested
           << " actual=" << GetClipboardSequenceNumber() << " previous=" << previous << " copies=" << copies << '\n';
}

bool WriteStoreUnlocked(const std::vector<ClipboardHistory::Entry> &items)
{
    const auto path = ClipboardHistory::StorePath();
    if (path.empty())
        return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    auto data = nlohmann::json::array();
    for (const auto &entry : items)
    {
        nlohmann::json row = {{"text", WideToUtf8(entry.text)}, {"copies", entry.copies}};
        if (entry.pinned)
            row["pinned"] = true;
        if (entry.at)
            row["at"] = entry.at;
        if (!entry.app.empty())
            row["app"] = WideToUtf8(entry.app);
        data.push_back(std::move(row));
    }
    const std::string payload = data.dump(2) + "\n";
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.close();
    const bool written = !output.fail() && MoveFileExW(temporary.c_str(), path.c_str(),
                                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!written)
        std::filesystem::remove(temporary, error);
    return written;
}

std::wstring NormalizeClipboardText(std::wstring text)
{
    while (!text.empty() && (text.back() == L'\0' || text.back() == L'\r'))
        text.pop_back();
    if (text.size() > ClipboardHistory::kMaxChars)
    {
        size_t cut = ClipboardHistory::kMaxChars;
        // wchar_t is UTF-16 here, so the cut can land between a surrogate pair and leave a lone high
        // surrogate, which is not valid UTF-16 and does not survive being written out and read back.
        // Drop the incomplete character rather than half of it.
        if (cut > 0 && text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF)
            --cut;
        text.resize(cut);
    }
    return text;
}

bool WriteEnabledFlag(bool enabled)
{
    NamedMutex lock(kConfigMutexName);
    if (!lock)
        return false;

    const auto path = ConfigPath();
    if (path.empty())
        return false;

    std::ifstream input(path, std::ios::binary);
    std::string text;
    if (input)
        text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    input.close();

    const std::string replacement = enabled ? "true" : "false";
    bool in_utility = false;
    size_t line_begin = 0;
    bool replaced = false;
    size_t utility_header_end = std::string::npos;

    auto is_section = [](const std::string &line, const char *name) {
        const std::string trimmed = TrimCopy(line);
        return trimmed == name;
    };

    while (line_begin < text.size())
    {
        size_t line_end = text.find('\n', line_begin);
        if (line_end == std::string::npos)
            line_end = text.size();
        const std::string line = text.substr(line_begin, line_end - line_begin);
        const std::string trimmed = TrimCopy(line);
        if (!trimmed.empty() && trimmed.front() == '[')
        {
            in_utility = is_section(line, "[utility]");
            if (in_utility)
                utility_header_end = line_end == text.size() ? text.size() : line_end + 1;
        }
        else if (in_utility)
        {
            const size_t equals = trimmed.find('=');
            if (equals != std::string::npos && TrimCopy(trimmed.substr(0, equals)) == "clipboard_history")
            {
                const size_t eq_pos = text.find('=', line_begin);
                if (eq_pos != std::string::npos && eq_pos < line_end)
                {
                    size_t value_begin = eq_pos + 1;
                    while (value_begin < line_end && (text[value_begin] == ' ' || text[value_begin] == '\t'))
                        ++value_begin;
                    size_t value_end = line_end;
                    while (value_end > value_begin &&
                           (text[value_end - 1] == ' ' || text[value_end - 1] == '\t' || text[value_end - 1] == '\r'))
                    {
                        --value_end;
                    }
                    const size_t comment = text.find('#', value_begin);
                    if (comment != std::string::npos && comment < value_end)
                        value_end = comment;
                    while (value_end > value_begin && (text[value_end - 1] == ' ' || text[value_end - 1] == '\t'))
                        --value_end;
                    text.replace(value_begin, value_end - value_begin, replacement);
                    replaced = true;
                    break;
                }
            }
        }
        line_begin = line_end == text.size() ? text.size() : line_end + 1;
    }

    if (!replaced)
    {
        const std::string line = "clipboard_history = " + replacement + "\n";
        if (utility_header_end != std::string::npos)
            text.insert(utility_header_end, line);
        else
            text += "\n[utility]\n" + line;
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

struct MonitorState
{
    std::mutex mutex;
    HANDLE thread = nullptr;
    HWND hwnd = nullptr;
    std::atomic<bool> running{false};
    DWORD lastSequence = 0;
    std::atomic<ULONGLONG> suppressUntil{0};
};

MonitorState g_monitor;

void CaptureClipboardText(HWND hwnd)
{
    if (!ClipboardHistory::IsEnabled())
        return;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return;

    DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == g_monitor.lastSequence)
        return;

    if (GetTickCount64() < g_monitor.suppressUntil.load())
    {
        g_monitor.lastSequence = sequence;
        return;
    }
    const auto settings = ClipboardHistory::LoadSettings();
    if (settings.pauseOnLock && WorkstationLocked())
    {
        g_monitor.lastSequence = sequence;
        return;
    }
    // The owner is only reliable while the clipboard still holds this copy, so read it before opening.
    const std::wstring app = ProcessFileName(GetClipboardOwner());
    if (!app.empty() && std::find(settings.excludedApps.begin(), settings.excludedApps.end(), Lower(app)) !=
                            settings.excludedApps.end())
    {
        g_monitor.lastSequence = sequence;
        return;
    }

    if (!OpenClipboard(hwnd))
    {
        SetTimer(hwnd, kCaptureTimerId, kCaptureDebounceMs, nullptr);
        return;
    }
    sequence = GetClipboardSequenceNumber();

    std::wstring text;
    const bool refused = settings.respectPrivacyFlag && ClipboardAsksNotToBeRecorded();
    if (refused)
    {
        CloseClipboard();
        g_monitor.lastSequence = sequence;
        return;
    }
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
    {
        if (const wchar_t *locked = static_cast<const wchar_t *>(GlobalLock(data)))
        {
            text = locked;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    g_monitor.lastSequence = sequence;

    text = NormalizeClipboardText(std::move(text));
    if (!text.empty())
        ClipboardHistory::AddText(std::move(text), sequence, app);
}

LRESULT CALLBACK MonitorWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CLIPBOARDUPDATE:
        KillTimer(hwnd, kCaptureTimerId);
        CaptureClipboardText(hwnd);
        return 0;
    case WM_TIMER:
        if (wParam == kCaptureTimerId)
        {
            KillTimer(hwnd, kCaptureTimerId);
            CaptureClipboardText(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureMonitorClass()
{
    static bool registered = false;
    if (registered)
        return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MonitorWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMonitorClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    registered = true;
    return true;
}

DWORD WINAPI MonitorThread(LPVOID)
{
    if (!EnsureMonitorClass())
    {
        g_monitor.running = false;
        return 0;
    }

    HWND hwnd = CreateWindowExW(0, kMonitorClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd)
    {
        g_monitor.running = false;
        return 0;
    }

    g_monitor.hwnd = hwnd;
    AddClipboardFormatListener(hwnd);
    g_monitor.lastSequence = GetClipboardSequenceNumber();

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_monitor.hwnd = nullptr;
    g_monitor.running = false;
    return 0;
}
} // namespace

std::filesystem::path ClipboardHistory::StorePath()
{
    const auto dir = LocalAppDataDir();
    if (dir.empty())
        return {};
    return dir / L"clipboard_history.json";
}

std::vector<ClipboardHistory::Entry> ClipboardHistory::LoadEntries()
{
    NamedMutex lock(kStoreMutexName);
    if (!lock)
        return {};
    auto entries = ReadStoreUnlocked();
    if (!entries)
        return {};
    const auto settings = LoadSettings();
    ApplyLimits(*entries, settings, true);
    // Storage order is newest first. Pinned entries lead; "frequent" re-sorts only the unpinned rest.
    const auto firstUnpinned =
        std::stable_partition(entries->begin(), entries->end(), [](const auto &e) { return e.pinned; });
    if (!settings.sortRecent)
        std::stable_sort(firstUnpinned, entries->end(),
                         [](const auto &a, const auto &b) { return a.copies > b.copies; });
    return std::move(*entries);
}

std::vector<std::wstring> ClipboardHistory::Load()
{
    std::vector<std::wstring> texts;
    for (auto &entry : LoadEntries())
        texts.push_back(std::move(entry.text));
    return texts;
}

bool ClipboardHistory::AddText(std::wstring text, uint32_t clipboardSequence, const std::wstring &app)
{
    text = NormalizeClipboardText(std::move(text));
    if (text.empty() || !IsEnabled())
        return false;
    NamedMutex lock(kStoreMutexName);
    if (!lock)
        return false;
    // The mapping lives only while a participating process is alive, so a previous
    // Windows logon or reboot cannot suppress a new event with a reused sequence.
    static std::filesystem::path capturePath;
    static std::unique_ptr<CapturedSequence> captured;
    const auto path = StorePath();
    if (!captured || capturePath != path)
    {
        captured = std::make_unique<CapturedSequence>(path);
        capturePath = path;
    }
    if (clipboardSequence)
    {
        if (!captured->value())
            return false;
        const auto previous = *captured->value();
        TraceCopySequence(clipboardSequence, previous, 0);
        // Reject duplicate and delayed events; unsigned serial ordering also handles wraparound.
        if (previous && (clipboardSequence - previous == 0 || clipboardSequence - previous > 0x7fffffffu))
            return false;
    }
    auto items = ReadStoreUnlocked();
    if (!items)
        return false; // Never replace malformed or unreadable existing history.
    const auto found =
        std::find_if(items->begin(), items->end(), [&](const auto &entry) { return entry.text == text; });
    uint32_t copies = 1;
    bool pinned = false;
    std::wstring source = app;
    if (found != items->end())
    {
        copies = found->copies < kMaxCopies ? found->copies + 1 : kMaxCopies;
        pinned = found->pinned;
        if (source.empty())
            source = found->app;
        items->erase(found);
    }
    items->insert(items->begin(), {std::move(text), copies, pinned, NowMs(), std::move(source)});
    ApplyLimits(*items, LoadSettings(), true);
    if (!WriteStoreUnlocked(*items))
        return false;
    TraceCopySequence(clipboardSequence, captured->value() ? *captured->value() : 0, copies);
    if (clipboardSequence)
        *captured->value() = clipboardSequence;
    return true;
}

bool ClipboardHistory::UpdateEntryText(const std::wstring &previous, std::wstring text)
{
    if (previous.empty() || text.empty() || text.size() > kMaxChars || text.find(L'\0') != std::wstring::npos)
        return false;
    text = NormalizeClipboardText(std::move(text));
    if (text.empty() || WideToUtf8(text).empty())
        return false;
    NamedMutex lock(kStoreMutexName);
    if (!lock)
        return false;
    auto items = ReadStoreUnlocked();
    if (!items)
        return false;
    const auto old =
        std::find_if(items->begin(), items->end(), [&](const auto &entry) { return entry.text == previous; });
    if (old == items->end())
        return false;
    if (previous == text)
        return true;
    const size_t oldIndex = static_cast<size_t>(old - items->begin());
    const auto other =
        std::find_if(items->begin(), items->end(), [&](const auto &entry) { return entry.text == text; });
    Entry replacement = *old;
    replacement.text = std::move(text);
    if (other != items->end())
    {
        replacement.copies =
            static_cast<uint32_t>(std::min<uint64_t>(kMaxCopies, uint64_t(replacement.copies) + other->copies));
        replacement.pinned = replacement.pinned || other->pinned;
        const size_t otherIndex = static_cast<size_t>(other - items->begin());
        (*items)[std::min(oldIndex, otherIndex)] = std::move(replacement);
        items->erase(items->begin() + std::max(oldIndex, otherIndex));
    }
    else
        (*items)[oldIndex] = std::move(replacement);
    return WriteStoreUnlocked(*items);
}

bool ClipboardHistory::RemoveText(const std::wstring &text)
{
    if (text.empty())
        return false;
    NamedMutex lock(kStoreMutexName);
    if (!lock)
        return false;
    auto items = ReadStoreUnlocked();
    if (!items)
        return false;
    const auto before = items->size();
    items->erase(std::remove_if(items->begin(), items->end(), [&](const auto &entry) { return entry.text == text; }),
                 items->end());
    if (items->size() == before)
        return false;
    return WriteStoreUnlocked(*items);
}

bool ClipboardHistory::SetPinned(const std::wstring &text, bool pinned)
{
    if (text.empty())
        return false;
    NamedMutex lock(kStoreMutexName);
    if (!lock)
        return false;
    auto items = ReadStoreUnlocked();
    if (!items)
        return false;
    const auto found =
        std::find_if(items->begin(), items->end(), [&](const auto &entry) { return entry.text == text; });
    if (found == items->end())
        return false;
    if (found->pinned == pinned)
        return true;
    found->pinned = pinned;
    return WriteStoreUnlocked(*items);
}

ClipboardHistory::Settings ClipboardHistory::LoadSettings()
{
    Settings settings;
    const auto number = [](const std::optional<std::string> &value, int fallback, int low, int high) {
        if (!value)
            return fallback;
        try
        {
            size_t end = 0;
            const int n = std::stoi(*value, &end);
            return end == value->size() && n >= low && n <= high ? n : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    };
    const auto flag = [](const std::optional<std::string> &value, bool fallback) {
        return value ? (*value == "true" ? true : *value == "false" ? false : fallback) : fallback;
    };
    if (const auto sort = ReadConfigString("clipboard", "sort"))
        settings.sortRecent = *sort != "frequent";
    settings.maxItems = static_cast<size_t>(number(ReadConfigString("clipboard", "max_items"), 200, 20, 500));
    settings.retentionDays = number(ReadConfigString("clipboard", "retention_days"), 30, 0, 3650);
    settings.respectPrivacyFlag = flag(ReadConfigString("clipboard", "respect_privacy_flag"), true);
    settings.pauseOnLock = flag(ReadConfigString("clipboard", "pause_on_lock"), true);
    const auto excluded = ReadConfigString("clipboard", "exclude_apps").value_or("KeePass.exe;1Password.exe");
    std::stringstream stream(excluded);
    for (std::string part; std::getline(stream, part, ';');)
    {
        part = TrimCopy(part);
        if (!part.empty())
            settings.excludedApps.push_back(Lower(Utf8ToWide(part)));
    }
    return settings;
}

void ClipboardHistory::Clear()
{
    (void)TryClear();
}
bool ClipboardHistory::TryClear()
{
    NamedMutex lock(kStoreMutexName);
    if (!lock)
        return false;
    const auto path = StorePath();
    if (path.empty())
        return false;
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return !error;
    return WriteStoreUnlocked({});
}

bool ClipboardHistory::IsEnabled()
{
    const auto path = ConfigPath();
    if (path.empty())
        return false;
    std::ifstream input(path);
    if (!input)
        return false;

    bool in_utility = false;
    for (std::string line; std::getline(input, line);)
    {
        const std::string trimmed = TrimCopy(line);
        if (!trimmed.empty() && trimmed.front() == '[')
        {
            in_utility = trimmed == "[utility]";
            continue;
        }
        if (!in_utility || trimmed.empty() || trimmed.front() == '#')
            continue;
        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos)
            continue;
        if (TrimCopy(trimmed.substr(0, equals)) != "clipboard_history")
            continue;
        std::string value = trimmed.substr(equals + 1);
        const size_t comment = value.find('#');
        if (comment != std::string::npos)
            value.resize(comment);
        return ParseBoolToken(std::move(value));
    }
    return false;
}

bool ClipboardHistory::SetEnabled(bool enabled)
{
    if (!WriteEnabledFlag(enabled))
        return false;
    if (!enabled)
        Clear();
    ClipboardMonitor::Sync(enabled);
    return true;
}

std::filesystem::file_time_type ClipboardHistory::StoreWriteTime()
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(StorePath(), error);
    return error ? std::filesystem::file_time_type{} : time;
}

std::filesystem::file_time_type ClipboardHistory::ConfigWriteTime()
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(ConfigPath(), error);
    return error ? std::filesystem::file_time_type{} : time;
}

void ClipboardMonitor::Start()
{
    std::lock_guard<std::mutex> lock(g_monitor.mutex);
    if (g_monitor.running)
        return;
    g_monitor.running = true;
    g_monitor.thread = CreateThread(nullptr, 0, MonitorThread, nullptr, 0, nullptr);
    if (!g_monitor.thread)
        g_monitor.running = false;
}

void ClipboardMonitor::Stop()
{
    std::lock_guard<std::mutex> lock(g_monitor.mutex);
    HANDLE thread = g_monitor.thread;
    if (!thread && !g_monitor.running)
        return;
    g_monitor.thread = nullptr;
    for (int i = 0; i < 50 && !g_monitor.hwnd && g_monitor.running; ++i)
        Sleep(10);
    if (g_monitor.hwnd)
        PostMessageW(g_monitor.hwnd, WM_CLOSE, 0, 0);
    if (thread)
    {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    g_monitor.running = false;
}

void ClipboardMonitor::Sync(bool enabled)
{
    if (enabled)
        Start();
    else
        Stop();
}

bool ClipboardMonitor::IsRunning()
{
    return g_monitor.running;
}

void ClipboardMonitor::SuppressFor(unsigned milliseconds)
{
    g_monitor.suppressUntil = GetTickCount64() + milliseconds;
}
