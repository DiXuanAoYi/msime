#include "settings/vision_capture.h"
#include "utils/common_utils.h"
#include <Windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "windowsapp.lib")
namespace Vision
{
namespace
{
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("图像处理失败，请换用 PNG 或 JPEG");
}
struct Selection
{
    POINT start{}, end{};
    RECT desktop{};
    bool dragging = false, done = false, cancelled = false;
};
LRESULT CALLBACK PickProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    auto *p = reinterpret_cast<Selection *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        p = static_cast<Selection *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
    }
    if (!p)
        return DefWindowProcW(hwnd, message, w, l);
    switch (message)
    {
    case WM_LBUTTONDOWN:
        p->start = p->end = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        p->dragging = true;
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (p->dragging)
        {
            p->end = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        p->end = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        p->dragging = false;
        p->done = true;
        ReleaseCapture();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
        {
            p->done = true;
            p->cancelled = true;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, L"拖动选择区域 · Esc 取消", -1, &client, DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (p->dragging)
        {
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(80, 230, 170));
            auto old = SelectObject(dc, pen);
            auto brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, p->start.x, p->start.y, p->end.x, p->end.y);
            SelectObject(dc, brush);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        p->done = p->cancelled = true;
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}
class Overlay
{
    std::thread thread_;
    std::atomic<HWND> window_{nullptr};
    std::mutex mutex_;
    RECT bounds_{};
    std::vector<OverlayBlock> blocks_;
    bool shown_ = false;
    std::atomic<bool> quitting_{false};
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        auto *p = reinterpret_cast<Overlay *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE)
        {
            p = static_cast<Overlay *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
        }
        if (!p)
            return DefWindowProcW(h, m, w, l);
        if (m == WM_APP + 1)
        {
            std::lock_guard lock(p->mutex_);
            if (p->shown_)
            {
                const auto &r = p->bounds_;
                SetWindowPos(h, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
                InvalidateRect(h, nullptr, TRUE);
            }
            else
                ShowWindow(h, SW_HIDE);
            return 0;
        }
        if (m == WM_PAINT)
        {
            PAINTSTRUCT ps;
            auto dc = BeginPaint(h, &ps);
            RECT r{};
            GetClientRect(h, &r);
            // Black is the color key. Only recognised text boxes obscure the source screen.
            HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(dc, &r, brush);
            DeleteObject(brush);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(245, 245, 245));
            const int dpi = GetDpiForWindow(h);
            {
                std::lock_guard lock(p->mutex_);
                for (const auto &block : p->blocks_)
                {
                    RECT box = block.bounds;
                    RECT clipped{};
                    if (block.text.empty() || !IntersectRect(&clipped, &box, &r))
                        continue;
                    auto background = CreateSolidBrush(RGB(25, 28, 33));
                    FillRect(dc, &clipped, background);
                    DeleteObject(background);
                    const auto text = string_to_wstring(OverlayDisplayText(block.text, block.format));
                    const int minimum = MulDiv(9, dpi, 96);
                    int size = std::clamp(int((clipped.bottom - clipped.top) * .75), minimum, MulDiv(20, dpi, 96));
                    HFONT font = nullptr;
                    HGDIOBJ old = nullptr;
                    for (;;)
                    {
                        font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                           L"Microsoft YaHei");
                        old = SelectObject(dc, font);
                        RECT measured = clipped;
                        DrawTextW(dc, text.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                        if (measured.bottom - measured.top <= clipped.bottom - clipped.top || size <= minimum)
                            break;
                        SelectObject(dc, old);
                        DeleteObject(font);
                        --size;
                    }
                    const int saved = SaveDC(dc);
                    IntersectClipRect(dc, clipped.left, clipped.top, clipped.right, clipped.bottom);
                    DrawTextW(dc, text.c_str(), -1, &clipped, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
                    RestoreDC(dc, saved);
                    SelectObject(dc, old);
                    DeleteObject(font);
                }
            }
            EndPaint(h, &ps);
            return 0;
        }
        if (m == WM_CLOSE)
        {
            DestroyWindow(h);
            return 0;
        }
        if (m == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

  public:
    Overlay()
    {
        thread_ = std::thread([this] {
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            WNDCLASSW c{};
            c.lpfnWndProc = Proc;
            c.hInstance = GetModuleHandleW(nullptr);
            c.lpszClassName = L"MetasequoiaVisionTranslation";
            RegisterClassW(&c);
            auto h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                     c.lpszClassName, L"屏幕翻译", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, c.hInstance,
                                     this);
            if (!h)
                return;
            SetLayeredWindowAttributes(h, RGB(0, 0, 0), 240, LWA_COLORKEY | LWA_ALPHA);
            SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
            window_ = h;
            PostMessageW(h, WM_APP + 1, 0, 0);
            MSG msg{};
            while (!quitting_ && GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (IsWindow(h))
                DestroyWindow(h);
            window_ = nullptr;
        });
    }
    ~Overlay()
    {
        quitting_ = true;
        if (auto h = window_.load())
            PostMessageW(h, WM_CLOSE, 0, 0);
        if (thread_.joinable())
            thread_.join();
    }
    void Set(RECT bounds, std::vector<OverlayBlock> blocks, bool show)
    {
        {
            std::lock_guard lock(mutex_);
            bounds_ = bounds;
            blocks_ = std::move(blocks);
            shown_ = show;
        }
        if (auto h = window_.load())
        {
            if (show)
                PostMessageW(h, WM_APP + 1, 0, 0);
            else
            {
                DWORD_PTR result = 0;
                // Capture waits for the overlay thread to hide it; no self-OCR feedback loop.
                SendMessageTimeoutW(h, WM_APP + 1, 0, 0, SMTO_ABORTIFHUNG, 500, &result);
            }
        }
    }
};
Overlay &OverlayWindow()
{
    static Overlay overlay;
    return overlay;
}
} // namespace
RECT PickRectangle(HWND owner)
{
    Selection s;
    s.desktop = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    s.desktop.right = s.desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    s.desktop.bottom = s.desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    WNDCLASSW c{};
    c.lpfnWndProc = PickProc;
    c.hInstance = GetModuleHandleW(nullptr);
    c.lpszClassName = L"MetasequoiaCaptureSelection";
    c.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassW(&c);
    ShowWindow(owner, SW_HIDE);
    auto h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, c.lpszClassName, L"选择截图区域", WS_POPUP,
                             s.desktop.left, s.desktop.top, s.desktop.right - s.desktop.left,
                             s.desktop.bottom - s.desktop.top, nullptr, nullptr, c.hInstance, &s);
    if (!h)
    {
        ShowWindow(owner, SW_SHOW);
        throw std::runtime_error("无法选择截图区域");
    }
    SetLayeredWindowAttributes(h, 0, 95, LWA_ALPHA);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
    SetFocus(h);
    MSG message{};
    while (!s.done)
    {
        const auto read = GetMessageW(&message, nullptr, 0, 0);
        if (read <= 0)
        {
            if (read == 0)
                PostQuitMessage(static_cast<int>(message.wParam));
            s.cancelled = true;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(h);
    if (s.cancelled)
    {
        ShowWindow(owner, SW_SHOW);
        return {};
    }
    RECT out{std::min(s.start.x, s.end.x) + s.desktop.left, std::min(s.start.y, s.end.y) + s.desktop.top,
             std::max(s.start.x, s.end.x) + s.desktop.left, std::max(s.start.y, s.end.y) + s.desktop.top};
    if (out.right - out.left < 12 || out.bottom - out.top < 12)
    {
        ShowWindow(owner, SW_SHOW);
        throw std::runtime_error("截图区域太小");
    }
    return out;
}
Image CaptureRectangle(RECT r)
{
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || uint64_t(w) * h > MaxImagePixels)
        throw std::runtime_error("截图区域超过 6400 万像素，请分区截图");
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *p = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &p, nullptr, 0);
    if (!bitmap)
    {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        throw std::runtime_error("截图内存不足");
    }
    auto old = SelectObject(memory, bitmap);
    const bool ok = BitBlt(memory, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
    Image out;
    out.width = w;
    out.height = h;
    if (ok)
    {
        out.pixels.assign(static_cast<unsigned char *>(p), static_cast<unsigned char *>(p) + size_t(w) * h * 4);
        for (size_t i = 3; i < out.pixels.size(); i += 4)
            out.pixels[i] = 255;
    }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!ok)
        throw std::runtime_error("无法读取截图区域");
    return out;
}
Image DecodeImage(const std::string &bytes)
{
    if (bytes.empty() || bytes.size() > MaxEncodedImageBytes)
        throw std::runtime_error("图片文件为空或超过 25 MB，请缩小后导入");
    using namespace winrt::Windows;
    Storage::Streams::InMemoryRandomAccessStream stream;
    Storage::Streams::DataWriter writer(stream);
    writer.WriteBytes(winrt::array_view<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()),
                                                       reinterpret_cast<const uint8_t *>(bytes.data() + bytes.size())));
    writer.StoreAsync().get();
    writer.DetachStream();
    stream.Seek(0);
    auto decoder = Graphics::Imaging::BitmapDecoder::CreateAsync(stream).get();
    if (uint64_t(decoder.PixelWidth()) * decoder.PixelHeight() > MaxImagePixels)
        throw std::runtime_error("图像超过 6400 万像素，请裁剪后导入");
    auto bitmap =
        decoder
            .GetSoftwareBitmapAsync(Graphics::Imaging::BitmapPixelFormat::Bgra8,
                                    Graphics::Imaging::BitmapAlphaMode::Ignore, Graphics::Imaging::BitmapTransform(),
                                    Graphics::Imaging::ExifOrientationMode::RespectExifOrientation,
                                    Graphics::Imaging::ColorManagementMode::DoNotColorManage)
            .get();
    Image out;
    out.width = bitmap.PixelWidth();
    out.height = bitmap.PixelHeight();
    Storage::Streams::Buffer buffer(out.width * out.height * 4);
    bitmap.CopyToBuffer(buffer);
    winrt::com_array<uint8_t> pixels;
    Security::Cryptography::CryptographicBuffer::CopyToByteArray(buffer, pixels);
    out.pixels.assign(pixels.begin(), pixels.end());
    return out;
}
std::string EncodePng(const Image &image)
{
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr));
    Check(frame->Initialize(nullptr));
    Check(frame->SetSize(image.width, image.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(frame->SetPixelFormat(&format));
    Check(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(image.pixels.size()),
                             const_cast<BYTE *>(image.pixels.data())));
    Check(frame->Commit());
    Check(encoder->Commit());
    HGLOBAL memory;
    Check(GetHGlobalFromStream(stream.Get(), &memory));
    STATSTG stat{};
    Check(stream->Stat(&stat, STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > MaxEncodedImageBytes)
        throw std::runtime_error("PNG 图像过大，请缩小区域");
    void *p = GlobalLock(memory);
    if (!p)
        throw std::runtime_error("读取 PNG 失败");
    std::string bytes(static_cast<const char *>(p), static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(memory);
    return bytes;
}
std::wstring NormalizeChineseOcrSpacing(const std::wstring &text)
{
    const auto chinese = [](wchar_t c) {
        return (c >= 0x3400 && c <= 0x9fff) ||
               std::wstring_view(L"，。！？；：、（）“”《》").find(c) != std::wstring_view::npos;
    };
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == L' ' && !out.empty() && chinese(out.back()))
        {
            size_t next = i;
            while (next < text.size() && text[next] == L' ')
                ++next;
            if (next < text.size() && chinese(text[next]))
            {
                i = next - 1;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}
std::string OverlayDisplayText(const std::string &text, const std::string &format)
{
    if (format != "html")
        return text;
    // A plain Win32 overlay never executes or renders HTML. Keep the original HTML in the editor/export.
    std::string readable;
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == '<')
        {
            const auto end = text.find('>', i + 1);
            if (end == std::string::npos)
            {
                readable.append(text, i, std::string::npos);
                break;
            }
            auto tag = text.substr(i + 1, end - i - 1);
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return std::tolower(c); });
            const auto space = tag.find_first_of(" \t\r\n");
            if (space != std::string::npos)
                tag.resize(space);
            if (tag == "/tr" || tag == "br" || tag == "br/" || tag == "/p")
            {
                while (!readable.empty() && readable.back() == '\t')
                    readable.pop_back();
                if (!readable.empty() && readable.back() != '\n')
                    readable += '\n';
            }
            else if (tag == "/td" || tag == "/th")
                readable += '\t';
            i = end + 1;
        }
        else
            readable += text[i++];
    }
    std::string decoded;
    for (size_t i = 0; i < readable.size();)
    {
        if (readable[i] == '&')
        {
            bool found = false;
            for (const auto &[entity, replacement] :
                 std::initializer_list<std::pair<std::string_view, std::string_view>>{{"&lt;", "<"},
                                                                                      {"&gt;", ">"},
                                                                                      {"&amp;", "&"},
                                                                                      {"&quot;", "\""},
                                                                                      {"&apos;", "'"},
                                                                                      {"&#39;", "'"},
                                                                                      {"&nbsp;", " "}})
                if (readable.compare(i, entity.size(), entity) == 0)
                {
                    decoded += replacement;
                    i += entity.size();
                    found = true;
                    break;
                }
            if (found)
                continue;
        }
        decoded += readable[i++];
    }
    while (!decoded.empty() && (decoded.back() == '\n' || decoded.back() == '\t'))
        decoded.pop_back();
    return decoded;
}
std::vector<RECT> ImageTileRegions(unsigned width, unsigned height, unsigned tileSize, unsigned overlap)
{
    if (!width || !height || uint64_t(width) * height > MaxImagePixels || tileSize <= overlap || tileSize < 128)
        throw std::runtime_error("图像或 OCR 分块参数无效");
    std::vector<RECT> tiles;
    const unsigned stride = tileSize - overlap;
    for (unsigned y = 0; y < height; y += stride)
    {
        for (unsigned x = 0; x < width; x += stride)
        {
            if (tiles.size() >= 128)
                throw std::runtime_error("长图需要过多 OCR 分块，请分段识别");
            tiles.push_back(
                {LONG(x), LONG(y), LONG(std::min(width, x + tileSize)), LONG(std::min(height, y + tileSize))});
            if (x + tileSize >= width)
                break;
        }
        if (y + tileSize >= height)
            break;
    }
    return tiles;
}
std::vector<OverlayBlock> OverlayBlocksForImage(const nlohmann::json &result, unsigned width, unsigned height)
{
    if (!width || !height || uint64_t(width) * height > MaxImagePixels)
        throw std::runtime_error("屏幕翻译图像尺寸无效");
    std::vector<OverlayBlock> blocks;
    if (result.contains("blocks") && result.at("blocks").is_array())
        for (const auto &block : result.at("blocks"))
        {
            if (blocks.size() >= 256)
                break;
            if (!block.is_object() || !block.contains("box") || !block.at("box").is_array())
                continue;
            double left = width, top = height, right = 0, bottom = 0;
            bool valid = true;
            const auto &points = block.at("box");
            if (points.size() < 4)
                continue;
            for (const auto &point : points)
            {
                double x = 0, y = 0;
                if (point.is_array() && point.size() == 2 && point[0].is_number() && point[1].is_number())
                {
                    x = point[0].get<double>();
                    y = point[1].get<double>();
                }
                else if (point.is_object() && point.contains("x") && point.contains("y") && point["x"].is_number() &&
                         point["y"].is_number())
                {
                    x = point["x"].get<double>();
                    y = point["y"].get<double>();
                }
                else
                {
                    valid = false;
                    break;
                }
                if (!std::isfinite(x) || !std::isfinite(y))
                {
                    valid = false;
                    break;
                }
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
            if (!valid || right <= left || bottom <= top || right <= 0 || bottom <= 0 || left >= width || top >= height)
                continue;
            // Reconstructed indentation belongs in the editor; the original box already has its screen x offset.
            const auto source = block.value("ocr_text", block.value("text", std::string()));
            if (source.empty())
                continue;
            OverlayBlock out;
            out.bounds = {LONG(std::floor(std::clamp(left, 0.0, double(width)))),
                          LONG(std::floor(std::clamp(top, 0.0, double(height)))),
                          LONG(std::ceil(std::clamp(right, 0.0, double(width)))),
                          LONG(std::ceil(std::clamp(bottom, 0.0, double(height))))};
            out.source = source;
            out.region = block.value("region", false);
            out.format = block.value("format", result.value("format", "text"));
            blocks.push_back(std::move(out));
        }
    if (blocks.empty())
    {
        auto text = result.value("text", std::string());
        if (!text.empty())
            blocks.push_back({{0, 0, LONG(width), LONG(height)}, text, "", true, result.value("format", "text")});
    }
    return blocks;
}
nlohmann::json WindowsOcr(const Image &image, const std::function<bool()> &cancelled)
{
    using namespace winrt::Windows;
    auto engine = Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine)
        throw std::runtime_error("本机未安装 OCR 语言包，请选择本地模型或云 OCR");
    if (image.pixels.size() != uint64_t(image.width) * image.height * 4)
        throw std::runtime_error("OCR 图像像素数据无效");
    const unsigned size = std::min(2048u, Media::Ocr::OcrEngine::MaxImageDimension());
    const auto tiles = ImageTileRegions(image.width, image.height, size, 96);
    nlohmann::json out = {{"text", ""},
                          {"blocks", nlohmann::json::array()},
                          {"format", "text"},
                          {"width", image.width},
                          {"height", image.height}};
    auto raw = nlohmann::json::array();
    for (const auto &tile : tiles)
    {
        if (cancelled && cancelled())
            return {};
        const unsigned w = tile.right - tile.left, h = tile.bottom - tile.top;
        std::vector<uint8_t> pixels(size_t(w) * h * 4);
        for (unsigned row = 0; row < h; ++row)
            std::memcpy(pixels.data() + size_t(row) * w * 4,
                        image.pixels.data() + (size_t(tile.top + row) * image.width + tile.left) * 4, size_t(w) * 4);
        auto buffer = Security::Cryptography::CryptographicBuffer::CreateFromByteArray(pixels);
        auto bitmap = Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
            buffer, Graphics::Imaging::BitmapPixelFormat::Bgra8, w, h, Graphics::Imaging::BitmapAlphaMode::Ignore);
        auto result = engine.RecognizeAsync(bitmap).get();
        raw.push_back({{"text", winrt::to_string(result.Text())}, {"x", tile.left}, {"y", tile.top}});
        for (const auto &line : result.Lines())
        {
            double left = w, top = h, right = 0, bottom = 0;
            for (const auto &word : line.Words())
            {
                const auto r = word.BoundingRect();
                left = std::min(left, double(r.X));
                top = std::min(top, double(r.Y));
                right = std::max(right, double(r.X + r.Width));
                bottom = std::max(bottom, double(r.Y + r.Height));
            }
            if (right <= left || bottom <= top)
                continue;
            const double cx = (left + right) / 2, cy = (top + bottom) / 2;
            if ((tile.left && cx < 48) || (tile.top && cy < 48) || (tile.right < LONG(image.width) && cx >= w - 48) ||
                (tile.bottom < LONG(image.height) && cy >= h - 48))
                continue;
            left += tile.left;
            right += tile.left;
            top += tile.top;
            bottom += tile.top;
            const auto value = wstring_to_string(NormalizeChineseOcrSpacing(std::wstring(line.Text())));
            out["blocks"].push_back({{"text", value},
                                     {"kind", "text"},
                                     {"format", "text"},
                                     {"box", {{left, top}, {right, top}, {right, bottom}, {left, bottom}}}});
        }
    }
    auto &blocks = out["blocks"];
    std::stable_sort(blocks.begin(), blocks.end(), [](const auto &a, const auto &b) {
        const auto ay = a.at("box")[0][1].template get<double>(), by = b.at("box")[0][1].template get<double>();
        return ay != by ? ay < by : a.at("box")[0][0].template get<double>() < b.at("box")[0][0].template get<double>();
    });
    std::string text;
    for (const auto &block : blocks)
    {
        if (!text.empty())
            text += '\n';
        text += block.at("text").get<std::string>();
    }
    out["text"] = text;
    out["raw"] = {{"tiles", raw}};
    return out;
}
void ShowTranslationOverlay(RECT rectangle, const std::vector<OverlayBlock> &blocks)
{
    OverlayWindow().Set(rectangle, blocks, !blocks.empty());
}
void HideTranslationOverlay()
{
    OverlayWindow().Set({}, {}, false);
}
} // namespace Vision
