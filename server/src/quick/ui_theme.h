#pragma once
// Colors and metrics shared by the quick-action windows. They follow the Windows "apps" light/dark
// setting and use the same purple accent as the settings window.

#include <Windows.h>
#include <ShellScalingApi.h>
#include <string>

#pragma comment(lib, "Shcore.lib")

namespace Quick::Theme
{
struct Palette
{
    COLORREF bg, bgAlt, fg, muted, line, accent, accentSoft, hover, warn, good, onAccent, overlayDim;
    bool dark;
};

inline bool AppsUseDarkTheme()
{
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

inline Palette Current()
{
    if (AppsUseDarkTheme())
        return {RGB(43, 43, 43),
                RGB(37, 37, 37),
                RGB(243, 243, 243),
                RGB(169, 169, 169),
                RGB(72, 72, 72),
                RGB(180, 178, 242),
                RGB(62, 61, 96),
                RGB(58, 58, 62),
                RGB(241, 182, 92),
                RGB(99, 212, 157),
                RGB(255, 255, 255),
                RGB(0, 0, 0),
                true};
    return {RGB(255, 255, 255),
            RGB(246, 246, 248),
            RGB(36, 36, 36),
            RGB(92, 92, 92),
            RGB(222, 222, 226),
            RGB(91, 87, 184),
            RGB(232, 231, 248),
            RGB(241, 241, 245),
            RGB(138, 83, 0),
            RGB(22, 121, 74),
            RGB(255, 255, 255),
            RGB(0, 0, 0),
            false};
}

inline UINT DpiForPoint(POINT point)
{
    UINT x = 96, y = 96;
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x)
        return x;
    return 96;
}

inline int Px(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

inline HFONT MakeFont(int logicalHeight, bool bold, UINT dpi)
{
    return CreateFontW(-Px(logicalHeight, dpi), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       L"Microsoft YaHei UI");
}

inline RECT WorkAreaNear(const RECT &rect)
{
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info))
        return info.rcWork;
    return {0, 0, 1920, 1080};
}

inline void FillRoundRect(HDC dc, const RECT &rect, int radius, COLORREF fill, COLORREF border, bool hasBorder)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = hasBorder ? CreatePen(PS_SOLID, 1, border) : static_cast<HPEN>(GetStockObject(NULL_PEN));
    auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right + 1, rect.bottom + 1, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
    if (hasBorder)
        DeleteObject(pen);
}

inline void PaintText(HDC dc, const std::wstring &text, RECT rect, COLORREF color, UINT flags)
{
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
}
} // namespace Quick::Theme
