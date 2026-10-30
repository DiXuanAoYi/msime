#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace NativeSmallWindowRecovery
{
// WebView warmup owns layered/cloak/clip state. A renderer swap must reconcile it.
// Keep the previous state until all three native presenters have bound successfully.
class HostTransition
{
  public:
    explicit HostTransition(HWND hwnd) : hwnd_(hwnd)
    {
        valid_ = IsWindow(hwnd_) && GetWindowRect(hwnd_, &bounds_);
        if (!valid_)
            return;
        style_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        visible_ = IsWindowVisible(hwnd_) != FALSE;
        DwmGetWindowAttribute(hwnd_, DWMWA_CLOAKED, &cloak_, sizeof(cloak_));
        layered_ = (style_ & WS_EX_LAYERED) && GetLayeredWindowAttributes(hwnd_, &color_, &alpha_, &alphaFlags_);
        region_ = CreateRectRgn(0, 0, 0, 0);
        if (region_ && GetWindowRgn(hwnd_, region_) == ERROR)
        {
            DeleteObject(region_);
            region_ = nullptr;
        }
    }

    ~HostTransition()
    {
        if (prepared_ && !committed_)
            Rollback();
        if (region_)
            DeleteObject(region_);
    }
    HostTransition(const HostTransition &) = delete;
    HostTransition &operator=(const HostTransition &) = delete;

    bool Prepare()
    {
        if (!valid_)
            return false;
        prepared_ = true;
        ShowWindow(hwnd_, SW_HIDE); // Clear cloak only while hidden; never flash an empty warmup host.
        failureStage_ = "clear old region";
        if (!SetWindowRgn(hwnd_, nullptr, FALSE))
            return false;
        SetLastError(0);
        const LONG_PTR nativeStyle = style_ & ~WS_EX_LAYERED;
        failureStage_ = "clear layered style";
        if (!SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, nativeStyle) && GetLastError())
            return false;
        // NOREDIRECTIONBITMAP is a creation-time style: Windows rejects adding it here (87).
        // DirectComposition also binds an existing ordinary host. Retain full glass so its
        // empty redirected background remains transparent outside the native visual.
        const MARGINS margins{-1, -1, -1, -1};
        failureStage_ = "prepare transparent composition frame";
        if (FAILED(DwmExtendFrameIntoClientArea(hwnd_, &margins)))
            return false;
        failureStage_ = "refresh native frame";
        if (!SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED))
            return false;
        failureStage_ = "none";
        return true;
    }

    const char *FailureStage() const
    {
        return failureStage_;
    }

    bool Uncloak()
    {
        const BOOL cloak = FALSE;
        return prepared_ && SUCCEEDED(DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &cloak, sizeof(cloak)));
    }

    void Commit()
    {
        committed_ = true;
    }

  private:
    void Rollback()
    {
        if (!IsWindow(hwnd_))
            return;
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style_);
        if (layered_)
            SetLayeredWindowAttributes(hwnd_, color_, alpha_, alphaFlags_);
        const MARGINS margins{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &margins);
        const BOOL cloak = (cloak_ & DWM_CLOAKED_APP) != 0;
        DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &cloak, sizeof(cloak));
        if (SetWindowRgn(hwnd_, region_, FALSE))
            region_ = nullptr; // USER owns the region after a successful call.
        SetWindowPos(hwnd_, nullptr, bounds_.left, bounds_.top, bounds_.right - bounds_.left,
                     bounds_.bottom - bounds_.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }

    HWND hwnd_ = nullptr;
    RECT bounds_{};
    LONG_PTR style_ = 0;
    HRGN region_ = nullptr;
    DWORD cloak_ = 0;
    COLORREF color_ = 0;
    BYTE alpha_ = 255;
    DWORD alphaFlags_ = 0;
    bool valid_ = false;
    bool visible_ = false;
    bool layered_ = false;
    bool prepared_ = false;
    bool committed_ = false;
    const char *failureStage_ = "invalid HWND";
};
} // namespace NativeSmallWindowRecovery
