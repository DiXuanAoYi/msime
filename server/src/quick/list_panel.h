#pragma once
// A numbered list that appears next to the text box the user is typing in, without taking focus.
// It serves 帮我回, 帮我改 and the clipboard panel; the model says what to show, the handlers say what
// each key or click does.

#include "quick/quick_thread.h"
#include <functional>
#include <string>
#include <vector>

namespace Quick
{
struct PanelItem
{
    std::wstring label; // short strategy name shown above the text (optional)
    std::wstring text;
    std::wstring meta; // right-aligned small text, e.g. source app or code
    bool pinned = false;
};

struct PanelTab
{
    std::wstring name;
    bool isDefault = false;
};

struct PanelModel
{
    std::wstring title;  // "帮我回"
    std::wstring source; // one-line quote of what is being answered
    std::vector<std::wstring> chips;
    std::vector<PanelTab> tabs;
    int tab = 0;
    std::vector<std::wstring> intents;
    int intent = -1;
    bool searchable = false;
    std::wstring query;
    std::vector<PanelItem> items;
    int highlight = 0;
    bool loading = false;
    std::wstring note;   // empty-state or error text shown instead of the list
    std::wstring warn;   // small warning line above the footer
    std::wstring hint;   // footer, left
    std::wstring status; // footer, right
    int width = 560;
    bool canRefresh = false;
    bool below = false;   // place the panel under the anchor instead of above it
    bool passive = false; // do not take over the keyboard; only the × button closes it
};

struct PanelHandlers
{
    std::function<void(int)> onPick;
    std::function<void(int)> onTab;
    std::function<void()> onRefresh;
    std::function<void(int)> onIntent;
    std::function<void(const std::wstring &)> onQuery;
    std::function<void()> onClose; // Esc, ×, or the user typing something the panel does not use
};

class ListPanel : public KeyClient
{
  public:
    ListPanel();
    ~ListPanel() override;
    // Quick thread only.
    void Open(const RECT &anchor, PanelHandlers handlers, PanelModel model);
    void Update(PanelModel model);
    void Close();
    bool IsOpen() const
    {
        return open_;
    }
    const PanelModel &model() const
    {
        return model_;
    }
    bool OnKey(const KeyEvent &event) override;

  private:
    struct Hit
    {
        RECT rect;
        enum Kind
        {
            Item,
            Tab,
            Intent,
            Close
        } kind;
        int index;
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void EnsureWindow();
    void Layout();
    void Paint(HDC dc, const RECT &client);
    void Reposition();
    void Dispatch(const std::function<void()> &fn);
    void Fire(Hit::Kind kind, int index);
    int HitTest(POINT point) const;

    HWND hwnd_ = nullptr;
    bool open_ = false;
    RECT anchor_{};
    PanelModel model_;
    PanelHandlers handlers_;
    std::vector<Hit> hits_;
    int hover_ = -1;
    UINT dpi_ = 96;
    int height_ = 0;
};
} // namespace Quick
