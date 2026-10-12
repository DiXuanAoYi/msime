#pragma once
#include <algorithm>
#include <windows.h>

namespace toolbar_placement
{
// All coordinates/insets are physical pixels from the host's current DPI.
inline RECT VisibleContent(RECT host, const RECT &insets)
{
    RECT content{host.left + (std::max)(0L, insets.left), host.top + (std::max)(0L, insets.top),
                 host.right - (std::max)(0L, insets.right), host.bottom - (std::max)(0L, insets.bottom)};
    return content.right > content.left && content.bottom > content.top ? content : host;
}
inline POINT Center(const RECT &content)
{
    return {content.left + (content.right - content.left) / 2, content.top + (content.bottom - content.top) / 2};
}
inline RECT FitVisibleContent(RECT host, const RECT &bounds, const RECT &insets)
{
    const auto content = VisibleContent(host, insets);
    const auto maxX = (std::max)(bounds.left, bounds.right - (content.right - content.left));
    const auto maxY = (std::max)(bounds.top, bounds.bottom - (content.bottom - content.top));
    const auto x = (std::max)(bounds.left, (std::min)(content.left, maxX));
    const auto y = (std::max)(bounds.top, (std::min)(content.top, maxY));
    OffsetRect(&host, x - content.left, y - content.top);
    return host;
}
inline POINT KeepVisibleOrigin(POINT host, const RECT &oldInsets, const RECT &newInsets)
{
    return {host.x + oldInsets.left - newInsets.left, host.y + oldInsets.top - newInsets.top};
}
} // namespace toolbar_placement
