#pragma once
#include <Windows.h>
#include <string>
#include <utility>

namespace NativeCommitUndo
{
inline constexpr UINT CaptureMessage = WM_APP + 0x519;
// Legacy EDIT exposes only the live preedit to TSF. Its committed document
// must be validated through the control, never through synthetic Backspace.
class Snapshot
{
    HWND window_ = nullptr;
    DWORD caret_ = 0;
    std::wstring expected_, document_;
    bool ready_ = false;
    static bool Supported(HWND window)
    {
        wchar_t name[64]{};
        const auto style = GetWindowLongPtrW(window, GWL_STYLE);
        return window && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId() && IsWindowUnicode(window) &&
               !(style & (ES_PASSWORD | ES_READONLY)) && GetClassNameW(window, name, 64) &&
               _wcsicmp(name, L"Edit") == 0;
    }
    static bool Read(HWND window, std::wstring &text)
    {
        const int length = GetWindowTextLengthW(window);
        if (length < 0 || length > 65536)
            return false;
        text.resize(static_cast<size_t>(length) + 1);
        const int copied = GetWindowTextW(window, text.data(), length + 1);
        text.resize(static_cast<size_t>(copied));
        return copied == length;
    }
    static bool Caret(HWND window, DWORD &position)
    {
        DWORD start = 0, end = 0;
        SendMessageW(window, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        position = end;
        return start == end;
    }

  public:
    void Clear()
    {
        window_ = nullptr;
        caret_ = 0;
        expected_.clear();
        document_.clear();
        ready_ = false;
    }
    void Arm(HWND window, const std::wstring &expected)
    {
        Clear();
        if (Supported(window) && !expected.empty() && expected.size() <= 4096)
        {
            window_ = window;
            expected_ = expected;
        }
    }
    bool Capture(HWND focus)
    {
        ready_ = false;
        if (focus != window_ || !Supported(window_) || !Caret(window_, caret_) || !Read(window_, document_) ||
            caret_ > document_.size() || caret_ < expected_.size() ||
            document_.compare(caret_ - expected_.size(), expected_.size(), expected_) != 0)
            return false;
        ready_ = true;
        return true;
    }
    bool Ready() const
    {
        return ready_;
    }
    template <class StillCurrent> bool Undo(HWND focus, StillCurrent stillCurrent)
    {
        if (!ready_)
            return false;
        ready_ = false;
        DWORD caret = 0;
        std::wstring current;
        if (focus != window_ || !Supported(window_) || !stillCurrent() || !Caret(window_, caret) || caret != caret_ ||
            !Read(window_, current) || current != document_)
            return false;
        const auto start = static_cast<DWORD>(caret_ - expected_.size());
        SendMessageW(window_, EM_SETSEL, start, caret_);
        // Selection notifications can reenter a host. Revalidate before writing.
        if (!stillCurrent() || !Read(window_, current) || current != document_)
            return false;
        DWORD selectedStart = 0, selectedEnd = 0;
        SendMessageW(window_, EM_GETSEL, reinterpret_cast<WPARAM>(&selectedStart),
                     reinterpret_cast<LPARAM>(&selectedEnd));
        if (selectedStart != start || selectedEnd != caret_ || !stillCurrent())
            return false;
        SendMessageW(window_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
        return true;
    }
};
} // namespace NativeCommitUndo
