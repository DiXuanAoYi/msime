#pragma once
#include <Windows.h>
#include <iomanip>
#include <sstream>
#include <string>
namespace SettingsWebView
{
inline std::wstring FailureMessage(const std::wstring &stage, HRESULT result)
{
    std::wostringstream out;
    out << L"设置页面启动失败：" << stage << L"\nHRESULT 0x" << std::uppercase << std::hex << std::setw(8)
        << std::setfill(L'0') << static_cast<unsigned long>(result) << L"\n\n";
    wchar_t *detail = nullptr;
    const auto count =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t *>(&detail), 0, nullptr);
    if (count && detail)
        out << std::wstring(detail, count) << L"\n";
    LocalFree(detail);
    out << L"请确认 Microsoft Edge WebView2 Runtime 已安装并更新，且设置缓存目录可写；然后重新打开设置。"
           L"\n输入法服务和当前配置仍保留，此错误不会关闭输入法。";
    return out.str();
}
} // namespace SettingsWebView
