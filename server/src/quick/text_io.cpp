#include "quick/text_io.h"
#include "clipboard/clipboard_history.h"
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "uiautomationcore.lib")

namespace Quick::TextIo
{
using Microsoft::WRL::ComPtr;
namespace
{
bool OpenClipboardRetry()
{
    for (int i = 0; i < 20; ++i)
    {
        if (OpenClipboard(nullptr))
            return true;
        Sleep(10);
    }
    return false;
}

bool IsGdiHandleFormat(UINT format)
{
    return format == CF_BITMAP || format == CF_PALETTE || format == CF_METAFILEPICT || format == CF_ENHMETAFILE ||
           format == CF_DSPBITMAP || format == CF_DSPMETAFILEPICT || format == CF_DSPENHMETAFILE ||
           (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST);
}

ComPtr<IUIAutomation> Automation()
{
    ComPtr<IUIAutomation> automation;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    return automation;
}

std::wstring TakeBstr(BSTR text)
{
    std::wstring out = text ? std::wstring(text, SysStringLen(text)) : std::wstring();
    SysFreeString(text);
    return out;
}

void SendKey(WORD vk, bool up)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &input, sizeof(input));
}

// A copy event we caused must not show up in the user's clipboard history.
void Quiet()
{
    ClipboardMonitor::SuppressFor(1500);
}

std::wstring CopyAfter(DWORD before)
{
    for (int i = 0; i < 25; ++i)
    {
        Sleep(20);
        if (GetClipboardSequenceNumber() != before)
            return ClipboardText();
    }
    return {};
}
} // namespace

Snapshot Snapshot::Take()
{
    Snapshot snapshot;
    if (!OpenClipboardRetry())
        return snapshot;
    for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format))
    {
        if (IsGdiHandleFormat(format))
            continue;
        HANDLE data = GetClipboardData(format);
        if (!data)
            continue;
        const SIZE_T size = GlobalSize(data);
        if (!size || size > 64u * 1024 * 1024)
            continue;
        if (const void *source = GlobalLock(data))
        {
            Item item;
            item.format = format;
            item.bytes.assign(static_cast<const char *>(source), static_cast<const char *>(source) + size);
            snapshot.items_.push_back(std::move(item));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return snapshot;
}

void Snapshot::Restore() const
{
    Quiet();
    if (!OpenClipboardRetry())
        return;
    EmptyClipboard();
    for (const auto &item : items_)
    {
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, item.bytes.size()))
        {
            if (void *target = GlobalLock(memory))
            {
                memcpy(target, item.bytes.data(), item.bytes.size());
                GlobalUnlock(memory);
                if (SetClipboardData(item.format, memory))
                    continue;
            }
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

bool Snapshot::hasText() const
{
    return std::any_of(items_.begin(), items_.end(), [](const Item &i) { return i.format == CF_UNICODETEXT; });
}

std::wstring ClipboardText()
{
    std::wstring text;
    if (!OpenClipboardRetry())
        return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
        if (const auto *locked = static_cast<const wchar_t *>(GlobalLock(data)))
        {
            text = locked;
            GlobalUnlock(data);
        }
    CloseClipboard();
    return text;
}

bool SetClipboardText(const std::wstring &text)
{
    Quiet();
    if (!OpenClipboardRetry())
        return false;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory)
    {
        if (void *target = GlobalLock(memory))
        {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok)
            GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

void WaitModifiersReleased(int timeoutMs)
{
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < until)
    {
        if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000) &&
            !(GetAsyncKeyState(VK_SHIFT) & 0x8000) && !(GetAsyncKeyState(VK_LWIN) & 0x8000) &&
            !(GetAsyncKeyState(VK_RWIN) & 0x8000))
            return;
        Sleep(15);
    }
}

void PressCtrl(WORD vk)
{
    SendKey(VK_CONTROL, false);
    SendKey(vk, false);
    SendKey(vk, true);
    SendKey(VK_CONTROL, true);
}

std::wstring ReadSelection()
{
    // Many native controls expose their selection without touching the clipboard at all.
    if (auto automation = Automation())
    {
        ComPtr<IUIAutomationElement> element;
        if (SUCCEEDED(automation->GetFocusedElement(&element)) && element)
        {
            ComPtr<IUIAutomationTextPattern> pattern;
            if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) && pattern)
            {
                ComPtr<IUIAutomationTextRangeArray> ranges;
                int length = 0;
                if (SUCCEEDED(pattern->GetSelection(&ranges)) && ranges && SUCCEEDED(ranges->get_Length(&length)))
                {
                    std::wstring text;
                    for (int i = 0; i < length; ++i)
                    {
                        ComPtr<IUIAutomationTextRange> range;
                        BSTR value = nullptr;
                        if (SUCCEEDED(ranges->GetElement(i, &range)) && SUCCEEDED(range->GetText(20000, &value)))
                            text += TakeBstr(value);
                    }
                    if (!text.empty())
                        return text;
                }
            }
        }
    }
    const auto snapshot = Snapshot::Take();
    const DWORD before = GetClipboardSequenceNumber();
    Quiet();
    PressCtrl('C');
    auto text = CopyAfter(before);
    if (GetClipboardSequenceNumber() != before)
        snapshot.Restore();
    return text;
}

std::wstring ReadWholeInput()
{
    if (auto automation = Automation())
    {
        ComPtr<IUIAutomationElement> element;
        if (SUCCEEDED(automation->GetFocusedElement(&element)) && element)
        {
            ComPtr<IUIAutomationValuePattern> value;
            if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value))) && value)
            {
                BSTR text = nullptr;
                if (SUCCEEDED(value->get_CurrentValue(&text)))
                {
                    auto out = TakeBstr(text);
                    if (!out.empty())
                        return out;
                }
            }
            ComPtr<IUIAutomationTextPattern> pattern;
            if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) && pattern)
            {
                ComPtr<IUIAutomationTextRange> range;
                BSTR text = nullptr;
                if (SUCCEEDED(pattern->get_DocumentRange(&range)) && range && SUCCEEDED(range->GetText(20000, &text)))
                {
                    auto out = TakeBstr(text);
                    if (!out.empty())
                        return out;
                }
            }
        }
    }
    const auto snapshot = Snapshot::Take();
    const DWORD before = GetClipboardSequenceNumber();
    Quiet();
    PressCtrl('A');
    Sleep(30);
    PressCtrl('C');
    auto text = CopyAfter(before);
    if (GetClipboardSequenceNumber() != before)
        snapshot.Restore();
    return text;
}

bool PasteText(const std::wstring &text, bool replaceAll)
{
    const auto snapshot = Snapshot::Take();
    if (!SetClipboardText(text))
        return false;
    Sleep(30);
    if (replaceAll)
    {
        PressCtrl('A');
        Sleep(40);
    }
    PressCtrl('V');
    // The target reads the clipboard asynchronously; putting it back too early would paste the old content.
    Sleep(350);
    snapshot.Restore();
    return true;
}

bool FocusRect(RECT &out)
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;
    const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(thread, &info) && info.hwndCaret && !IsRectEmpty(&info.rcCaret))
    {
        POINT topLeft{info.rcCaret.left, info.rcCaret.top}, bottomRight{info.rcCaret.right, info.rcCaret.bottom};
        ClientToScreen(info.hwndCaret, &topLeft);
        ClientToScreen(info.hwndCaret, &bottomRight);
        out = {topLeft.x, topLeft.y, std::max<LONG>(bottomRight.x, topLeft.x + 2), bottomRight.y};
        return true;
    }
    if (auto automation = Automation())
    {
        ComPtr<IUIAutomationElement> element;
        RECT rect{};
        if (SUCCEEDED(automation->GetFocusedElement(&element)) && element &&
            SUCCEEDED(element->get_CurrentBoundingRectangle(&rect)) && rect.right > rect.left && rect.bottom > rect.top)
        {
            out = rect;
            return true;
        }
    }
    return false;
}

std::wstring ProcessNameOf(HWND window)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
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
} // namespace Quick::TextIo
