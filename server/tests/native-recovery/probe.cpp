#include "webview2/native_small_window_recovery.h"
#include "msimeui/DeviceResources.h"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
constexpr wchar_t kFixtureClass[] = L"MsimeOwnedNativeRecoveryFixture";

LRESULT CALLBACK FixtureWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

class OwnedWindow
{
  public:
    explicit OwnedWindow(const wchar_t *name, int x, int y)
    {
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kFixtureClass, name, WS_POPUP, x, y,
                               240, 72, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(hwnd != nullptr, "Create owned fixture window");
        SetLayeredWindowAttributes(hwnd, 0, 12, LWA_ALPHA);
        SetWindowRgn(hwnd, CreateRectRgn(0, 0, 1, 1), FALSE);
        const BOOL cloak = TRUE;
        Check(SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak))), "Initial DWM cloak");
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    ~OwnedWindow()
    {
        if (hwnd)
            DestroyWindow(hwnd);
    }
    HWND hwnd = nullptr;
};

DWORD Cloak(HWND hwnd)
{
    DWORD value = 0;
    Check(SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))), "Read actual DWM cloak");
    return value;
}

void RollbackFixture(int x, int y)
{
    OwnedWindow window(L"Owned rollback fixture", x, y);
    RECT before{};
    GetWindowRect(window.hwnd, &before);
    {
        NativeSmallWindowRecovery::HostTransition transition(window.hwnd);
        const bool prepared = transition.Prepare();
        if (!prepared)
            std::fprintf(stderr, "Native prepare Win32 error: %lu\n", GetLastError());
        Check(prepared, transition.FailureStage());
        Check(transition.Uncloak(), "Uncloak rollback fixture");
        SetWindowPos(window.hwnd, nullptr, x + 10, y + 10, 400, 120, SWP_NOACTIVATE | SWP_NOZORDER);
    }
    Check((GetWindowLongPtrW(window.hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0, "Rollback layered style");
    Check((Cloak(window.hwnd) & DWM_CLOAKED_APP) != 0, "Rollback cloak");
    Check(IsWindowVisible(window.hwnd), "Rollback visible flag");
    RECT after{};
    GetWindowRect(window.hwnd, &after);
    Check(EqualRect(&before, &after), "Rollback physical geometry");
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    Check(GetWindowRgn(window.hwnd, region) == SIMPLEREGION, "Rollback clip region");
    RECT box{};
    GetRgnBox(region, &box);
    DeleteObject(region);
    Check(box.right == 1 && box.bottom == 1, "Rollback original one-pixel clip");
    BYTE alpha = 0;
    DWORD flags = 0;
    COLORREF color = 0;
    Check(GetLayeredWindowAttributes(window.hwnd, &color, &alpha, &flags) && alpha == 12, "Rollback alpha");
}
} // namespace

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try
    {
        Check(SUCCEEDED(apartment), "Initialize COM");
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = FixtureWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kFixtureClass;
        Check(RegisterClassW(&windowClass) != 0, "Register owned fixture class");
        MONITORINFO monitor{sizeof(monitor)};
        Check(GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor), "Fixture monitor");
        const int x = monitor.rcWork.left + 40;
        const int y = monitor.rcWork.top + 40;
        RollbackFixture(x, y);
        const HWND originalForeground = GetForegroundWindow();
        const std::array<const wchar_t *, 3> names{L"Owned native candidate", L"Owned native toolbar",
                                                   L"Owned native menu"};
        const std::array<COLORREF, 3> expected{RGB(240, 40, 40), RGB(40, 210, 40), RGB(40, 40, 230)};
        std::array<std::unique_ptr<OwnedWindow>, 3> windows;
        std::array<std::unique_ptr<msimeui::DeviceResources>, 3> resources;
        for (size_t i = 0; i < windows.size(); ++i)
        {
            windows[i] = std::make_unique<OwnedWindow>(names[i], x, y + static_cast<int>(i) * 100);
            const HWND hwnd = windows[i]->hwnd;
            Check(Cloak(hwnd) != 0, "Precondition: warmup fixture cloaked");
            NativeSmallWindowRecovery::HostTransition transition(hwnd);
            Check(transition.Prepare(), "Prepare native fixture");
            Check((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0, "Clear layered WebView style");
            resources[i] = std::make_unique<msimeui::DeviceResources>();
            Check(resources[i]->EnsureForComposition(hwnd), "Actual native DComp renderer binding");
            Check(transition.Uncloak(), "Commit uncloak");
            transition.Commit();
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            Check(GetWindowRgn(hwnd, region) == ERROR, "Remove obsolete one-pixel WebView region");
            DeleteObject(region);
            Check(SetWindowPos(hwnd, HWND_TOPMOST, x, y + static_cast<int>(i) * 100, 240, 72,
                               SWP_NOACTIVATE | SWP_SHOWWINDOW),
                  "Restore explicit physical position");
            auto *target = resources[i]->GetRenderTarget();
            target->BeginDraw();
            const COLORREF rgb = expected[i];
            target->Clear(D2D1::ColorF(GetRValue(rgb) / 255.0f, GetGValue(rgb) / 255.0f, GetBValue(rgb) / 255.0f, 1));
            Check(SUCCEEDED(target->EndDraw()) && SUCCEEDED(resources[i]->Present()), "Present actual native frame");
            Check(IsWindowVisible(hwnd) && Cloak(hwnd) == 0, "Native host visible and uncloaked");
            RECT actual{};
            Check(GetWindowRect(hwnd, &actual) && actual.left == x && actual.top == y + static_cast<int>(i) * 100,
                  "Native physical position");
            const POINT pixel{x + 20, actual.top + 20};
            // Hit-test query only. No keyboard/mouse event, input injection or external HWND manipulation.
            Check(SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(pixel.x, pixel.y)) == HTCLIENT,
                  "Visible client area remains hit-testable");
        }
        DwmFlush();
        Sleep(100);
        HDC screen = GetDC(nullptr);
        Check(screen != nullptr, "Read owned opaque fixture pixels");
        for (size_t i = 0; i < windows.size(); ++i)
        {
            const COLORREF actual = GetPixel(screen, x + 20, y + static_cast<int>(i) * 100 + 20);
            const COLORREF wanted = expected[i];
            std::fprintf(stderr, "Owned native pixels %zu: actual=%06lx expected=%06lx\n", i,
                         static_cast<unsigned long>(actual), static_cast<unsigned long>(wanted));
            // The desktop compositor applies monitor colour management (this machine maps
            // 240/40/40 to 255/9/9). This checks the visible distinct colour, not offscreen RGB parity.
            const auto closeEnough = [](int a, int b) { return a - b <= 40 && b - a <= 40; };
            Check(closeEnough(GetRValue(actual), GetRValue(wanted)) &&
                      closeEnough(GetGValue(actual), GetGValue(wanted)) &&
                      closeEnough(GetBValue(actual), GetBValue(wanted)),
                  "Actual visible native composition pixels");
        }
        ReleaseDC(nullptr, screen);
        Check(GetForegroundWindow() == originalForeground, "Owned fixtures never steal foreground");
        std::printf("{\"passed\":true,\"owned_native_hosts\":3,\"dpi\":%u,\"rollback\":true,\"cloak\":0,"
                    "\"regions_cleared\":true,\"physical_positions\":true,\"native_dcomp_pixels\":true,"
                    "\"hit_test_queries\":true,\"foreground_preserved\":true,\"input_injection\":false}\n",
                    GetDpiForWindow(windows[0]->hwnd));
        resources = {};
        windows = {};
        CoUninitialize();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        if (SUCCEEDED(apartment))
            CoUninitialize();
        return 1;
    }
}
