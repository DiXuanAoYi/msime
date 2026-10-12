#pragma once

#include <memory>
#include <windows.h>

class FloatingToolbarPresenter
{
  public:
    static FloatingToolbarPresenter &Instance();

    bool Bind(HWND hwnd);
    bool IsBound() const;
    void Present();
    void ApplyTheme();
    void ApplyAppearance();
    void RelayoutHost(FLOAT scaleOverride = 0.0f, const RECT *suggestedRect = nullptr);
    RECT ShadowInsetsPx(int hostWidthPx, int hostHeightPx, FLOAT scale) const;
    void SyncUi(int cnEn, int doubleSingleByte, int punctuation, int englishInputMode, int capsLock,
                int japaneseInputMode);
    bool HitCaptionDrag(POINT clientPoint) const;
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

  private:
    FloatingToolbarPresenter();
    ~FloatingToolbarPresenter();
    FloatingToolbarPresenter(const FloatingToolbarPresenter &) = delete;
    FloatingToolbarPresenter &operator=(const FloatingToolbarPresenter &) = delete;

    void RebuildScene();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    HWND hwnd_ = nullptr;
    bool bound_ = false;
};
