// Read-only UI Automation probe. Run out of process: a broken provider cannot hang the IME.
#include <Windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include "chat_reader.h"
#include <iostream>
#include <string>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
using Microsoft::WRL::ComPtr;
using nlohmann::json;
namespace
{
// Local OCR supplements only missing WeChat sender labels. Pixels never leave this process.
json ReadLabels(HWND hwnd, json &nodes)
{
    json lines = json::array();
    RECT bounds{};
    GetWindowRect(hwnd, &bounds);
    int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0 || width > 3500 || height > 2500)
        return lines;
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap)
    {
        DeleteDC(memory);
        ReleaseDC(hwnd, dc);
        return lines;
    }
    auto old = SelectObject(memory, bitmap);
    bool captured = PrintWindow(hwnd, memory, 2) != FALSE;
    if (captured)
    {
        // WeChat 4 exposes full-width rows with no accessible avatar children.
        // Identify only text bubbles with a sustained outgoing green surface
        // immediately left of the right-hand avatar; never infer self from text X alone.
        const double scale = GetDpiForWindow(hwnd) / 96.0;
        auto *rgba = static_cast<uint8_t *>(pixels);
        for (auto &n : nodes)
        {
            if (n.value("class", "") != "mmui::ChatTextItemView" || n.value("offscreen", false))
                continue;
            const int rowRight = n.value("x", 0) + n.value("width", 0) - bounds.left;
            const int rowTop = n.value("y", 0) - bounds.top;
            const int x1 = rowRight - int(110 * scale), x2 = rowRight - int(85 * scale);
            const int y1 = std::max(0, rowTop + int(12 * scale));
            const int y2 = std::min(height, rowTop + std::min(n.value("height", 0), int(100 * scale)));
            if (x1 < 0 || x2 >= width || x2 <= x1 || y2 <= y1)
                continue;
            int filledRows = 0;
            for (int y = y1; y < y2; ++y)
            {
                int green = 0;
                for (int x = x1; x < x2; ++x)
                {
                    const auto at = (size_t(y) * width + x) * 4;
                    const int b = rgba[at], g = rgba[at + 1], r = rgba[at + 2];
                    green += g > 75 && g - r > 25 && g - b > 18;
                }
                filledRows += green * 5 >= (x2 - x1) * 3;
            }
            if (filledRows >= 16 * scale)
                n["directionEvidence"] = "wechat-green-right-text-bubble";
            else
            {
                // The private-chat contact has no name above a left-hand bubble.
                // Require the neutral bubble surface and a distinct empty gutter;
                // an unrecognized theme remains unknown instead of assuming incoming.
                const int rowLeft = n.value("x", 0) - bounds.left;
                const int a = rowLeft + int(90 * scale), b = rowLeft + int(108 * scale);
                const int gutter = rowLeft + int(54 * scale);
                int neutralRows = 0;
                if (a >= 0 && b < width && gutter >= 0)
                    for (int y = y1; y < y2; ++y)
                    {
                        const size_t bg = (size_t(y) * width + gutter) * 4;
                        const int background = (rgba[bg] + rgba[bg + 1] + rgba[bg + 2]) / 3;
                        int filled = 0;
                        for (int x = a; x < b; ++x)
                        {
                            const size_t at = (size_t(y) * width + x) * 4;
                            const int blue = rgba[at], green = rgba[at + 1], red = rgba[at + 2];
                            const int shade = (red + green + blue) / 3;
                            filled += std::abs(red - green) < 8 && std::abs(green - blue) < 8 &&
                                      shade - background >= 10 && shade - background <= 100;
                        }
                        neutralRows += filled * 5 >= (b - a) * 3;
                    }
                if (neutralRows >= 16 * scale)
                    n["directionEvidence"] = "wechat-neutral-left-text-bubble";
            }
        }
        try
        {
            using namespace winrt::Windows;
            auto engine = Media::Ocr::OcrEngine::TryCreateFromLanguage(Globalization::Language(L"zh-Hans"));
            if (engine)
            {
                // OCR only the narrow sender strip above each WeChat group bubble.
                // Upscaling/contrast preserves small grey nicknames on dark themes.
                double dpi = GetDpiForWindow(hwnd) / 96.0;
                std::vector<RECT> strips;
                for (const auto &n : nodes)
                {
                    if (n.value("class", "") != "mmui::ChatTextItemView")
                        continue;
                    RECT r{LONG(n.value("x", 0) + 60 * dpi) - bounds.left, LONG(n.value("y", 0) + 4 * dpi) - bounds.top,
                           LONG(n.value("x", 0) + 340 * dpi) - bounds.left,
                           LONG(n.value("y", 0) + 27 * dpi) - bounds.top};
                    if (r.left >= 0 && r.top >= 0 && r.right <= width && r.bottom <= height)
                        strips.push_back(r);
                    if (strips.size() == 20)
                        break;
                }
                if (strips.empty())
                    throw std::runtime_error("no sender strips");
                int stripWidth = (strips[0].right - strips[0].left) * 3,
                    stripHeight = (strips[0].bottom - strips[0].top) * 3 + 18;
                int totalHeight = stripHeight * static_cast<int>(strips.size());
                if (totalHeight > 3500)
                    throw std::runtime_error("too many strips");
                std::vector<uint8_t> normalized(size_t(stripWidth) * totalHeight * 4, 255);
                auto *src = static_cast<uint8_t *>(pixels);
                for (size_t i = 0; i < strips.size(); ++i)
                    for (int y = 0; y < (strips[i].bottom - strips[i].top) * 3; ++y)
                        for (int x = 0; x < stripWidth; ++x)
                        {
                            size_t source = (size_t(strips[i].top + y / 3) * width + strips[i].left + x / 3) * 4;
                            int shade = (int(src[source]) + src[source + 1] + src[source + 2]) / 3;
                            // Determine background from the left margin of this sender strip.
                            size_t bg = (size_t(strips[i].top) * width + strips[i].left) * 4;
                            int background = (int(src[bg]) + src[bg + 1] + src[bg + 2]) / 3;
                            uint8_t v = static_cast<uint8_t>(std::clamp(
                                background < 128 ? 255 - (shade - background) * 4 : 255 - (background - shade) * 4, 0,
                                255));
                            size_t target = ((i * stripHeight + y) * stripWidth + x) * 4;
                            normalized[target] = normalized[target + 1] = normalized[target + 2] = v;
                        }
                // Separate recognition avoids the OCR engine interpreting repeated
                // Chinese nicknames in a vertical column as one vertical text line.
                for (size_t strip = 0; strip < strips.size(); ++strip)
                {
                    size_t start = strip * stripWidth * stripHeight * 4;
                    std::vector<uint8_t> crop(normalized.begin() + start,
                                              normalized.begin() + start + stripWidth * stripHeight * 4);
                    auto bytes = Security::Cryptography::CryptographicBuffer::CreateFromByteArray(crop);
                    auto image = Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
                        bytes, Graphics::Imaging::BitmapPixelFormat::Bgra8, stripWidth, stripHeight,
                        Graphics::Imaging::BitmapAlphaMode::Ignore);
                    auto result = engine.RecognizeAsync(image).get();
                    for (const auto &line : result.Lines())
                    {
                        float left = float(stripWidth), top = float(stripHeight), right = 0, bottom = 0;
                        std::wstring text;
                        for (const auto &word : line.Words())
                        {
                            auto r = word.BoundingRect();
                            left = std::min(left, r.X);
                            top = std::min(top, r.Y);
                            right = std::max(right, r.X + r.Width);
                            bottom = std::max(bottom, r.Y + r.Height);
                            auto part = std::wstring(word.Text());
                            if (!text.empty() && !part.empty() && text.back() < 128 && part.front() < 128)
                                text += L' ';
                            text += part;
                        }
                        lines.push_back({{"text", winrt::to_string(text)},
                                         {"x", bounds.left + strips[strip].left + left / 3},
                                         {"y", bounds.top + strips[strip].top + top / 3},
                                         {"width", (right - left) / 3},
                                         {"height", (bottom - top) / 3}});
                    }
                }
            }
        }
        catch (...)
        { /* Missing OCR language or unavailable capture: keep sender unknown. */
        }
    }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(hwnd, dc);
    return lines;
}

// WeCom 5 uses a custom drawn window with no message UIA provider.
// This local-only fallback accepts the observed light theme and isolates colored
// bubble components from the composer, navigation and group member sidebar.
json ReadWeCom(HWND hwnd, bool qq = false)
{
    json result = {{"lines", json::array()}, {"bubbles", json::array()}};
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    double scale = GetDpiForWindow(hwnd) / 96.0;
    if (width < 600 * scale || width > 2300 || height < 350 * scale || height > 1900)
        return result;
    HDC dc = GetDC(hwnd), mem = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap)
    {
        DeleteDC(mem);
        ReleaseDC(hwnd, dc);
        return result;
    }
    auto old = SelectObject(mem, bitmap);
    if (PrintWindow(hwnd, mem, 2))
        try
        {
            using namespace winrt::Windows;
            auto engine = Media::Ocr::OcrEngine::TryCreateFromLanguage(Globalization::Language(L"zh-Hans"));
            if (engine)
            {
                auto *src = static_cast<uint8_t *>(pixels);
                std::vector<uint8_t> enlarged(size_t(width) * height * 16, 255);
                for (int y = 0; y < height * 2; ++y)
                    for (int x = 0; x < width * 2; ++x)
                    {
                        size_t a = (size_t(y / 2) * width + x / 2) * 4, b = (size_t(y) * width * 2 + x) * 4;
                        for (int c = 0; c < 3; ++c)
                            enlarged[b + c] = src[a + c];
                    }
                auto bytes = Security::Cryptography::CryptographicBuffer::CreateFromByteArray(enlarged);
                auto image = Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
                    bytes, Graphics::Imaging::BitmapPixelFormat::Bgra8, width * 2, height * 2,
                    Graphics::Imaging::BitmapAlphaMode::Ignore);
                auto ocr = engine.RecognizeAsync(image).get();
                for (const auto &line : ocr.Lines())
                {
                    double left = width * 2, top = height * 2, right = 0, bottom = 0;
                    std::wstring text;
                    for (const auto &word : line.Words())
                    {
                        auto b = word.BoundingRect();
                        left = std::min(left, double(b.X));
                        top = std::min(top, double(b.Y));
                        right = std::max(right, double(b.X + b.Width));
                        bottom = std::max(bottom, double(b.Y + b.Height));
                        auto part = std::wstring(word.Text());
                        if (!text.empty() && !part.empty() && text.back() < 128 && part.front() < 128)
                            text += L' ';
                        text += part;
                    }
                    result["lines"].push_back({{"text", winrt::to_string(text)},
                                               {"x", left / 2 / scale},
                                               {"y", top / 2 / scale},
                                               {"width", (right - left) / 2 / scale},
                                               {"height", (bottom - top) / 2 / scale}});
                }
                // Locate the actual sidebar boundary from a persistent vertical edge,
                // measured in physical pixels. The OCR geometry is converted once.
                auto luminance = [&](int x, int y) {
                    size_t a = (size_t(y) * width + x) * 4;
                    return (int(src[a]) + src[a + 1] + src[a + 2]) / 3;
                };
                int divider = 0;
                double best = 0;
                for (int x = int(180 * scale); x < std::min(width / 2, int(440 * scale)); ++x)
                {
                    double score = 0;
                    for (int y = int(75 * scale); y < height - int(35 * scale); y += 3)
                        score += std::abs(luminance(x - 2, y) - luminance(x + 2, y));
                    if (score > best)
                    {
                        best = score;
                        divider = x;
                    }
                }
                if (!divider || best < height * 0.5)
                    throw std::runtime_error("sidebar not found");
                int left = divider + int(12 * scale), top = int(70 * scale);
                int right = width - int(16 * scale), bottom = height - int(145 * scale);
                for (const auto &line : result["lines"])
                {
                    auto text = line.value("text", "");
                    double x = line["x"], y = line["y"];
                    if ((text.find("群成员") != std::string::npos ||
                         (text.find("群") != std::string::npos && text.find("成员") != std::string::npos) ||
                         text == "群公告") &&
                        x * scale > width * 0.64)
                        right = std::min(right, int((x - 12) * scale));
                }
                // Composer border spans the message column; do not OCR drafts as messages.
                double horizontalBest = 0;
                for (int y = height / 2; y < height - int(70 * scale); ++y)
                {
                    int edges = 0;
                    for (int x = left + int(15 * scale); x < right - int(15 * scale); x += 3)
                        if (std::abs(luminance(x, y - 2) - luminance(x, y + 2)) >= 4)
                            ++edges;
                    if (edges > horizontalBest && edges * 3 > (right - left) * 0.65)
                    {
                        horizontalBest = edges;
                        bottom = y - int(5 * scale);
                    }
                }
                const bool dark = luminance(left + int(8 * scale), top + int(4 * scale)) < 128;
                if (!qq && dark)
                    throw std::runtime_error("unsupported WeCom theme");
                auto color = [&](int x, int y) {
                    size_t a = (size_t(y) * width + x) * 4;
                    int b = src[a], g = src[a + 1], r = src[a + 2];
                    if (r >= 175 && r <= 220 && g >= 210 && b >= 235 && b - r >= 22)
                        return 2;
                    if (qq && b > 130 && b - r > 65 && g > 70)
                        return 2;
                    if (dark && r >= 29 && r <= 64 && std::abs(r - g) < 5 && std::abs(g - b) < 5)
                        return 1;
                    if (!dark && r >= 217 && r <= 241 && g >= r - 3 && g <= r + 8 && b >= g && b <= g + 12)
                        return 1;
                    return 0;
                };
                result["left"] = divider / scale;
                result["right"] = right / scale;
                result["bottom"] = bottom / scale;
                result["dark"] = dark;
                std::vector<uint8_t> seen(size_t(width) * height, 0);
                for (int y = top; y < bottom; ++y)
                    for (int x = left; x < right; ++x)
                    {
                        size_t pos = size_t(y) * width + x;
                        int kind = color(x, y);
                        if (seen[pos] || !kind)
                            continue;
                        std::vector<int> queue{int(pos)};
                        seen[pos] = 1;
                        int x1 = x, x2 = x, y1 = y, y2 = y;
                        for (size_t q = 0; q < queue.size(); ++q)
                        {
                            int px = queue[q] % width, py = queue[q] / width;
                            x1 = std::min(x1, px);
                            x2 = std::max(x2, px);
                            y1 = std::min(y1, py);
                            y2 = std::max(y2, py);
                            for (auto d : {std::pair<int, int>{-1, 0}, {1, 0}, {0, -1}, {0, 1}})
                            {
                                int nx = px + d.first, ny = py + d.second;
                                if (nx < left || nx >= right || ny < top || ny >= bottom)
                                    continue;
                                size_t np = size_t(ny) * width + nx;
                                if (!seen[np] && color(nx, ny) == kind)
                                {
                                    seen[np] = 1;
                                    queue.push_back(int(np));
                                }
                            }
                        }
                        if (x2 - x1 >= 24 * scale && y2 - y1 >= 24 * scale && x2 - x1 < 650 * scale &&
                            y2 - y1 < 400 * scale && queue.size() > 300 * scale * scale)
                            result["bubbles"].push_back({{"x", x1 / scale},
                                                         {"y", y1 / scale},
                                                         {"width", (x2 - x1 + 1) / scale},
                                                         {"height", (y2 - y1 + 1) / scale},
                                                         {"self", kind == 2}});
                    }
                result["width"] = width / scale;
                result["height"] = height / scale;
                result["verifiedLayout"] = true;
            }
        }
        catch (...)
        {
            result["verifiedLayout"] = false;
        }
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    return result;
}
std::string Utf8(const wchar_t *s, int n)
{
    if (!s || n <= 0)
        return {};
    auto length = WideCharToMultiByte(CP_UTF8, 0, s, n, nullptr, 0, nullptr, nullptr);
    std::string out(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, n, out.data(), length, nullptr, nullptr);
    return out;
}
std::string Property(IUIAutomationElement *e, PROPERTYID id)
{
    VARIANT v{};
    if (FAILED(e->GetCachedPropertyValue(id, &v)))
        return {};
    std::string result;
    if (v.vt == VT_BSTR)
        result = Utf8(v.bstrVal, SysStringLen(v.bstrVal));
    VariantClear(&v);
    if (result.size() > 16000)
        result.clear();
    return result;
}
std::string Platform(HWND hwnd, DWORD &pid, std::string &instance)
{
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t path[32768]{};
    DWORD size = 32768;
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(process, &created, &exited, &kernel, &user);
    bool ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok)
        return {};
    auto name = std::filesystem::path(path).filename().wstring();
    for (auto &c : name)
        c = towlower(c);
    instance =
        std::to_string(pid) + "-" + std::to_string((uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime);
    if (name == L"weixin.exe" || name == L"wechat.exe")
        return "wechat";
    if (name == L"qq.exe")
        return "qq";
    if (name == L"wxwork.exe" || name == L"wecom.exe")
        return "wecom";
    if (name == L"dingtalk.exe")
        return "dingtalk";
    if (name == L"feishu.exe" || name == L"lark.exe")
        return "feishu";
    return {};
}
struct Probe
{
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationTreeWalker> walker;
    ComPtr<IUIAutomationCacheRequest> cache;
    ULONGLONG deadline = GetTickCount64() + 6500;
    json windows = json::array();
    bool truncated = false;
    void Walk(IUIAutomationElement *element, int parent, int depth, json &nodes)
    {
        if (!element || depth > 48 || nodes.size() >= 3500 || GetTickCount64() > deadline)
        {
            truncated = true;
            return;
        }
        int index = static_cast<int>(nodes.size());
        CONTROLTYPEID type = 0;
        element->get_CachedControlType(&type);
        BOOL offscreen = FALSE;
        element->get_CachedIsOffscreen(&offscreen);
        RECT r{};
        element->get_CachedBoundingRectangle(&r);
        SAFEARRAY *ids = nullptr;
        json runtime = json::array();
        VARIANT rid{};
        element->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &rid);
        if (rid.vt == (VT_ARRAY | VT_I4))
            ids = rid.parray;
        if (ids)
        {
            LONG first = 0, last = -1;
            SafeArrayGetLBound(ids, 1, &first);
            SafeArrayGetUBound(ids, 1, &last);
            for (LONG i = first; i <= last && i < first + 32; ++i)
            {
                int value = 0;
                SafeArrayGetElement(ids, &i, &value);
                runtime.push_back(value);
            }
        }
        VariantClear(&rid);
        nodes.push_back({{"parent", parent},
                         {"type", type},
                         {"name", Property(element, UIA_NamePropertyId)},
                         {"id", Property(element, UIA_AutomationIdPropertyId)},
                         {"class", Property(element, UIA_ClassNamePropertyId)},
                         {"help", Property(element, UIA_HelpTextPropertyId)},
                         {"description", Property(element, UIA_LegacyIAccessibleDescriptionPropertyId)},
                         {"value", Property(element, UIA_LegacyIAccessibleValuePropertyId)},
                         {"runtime", runtime.dump()},
                         {"offscreen", bool(offscreen)},
                         {"x", r.left},
                         {"y", r.top},
                         {"width", r.right - r.left},
                         {"height", r.bottom - r.top}});
        ComPtr<IUIAutomationElementArray> children;
        if (SUCCEEDED(element->GetCachedChildren(&children)) && children)
        {
            int count = 0;
            children->get_Length(&count);
            for (int i = 0; i < count; ++i)
            {
                if (GetTickCount64() > deadline || nodes.size() >= 3500)
                {
                    truncated = true;
                    break;
                }
                ComPtr<IUIAutomationElement> child;
                children->GetElement(i, &child);
                Walk(child.Get(), index, depth + 1, nodes);
            }
        }
    }
    void Window(HWND hwnd)
    {
        if (!IsWindowVisible(hwnd) || windows.size() >= 16)
            return;
        DWORD pid = 0;
        std::string instance;
        auto platform = Platform(hwnd, pid, instance);
        if (platform.empty())
            return;
        wchar_t title[512]{};
        GetWindowTextW(hwnd, title, 512);
        json row = {{"platform", platform},
                    {"window", instance + "-" + std::to_string(reinterpret_cast<uintptr_t>(hwnd))},
                    {"dpi", GetDpiForWindow(hwnd)},
                    {"title", Utf8(title, lstrlenW(title))},
                    {"nodes", json::array()},
                    {"complete", false}};
        if (!IsIconic(hwnd) && GetTickCount64() <= deadline)
        {
            truncated = false;
            ComPtr<IUIAutomationElement> root;
            if (SUCCEEDED(automation->ElementFromHandleBuildCache(hwnd, cache.Get(), &root)) && root)
            {
                Walk(root.Get(), -1, 0, row["nodes"]);
                row["complete"] = !truncated;
                if (platform == "wechat" && !truncated && GetTickCount64() < deadline)
                    row["labels"] = ReadLabels(hwnd, row["nodes"]);
                const bool qqNative =
                    platform == "qq" && std::any_of(row["nodes"].begin(), row["nodes"].end(),
                                                    [](const json &n) { return n.value("id", "") == "ml-root"; });
                if ((platform == "wecom" || (platform == "qq" && !qqNative)) && !truncated &&
                    (row["title"] == "企业微信" || platform == "qq") && GetTickCount64() < deadline)
                    row["ocr"] = ReadWeCom(hwnd, platform == "qq");
                // Re-read the active chat title to reject a mid-capture conversation switch.
                for (auto &node : row["nodes"])
                {
                    auto id = node.value("id", "");
                    const bool qqTitle = platform == "qq" && node.value("class", "") == "chat-header__contact-name";
                    if (!qqTitle && id.find("current_chat_name_label") == std::string::npos &&
                        !(platform == "dingtalk" && id == "title_label"))
                        continue;
                    VARIANT v{};
                    v.vt = VT_BSTR;
                    const auto selector = qqTitle ? std::string("chat-header__contact-name") : id;
                    std::wstring wide(selector.begin(), selector.end());
                    v.bstrVal = SysAllocString(wide.c_str());
                    ComPtr<IUIAutomationCondition> condition;
                    automation->CreatePropertyCondition(qqTitle ? UIA_ClassNamePropertyId : UIA_AutomationIdPropertyId,
                                                        v, &condition);
                    VariantClear(&v);
                    ComPtr<IUIAutomationElement> current;
                    if (!condition || FAILED(root->FindFirst(TreeScope_Descendants, condition.Get(), &current)) ||
                        !current)
                        row["complete"] = false;
                    else
                    {
                        BSTR name = nullptr;
                        current->get_CurrentName(&name);
                        if (node["name"] != Utf8(name, SysStringLen(name)))
                            row["complete"] = false;
                        SysFreeString(name);
                    }
                }
            }
        }
        windows.push_back(std::move(row));
    }
};
} // namespace
int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "--self-test-pipe")
    {
        json nodes = json::array();
        nodes.push_back({{"parent", -1}, {"type", 50020}, {"name", "测试群"}, {"id", "chat-title"}});
        nodes.push_back({{"parent", -1}, {"type", 50008}, {"name", "消息"}, {"id", "message-list"}});
        nodes.push_back({{"parent", 1},
                         {"type", 50007},
                         {"name", std::string(11000, 'x')},
                         {"id", "message-item"},
                         {"class", "message-item"}});
        for (int i = 0; i < 1000; ++i)
            nodes.push_back({{"parent", -1}, {"type", 50020}, {"name", std::string(400, 'y')}});
        std::cout << json{{"windows",
                           json::array({json{
                               {"window", "fixture"}, {"platform", "wechat"}, {"complete", true}, {"nodes", nodes}}})}}
                         .dump();
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 2;
    int code = 0;
    {
        Probe p;
        ComPtr<IUIAutomation2> automation2;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation2))))
            code = 3;
        else
        {
            automation2->put_ConnectionTimeout(500);
            automation2->put_TransactionTimeout(1500);
            automation2.As(&p.automation);
            p.automation->get_RawViewWalker(&p.walker);
            p.automation->CreateCacheRequest(&p.cache);
            p.cache->put_TreeScope(TreeScope_Subtree);
            ComPtr<IUIAutomationCondition> raw;
            p.automation->get_RawViewCondition(&raw);
            p.cache->put_TreeFilter(raw.Get());
            for (auto id : {UIA_RuntimeIdPropertyId, UIA_NamePropertyId, UIA_AutomationIdPropertyId,
                            UIA_ClassNamePropertyId, UIA_LegacyIAccessibleDescriptionPropertyId,
                            UIA_LegacyIAccessibleValuePropertyId, UIA_HelpTextPropertyId, UIA_ControlTypePropertyId,
                            UIA_IsOffscreenPropertyId, UIA_BoundingRectanglePropertyId})
                p.cache->AddProperty(id);
            if (!p.walker || !p.cache)
                code = 4;
            else
            {
                EnumWindows(
                    [](HWND w, LPARAM param) -> BOOL {
                        static_cast<Probe *>(reinterpret_cast<void *>(param))->Window(w);
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&p));
                if (argc > 1 && std::string(argv[1]) == "--diagnose")
                {
                    json report = json::array();
                    for (const auto &rawWindow : p.windows)
                    {
                        auto parsed = ReplyStudio::ParseChatWindow(rawWindow);
                        size_t named = 0, self = 0;
                        for (const auto &m : parsed["messages"])
                        {
                            named += !m.value("speaker", "").empty();
                            self += m.value("self", false);
                        }
                        report.push_back({{"platform", parsed["platform"]},
                                          {"readable", parsed["readable"]},
                                          {"kind", parsed["kind"]},
                                          {"messages", parsed["messages"].size()},
                                          {"attributed", named},
                                          {"self", self},
                                          {"status", parsed["status"]}});
                    }
                    std::cout << report.dump();
                }
                else
                    std::cout << json{{"windows", p.windows}}.dump();
            }
        }
    }
    CoUninitialize();
    return code;
}
