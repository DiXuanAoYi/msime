#include "quick/list_panel.h"
#include "quick/ui_theme.h"
#include <algorithm>

namespace Quick
{
namespace
{
constexpr wchar_t kClass[] = L"MetasequoiaImeQuickListPanel";
constexpr int kPad = 12;
constexpr int kMaxItemLines = 3;

bool IsDigitKey(UINT vk, int &digit)
{
    if (vk >= '1' && vk <= '9')
        digit = static_cast<int>(vk - '0');
    else if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9)
        digit = static_cast<int>(vk - VK_NUMPAD0);
    else
        return false;
    return true;
}

bool IsModifierKey(UINT vk)
{
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL ||
           vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}
} // namespace

ListPanel::ListPanel() = default;

ListPanel::~ListPanel()
{
    if (hwnd_ && IsWindow(hwnd_))
        DestroyWindow(hwnd_);
}

void ListPanel::EnsureWindow()
{
    if (hwnd_)
        return;
    WNDCLASSW cls{};
    cls.lpfnWndProc = Proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP, 0, 0, 10, 10,
                            nullptr, nullptr, cls.hInstance, this);
}

void ListPanel::Open(const RECT &anchor, PanelHandlers handlers, PanelModel model)
{
    EnsureWindow();
    anchor_ = anchor;
    handlers_ = std::move(handlers);
    model_ = std::move(model);
    hover_ = -1;
    open_ = true;
    Reposition();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!model_.passive)
        SetKeyClient(this);
}

void ListPanel::Update(PanelModel model)
{
    if (!open_)
        return;
    model_ = std::move(model);
    Reposition();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ListPanel::Close()
{
    if (!open_)
        return;
    open_ = false;
    if (CurrentKeyClient() == this)
        SetKeyClient(nullptr);
    if (hwnd_)
        ShowWindow(hwnd_, SW_HIDE);
}

void ListPanel::Reposition()
{
    const POINT anchorPoint{anchor_.left, anchor_.top};
    dpi_ = Theme::DpiForPoint(anchorPoint);
    Layout();
    const int width = Theme::Px(model_.width, dpi_);
    const RECT work = Theme::WorkAreaNear(anchor_);
    const int gap = Theme::Px(8, dpi_);
    int x = anchor_.left;
    int y = model_.below ? anchor_.bottom + gap : anchor_.top - gap - height_;
    if (model_.below && y + height_ > work.bottom)
        y = anchor_.top - gap - height_;
    else if (!model_.below && y < work.top)
        y = anchor_.bottom + gap;
    if (y + height_ > work.bottom)
        y = std::max<int>(work.top, work.bottom - height_);
    x = std::max<int>(work.left, std::min<int>(x, work.right - width));
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height_, SWP_NOACTIVATE);
    const int radius = Theme::Px(10, dpi_);
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, width + 1, height_ + 1, radius * 2, radius * 2), TRUE);
}

void ListPanel::Layout()
{
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 1, 1);
    auto old = SelectObject(dc, bitmap);
    RECT client{0, 0, Theme::Px(model_.width, dpi_), 20000};
    Paint(dc, client);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

void ListPanel::Paint(HDC dc, const RECT &client)
{
    const auto palette = Theme::Current();
    const UINT dpi = dpi_;
    const auto px = [&](int v) { return Theme::Px(v, dpi); };
    HFONT normal = Theme::MakeFont(14, false, dpi), bold = Theme::MakeFont(14, true, dpi),
          tiny = Theme::MakeFont(12, false, dpi);
    auto oldFont = SelectObject(dc, normal);
    hits_.clear();
    const int width = client.right - client.left;
    const int left = px(kPad), right = width - px(kPad);
    int y = px(10);
    // Fill is painted last-to-first by area, so draw the card background up front once the height is known.
    struct Deferred
    {
        std::function<void()> draw;
    };
    std::vector<std::function<void()>> late;

    // header
    {
        SelectObject(dc, bold);
        RECT title{left, y, left + px(120), y + px(24)};
        RECT measured = title;
        DrawTextW(dc, model_.title.c_str(), -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int titleWidth = measured.right - measured.left;
        late.push_back([=] {
            SelectObject(dc, bold);
            Theme::PaintText(dc, model_.title, title, palette.accent, DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, normal);
            RECT src{left + titleWidth + px(10), y, right - px(28), y + px(24)};
            Theme::PaintText(dc, model_.source, src, palette.muted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            RECT close{right - px(22), y + px(1), right, y + px(23)};
            Theme::PaintText(dc, L"×", close, palette.muted, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        });
        hits_.push_back({{right - px(24), y, right, y + px(24)}, Hit::Close, 0});
        y += px(30);
    }
    SelectObject(dc, tiny);
    // chips
    if (!model_.chips.empty())
    {
        int x = left;
        for (const auto &chip : model_.chips)
        {
            RECT m{0, 0, 0, 0};
            DrawTextW(dc, chip.c_str(), -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            const int w = m.right - m.left + px(16);
            if (x + w > right)
                break;
            const RECT box{x, y, x + w, y + px(20)};
            const std::wstring text = chip;
            late.push_back([=] {
                SelectObject(dc, tiny);
                Theme::FillRoundRect(dc, box, px(10), palette.bgAlt, palette.line, false);
                Theme::PaintText(dc, text, box, palette.muted, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            });
            x += w + px(6);
        }
        y += px(26);
    }
    // tabs
    if (model_.tabs.size() > 1)
    {
        int x = left;
        for (size_t i = 0; i < model_.tabs.size(); ++i)
        {
            std::wstring label = model_.tabs[i].name + (model_.tabs[i].isDefault ? L" ·默认" : L"");
            SelectObject(dc, normal);
            RECT m{0, 0, 0, 0};
            DrawTextW(dc, label.c_str(), -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            const int w = m.right - m.left + px(20);
            const RECT box{x, y, x + w, y + px(26)};
            const bool selected = static_cast<int>(i) == model_.tab;
            late.push_back([=] {
                SelectObject(dc, selected ? bold : normal);
                if (selected)
                    Theme::FillRoundRect(dc, box, px(13), palette.accentSoft, palette.accent, false);
                Theme::PaintText(dc, label, box, selected ? palette.accent : palette.fg,
                                 DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            });
            hits_.push_back({box, Hit::Tab, static_cast<int>(i)});
            x += w + px(4);
        }
        y += px(32);
    }
    // intent chips
    if (!model_.intents.empty())
    {
        int x = left;
        const std::wstring lead = L"我的意思：";
        SelectObject(dc, tiny);
        RECT m{0, 0, 0, 0};
        DrawTextW(dc, lead.c_str(), -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const RECT leadBox{x, y, x + (m.right - m.left), y + px(22)};
        late.push_back([=] {
            SelectObject(dc, tiny);
            Theme::PaintText(dc, lead, leadBox, palette.muted, DT_SINGLELINE | DT_VCENTER);
        });
        x = leadBox.right + px(4);
        for (size_t i = 0; i < model_.intents.size(); ++i)
        {
            RECT mm{0, 0, 0, 0};
            DrawTextW(dc, model_.intents[i].c_str(), -1, &mm, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            const int w = mm.right - mm.left + px(16);
            if (x + w > right)
                break;
            const RECT box{x, y, x + w, y + px(22)};
            const bool selected = static_cast<int>(i) == model_.intent;
            const std::wstring text = model_.intents[i];
            late.push_back([=] {
                SelectObject(dc, tiny);
                Theme::FillRoundRect(dc, box, px(11), selected ? palette.accentSoft : palette.bg,
                                     selected ? palette.accent : palette.line, true);
                Theme::PaintText(dc, text, box, selected ? palette.accent : palette.fg,
                                 DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            });
            hits_.push_back({box, Hit::Intent, static_cast<int>(i)});
            x += w + px(6);
        }
        y += px(28);
    }
    // search row
    if (model_.searchable)
    {
        const RECT box{left, y, right, y + px(28)};
        const bool empty = model_.query.empty();
        late.push_back([=] {
            SelectObject(dc, normal);
            Theme::FillRoundRect(dc, box, px(6), palette.bgAlt, palette.line, true);
            RECT text{box.left + px(10), box.top, box.right - px(10), box.bottom};
            Theme::PaintText(dc, empty ? L"直接打字搜索，按数字键粘贴" : model_.query + L"│", text,
                             empty ? palette.muted : palette.fg, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        });
        y += px(34);
    }
    // body
    const int badge = px(20);
    const int textLeft = left + badge + px(10);
    SelectObject(dc, normal);
    if (model_.loading)
    {
        for (int i = 0; i < 3; ++i)
        {
            const int w = (right - left) * (i == 0 ? 100 : i == 1 ? 86 : 70) / 100;
            const RECT bar{left, y + px(6), left + w, y + px(18)};
            late.push_back([=] { Theme::FillRoundRect(dc, bar, px(5), palette.hover, palette.hover, false); });
            y += px(28);
        }
    }
    else if (model_.items.empty())
    {
        const RECT note{left, y, right, y + px(48)};
        const std::wstring text = model_.note;
        late.push_back([=] {
            SelectObject(dc, normal);
            Theme::PaintText(dc, text, note, palette.muted, DT_WORDBREAK);
        });
        y += px(52);
    }
    else
    {
        const size_t count = std::min<size_t>(model_.items.size(), 9);
        for (size_t i = 0; i < count; ++i)
        {
            const auto &item = model_.items[i];
            int textTop = y + px(6);
            RECT labelRect{textLeft, textTop, right, textTop};
            if (!item.label.empty())
                textTop += px(16);
            RECT measure{textLeft, 0, right - (item.meta.empty() ? 0 : px(80)), 0};
            DrawTextW(dc, item.text.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            const int lineHeight = px(21);
            int textHeight = std::min<int>(measure.bottom - measure.top, lineHeight * kMaxItemLines);
            textHeight = std::max(textHeight, lineHeight);
            const RECT row{px(4), y, width - px(4), textTop + textHeight + px(6)};
            const RECT textBox{textLeft, textTop, right - (item.meta.empty() ? 0 : px(80)), textTop + textHeight};
            const bool highlighted = static_cast<int>(i) == model_.highlight || static_cast<int>(i) == hover_;
            const RECT badgeBox{left, row.top + px(7), left + badge, row.top + px(7) + badge};
            const PanelItem copy = item;
            const std::wstring number = std::to_wstring(i + 1);
            labelRect.bottom = labelRect.top + px(16);
            late.push_back([=] {
                if (highlighted)
                    Theme::FillRoundRect(dc, row, px(6), palette.hover, palette.hover, false);
                Theme::FillRoundRect(dc, badgeBox, px(4), palette.accentSoft, palette.accent, false);
                SelectObject(dc, tiny);
                Theme::PaintText(dc, number, badgeBox, palette.accent, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
                if (!copy.label.empty())
                    Theme::PaintText(dc, copy.label, labelRect, palette.accent, DT_SINGLELINE | DT_VCENTER);
                SelectObject(dc, normal);
                Theme::PaintText(dc, copy.text, textBox, palette.fg, DT_WORDBREAK | DT_END_ELLIPSIS);
                if (!copy.meta.empty())
                {
                    SelectObject(dc, tiny);
                    RECT meta{right - px(76), row.top + px(7), right, row.top + px(25)};
                    Theme::PaintText(dc, (copy.pinned ? L"● " : L"") + copy.meta, meta, palette.muted,
                                     DT_SINGLELINE | DT_RIGHT | DT_END_ELLIPSIS);
                }
            });
            hits_.push_back({row, Hit::Item, static_cast<int>(i)});
            y = row.bottom + px(2);
        }
    }
    if (!model_.warn.empty())
    {
        const RECT warn{left, y + px(2), right, y + px(20)};
        const std::wstring text = model_.warn;
        late.push_back([=] {
            SelectObject(dc, tiny);
            Theme::PaintText(dc, text, warn, palette.warn, DT_SINGLELINE | DT_END_ELLIPSIS);
        });
        y += px(22);
    }
    // footer
    y += px(4);
    const int footerTop = y;
    const RECT footer{left, y + px(4), right, y + px(24)};
    late.push_back([=] {
        SelectObject(dc, tiny);
        RECT hint = footer;
        hint.right = footer.right - px(130);
        Theme::PaintText(dc, model_.hint, hint, palette.muted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT status = footer;
        status.left = footer.right - px(130);
        Theme::PaintText(dc, model_.status, status, palette.muted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        HPEN pen = CreatePen(PS_SOLID, 1, palette.line);
        auto oldPen = SelectObject(dc, pen);
        MoveToEx(dc, 0, footerTop, nullptr);
        LineTo(dc, width, footerTop);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    });
    y += px(30);
    height_ = y;

    // The card background, then the deferred content on top of it.
    RECT card{0, 0, width, y};
    HBRUSH bg = CreateSolidBrush(palette.bg);
    FillRect(dc, &card, bg);
    DeleteObject(bg);
    for (auto &draw : late)
        draw();
    // Border.
    HPEN pen = CreatePen(PS_SOLID, 1, palette.accent);
    auto oldPen = SelectObject(dc, pen);
    auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 0, 0, width, y, px(20), px(20));
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    SelectObject(dc, oldFont);
    DeleteObject(normal);
    DeleteObject(bold);
    DeleteObject(tiny);
    (void)client;
}

int ListPanel::HitTest(POINT point) const
{
    for (size_t i = 0; i < hits_.size(); ++i)
        if (PtInRect(&hits_[i].rect, point))
            return static_cast<int>(i);
    return -1;
}

void ListPanel::Dispatch(const std::function<void()> &fn)
{
    if (fn)
        Post(fn);
}

void ListPanel::Fire(Hit::Kind kind, int index)
{
    switch (kind)
    {
    case Hit::Item:
        if (handlers_.onPick)
            Dispatch([h = handlers_.onPick, index] { h(index); });
        break;
    case Hit::Tab:
        if (handlers_.onTab)
            Dispatch([h = handlers_.onTab, index] { h(index); });
        break;
    case Hit::Intent:
        if (handlers_.onIntent)
            Dispatch([h = handlers_.onIntent, index] { h(index); });
        break;
    case Hit::Close:
        if (handlers_.onClose)
            Dispatch([h = handlers_.onClose] { h(); });
        break;
    }
}

bool ListPanel::OnKey(const KeyEvent &event)
{
    if (!open_ || !event.down)
        return false;
    if (IsModifierKey(event.vk))
        return false;
    int digit = 0;
    const bool plain = !event.ctrl && !event.alt && !event.win;
    if (event.vk == VK_ESCAPE)
    {
        if (handlers_.onClose)
            Dispatch(handlers_.onClose);
        return true;
    }
    if (plain && IsDigitKey(event.vk, digit) && (!model_.searchable || model_.query.empty()))
    {
        if (digit <= static_cast<int>(model_.items.size()) && handlers_.onPick)
            Dispatch([h = handlers_.onPick, digit] { h(digit - 1); });
        return true;
    }
    if (event.vk == VK_TAB && plain && model_.tabs.size() > 1 && handlers_.onTab)
    {
        const int n = static_cast<int>(model_.tabs.size());
        const int next = (model_.tab + (event.shift ? n - 1 : 1)) % n;
        Dispatch([h = handlers_.onTab, next] { h(next); });
        return true;
    }
    if ((event.vk == VK_DOWN || event.vk == VK_UP) && plain && !model_.items.empty())
    {
        const int n = static_cast<int>(std::min<size_t>(model_.items.size(), 9));
        model_.highlight = (model_.highlight + (event.vk == VK_DOWN ? 1 : n - 1)) % n;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (event.vk == VK_RETURN && plain && !model_.items.empty() && handlers_.onPick)
    {
        const int index = model_.highlight;
        Dispatch([h = handlers_.onPick, index] { h(index); });
        return true;
    }
    if (model_.searchable && plain)
    {
        std::wstring next = model_.query;
        if (event.vk == VK_BACK)
        {
            if (next.empty())
                return true;
            next.pop_back();
        }
        else
        {
            BYTE state[256]{};
            GetKeyboardState(state);
            wchar_t buffer[4]{};
            const UINT scan = MapVirtualKeyW(event.vk, MAPVK_VK_TO_VSC);
            const int n = ToUnicode(event.vk, scan, state, buffer, 4, 0);
            if (n != 1 || buffer[0] < 0x20)
            {
                if (handlers_.onClose)
                    Dispatch(handlers_.onClose);
                return false;
            }
            next.push_back(buffer[0]);
        }
        if (handlers_.onQuery)
            Dispatch([h = handlers_.onQuery, next] { h(next); });
        return true;
    }
    if (plain && !model_.searchable)
    {
        if (event.vk == 'R' && model_.canRefresh && handlers_.onRefresh)
        {
            Dispatch(handlers_.onRefresh);
            return true;
        }
        if (event.vk == 'I' && !model_.intents.empty() && handlers_.onIntent)
        {
            const int n = static_cast<int>(model_.intents.size());
            const int next = model_.intent + 1 >= n ? -1 : model_.intent + 1;
            Dispatch([h = handlers_.onIntent, next] { h(next); });
            return true;
        }
    }
    // Anything else means the user went back to typing: let the key through and get out of the way.
    if (handlers_.onClose)
        Dispatch(handlers_.onClose);
    return false;
}

LRESULT CALLBACK ListPanel::Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    auto *self = reinterpret_cast<ListPanel *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        self = static_cast<ListPanel *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(hwnd, message, w, l);
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        auto old = SelectObject(memory, bitmap);
        self->Paint(memory, client);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        int hover = -1;
        const int hit = self->HitTest(point);
        if (hit >= 0 && self->hits_[hit].kind == Hit::Item)
            hover = self->hits_[hit].index;
        if (hover != self->hover_)
        {
            self->hover_ = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        self->hover_ = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        const int hit = self->HitTest(point);
        if (hit >= 0)
            self->Fire(self->hits_[hit].kind, self->hits_[hit].index);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, w, l);
}
} // namespace Quick
