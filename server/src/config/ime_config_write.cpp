// 把设置写回 config.toml（批量、保留格式、先校验再原子替换），并通知 Server 重新加载配置或切换输入方案。
#include "config/ime_config_internal.h"
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include "defines/defines.h"
#include "ipc/ipc.h"

using namespace ime_config_detail;

namespace
{
// This opt-in diagnostic deliberately records metadata only: never log setting values or credentials.
void TraceConfigWrite(const char *stage, const std::vector<ConfigValueUpdate> &updates, DWORD error = ERROR_SUCCESS)
{
    unsigned long dos_error = 0;
    _get_doserrno(&dos_error);
    wchar_t enabled[8]{};
    if (!GetEnvironmentVariableW(L"METASEQUOIA_IME_SETTINGS_TRACE", enabled, 8) || enabled[0] != L'1')
        return;
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp))
        return;
    FILE *file = nullptr;
    const auto log = std::filesystem::path(temp) / L"metasequoia-config-write.log";
    if (_wfopen_s(&file, log.c_str(), L"a") != 0 || !file)
        return;
    const auto path = g_config_path.u8string();
    fprintf(file, "pid=%lu stage=%s win32=%lu dos=%lu path=%s", GetCurrentProcessId(), stage, error, dos_error,
            reinterpret_cast<const char *>(path.c_str()));
    for (const auto &update : updates)
        fprintf(file, " key=%s.%s", update.section.c_str(), update.key.c_str());
    fprintf(file, "\n");
    fclose(file);
}
} // namespace

namespace ime_config_detail
{
bool WriteConfiguredValues(const std::vector<ConfigValueUpdate> &updates)
{
    ConfigFileLock lock;
    if (!lock)
    {
        TraceConfigWrite("lock-failed", updates, lock.error());
        return false;
    }
    std::error_code exists_error;
    const bool config_exists = std::filesystem::is_regular_file(g_config_path, exists_error);
    const auto config_size =
        config_exists ? std::filesystem::file_size(g_config_path, exists_error) : std::uintmax_t{0};
    std::string text = ReadFileText(g_config_path);
    if (!TomlTextIsParseable(text))
    {
        if (config_exists && config_size > 0 && text.empty())
        {
            TraceConfigWrite("read-failed", updates, GetLastError());
            return false;
        }
        text = ReadFileText(g_config_path.parent_path() / kConfigTemplateFileName);
        if (!TomlTextIsParseable(text))
        {
            TraceConfigWrite("template-invalid", updates);
            return false;
        }
    }

    for (const auto &update : updates)
    {
        if (!ReplaceTomlValuePreservingFormatting(text, update.section, update.key, update.value) &&
            !InsertTomlValuePreservingFormatting(text, update.section, update.key, update.value))
        {
            TraceConfigWrite("replace-failed", updates);
            return false;
        }
    }

    try
    {
        (void)toml::parse(text);
    }
    catch (const toml::parse_error &)
    {
        TraceConfigWrite("updated-toml-invalid", updates);
        return false;
    }

    // Write a temp file and rename it so a crash can never leave the user with a truncated config. This only makes the
    // directory entry swap atomic: there is no FlushFileBuffers, so a power loss can still lose the contents.
    SetLastError(ERROR_SUCCESS);
    _set_doserrno(0);
    if (!WriteFileTextAtomically(g_config_path, text))
    {
        TraceConfigWrite("write-failed", updates, GetLastError());
        return false;
    }

    RememberConfigWriteTime();
    NotifyImeServerConfigChanged();
    TraceConfigWrite("saved", updates);
    return true;
}

bool WriteConfiguredValue(const std::string &section, const std::string &key, const std::string &replacement)
{
    return WriteConfiguredValues({{section, key, replacement}});
}
} // namespace ime_config_detail

namespace
{
HWND FindImeServerCandidateWindow()
{
    // Candidate, tray menu, and floating toolbar share the same class. FindWindow
    // without a title often hits the toolbar, which does not apply config.
    if (const HWND hwnd = FindWindowW(L"metasequoiaime_windows", L"metaseuqoiaimecandwnd"))
    {
        return hwnd;
    }
    return FindWindowW(L"metasequoiaime_windows", nullptr);
}

bool SendAuxConfigNotification(const wchar_t *message)
{
    HANDLE pipe = CreateFileW(FANY_IME_AUX_NAMED_PIPE, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(FANY_IME_AUX_NAMED_PIPE, 200))
    {
        pipe = CreateFileW(FANY_IME_AUX_NAMED_PIPE, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD bytesWritten = 0;
    const DWORD byteCount = static_cast<DWORD>(wcslen(message) * sizeof(wchar_t));
    const bool sent = WriteFile(pipe, message, byteCount, &bytesWritten, nullptr) != FALSE && bytesWritten == byteCount;
    CloseHandle(pipe);
    return sent;
}
} // namespace

namespace ime_config_detail
{
void NotifyImeServer(UINT windowMessage, const wchar_t *auxMessage, WPARAM wParam)
{
    const HWND hwnd = FindImeServerCandidateWindow();
    DWORD serverProcessId = 0;
    if (hwnd)
    {
        GetWindowThreadProcessId(hwnd, &serverProcessId);
    }

    if (serverProcessId == GetCurrentProcessId())
    {
        PostMessageW(hwnd, windowMessage, wParam, 0);
        return;
    }

    // MetasequoiaImeServer runs with uiAccess while the standalone Settings
    // process does not. Cross-process WM_USER delivery can therefore be
    // rejected by UIPI. The session-less Aux pipe is already the supported
    // cross-integrity control path; use it for config invalidation as well.
    if (!SendAuxConfigNotification(auxMessage) && hwnd)
    {
        // Retain the old route as a best-effort fallback. The Server's periodic
        // file watcher remains the final recovery path if both transports are
        // temporarily unavailable during startup.
        PostMessageW(hwnd, windowMessage, wParam, 0);
    }
}
} // namespace ime_config_detail

void NotifyImeServerConfigChanged()
{
    NotifyImeServer(WM_APPLY_IME_CONFIG, L"ConfigChanged");
}

void NotifyImeServerCandidateSkinRefresh()
{
    NotifyImeServer(WM_APPLY_IME_CONFIG, L"CandidateSkinRefresh", 1);
}

void NotifyImeServerInputSchemeChanged()
{
    NotifyImeServer(WM_APPLY_IME_INPUT_SCHEME, L"InputSchemeChanged");
}

bool NotifyImeServerRestart()
{
    // No window message carries a restart, so there is no PostMessage fallback
    // here: if the Aux pipe is unavailable the Server is not running anyway, and
    // the caller reports that instead of silently doing nothing.
    return SendAuxConfigNotification(L"RestartServer");
}
