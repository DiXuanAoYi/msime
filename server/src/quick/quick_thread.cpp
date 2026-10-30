#include "quick/quick_thread.h"
#include "quick/quick_actions.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <set>

namespace Quick
{
namespace
{
constexpr UINT kRunMessage = WM_APP + 41;
constexpr wchar_t kClassName[] = L"MetasequoiaImeQuickActions";

std::mutex g_mutex;
std::thread g_thread;
std::atomic<HWND> g_controller{nullptr};
std::atomic<DWORD> g_threadId{0};
std::atomic<bool> g_running{false};
HHOOK g_hook = nullptr;
KeyClient *g_client = nullptr;
// Keys whose press was swallowed; their release must be swallowed too or the app sees a stray key-up.
std::set<UINT> g_swallowed;

LRESULT CALLBACK HookProc(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && g_client)
    {
        const auto *info = reinterpret_cast<const KBDLLHOOKSTRUCT *>(l);
        if (!(info->flags & LLKHF_INJECTED))
        {
            const bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
            if (!down && g_swallowed.erase(info->vkCode))
                return 1;
            if (down)
            {
                KeyEvent event;
                event.vk = info->vkCode;
                event.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                event.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                event.alt = (info->flags & LLKHF_ALTDOWN) != 0;
                event.win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
                if (g_client->OnKey(event))
                {
                    g_swallowed.insert(info->vkCode);
                    return 1;
                }
            }
            else
            {
                KeyEvent event;
                event.vk = info->vkCode;
                event.down = false;
                g_client->OnKey(event);
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

LRESULT CALLBACK ControllerProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    switch (message)
    {
    case kRunMessage: {
        std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()> *>(l));
        if (fn && *fn)
            (*fn)();
        return 0;
    }
    case WM_HOTKEY:
        Actions::OnHotkey(static_cast<int>(w));
        return 0;
    case WM_TIMER:
        Actions::OnTimer(static_cast<UINT_PTR>(w));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

void ThreadMain()
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW cls{};
    cls.lpfnWndProc = ControllerProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kClassName;
    RegisterClassW(&cls);
    HWND hwnd = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    g_threadId = GetCurrentThreadId();
    g_controller = hwnd;
    g_running = true;
    Actions::OnStart();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Actions::OnStop();
    SetKeyClient(nullptr);
    g_running = false;
    g_controller = nullptr;
    if (hwnd)
        DestroyWindow(hwnd);
    CoUninitialize();
}
} // namespace

void Start()
{
    std::lock_guard lock(g_mutex);
    if (g_running || g_thread.joinable())
        return;
    g_thread = std::thread(ThreadMain);
    for (int i = 0; i < 200 && !g_running; ++i)
        Sleep(5);
}

void Stop()
{
    std::thread joinable;
    {
        std::lock_guard lock(g_mutex);
        if (!g_thread.joinable())
            return;
        if (const DWORD id = g_threadId.load())
            PostThreadMessageW(id, WM_QUIT, 0, 0);
        joinable = std::move(g_thread);
    }
    joinable.join();
}

bool Running()
{
    return g_running;
}

bool OnQuickThread()
{
    return GetCurrentThreadId() == g_threadId.load();
}

void Post(std::function<void()> fn)
{
    const HWND hwnd = g_controller.load();
    if (!hwnd)
        return;
    auto *copy = new std::function<void()>(std::move(fn));
    if (!PostMessageW(hwnd, kRunMessage, 0, reinterpret_cast<LPARAM>(copy)))
        delete copy;
}

void SetKeyClient(KeyClient *client)
{
    g_client = client;
    if (client && !g_hook)
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, GetModuleHandleW(nullptr), 0);
    else if (!client && g_hook)
    {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        g_swallowed.clear();
    }
}

KeyClient *CurrentKeyClient()
{
    return g_client;
}

HWND Controller()
{
    return g_controller.load();
}
} // namespace Quick
