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
    std::wstring text_;
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
            HBRUSH brush = CreateSolidBrush(RGB(25, 28, 33));
            FillRect(dc, &r, brush);
            DeleteObject(brush);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(245, 245, 245));
            const int dpi = GetDpiForWindow(h);
            HFONT font = CreateFontW(-MulDiv(16, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                     L"Microsoft YaHei");
            auto old = SelectObject(dc, font);
            InflateRect(&r, -MulDiv(8, dpi, 96), -MulDiv(8, dpi, 96));
            {
                std::lock_guard lock(p->mutex_);
                DrawTextW(dc, p->text_.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX);
            }
            SelectObject(dc, old);
            DeleteObject(font);
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
            SetLayeredWindowAttributes(h, 0, 230, LWA_ALPHA);
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
    void Set(RECT bounds, std::wstring text, bool show)
    {
        {
            std::lock_guard lock(mutex_);
            bounds_ = bounds;
            text_ = std::move(text);
            shown_ = show;
        }
        if (auto h = window_.load())
            PostMessageW(h, WM_APP + 1, 0, 0);
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
    if (w <= 0 || h <= 0 || int64_t(w) * h > 16000000)
        throw std::runtime_error("截图区域超过 1600 万像素");
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
    using namespace winrt::Windows;
    Storage::Streams::InMemoryRandomAccessStream stream;
    Storage::Streams::DataWriter writer(stream);
    writer.WriteBytes(winrt::array_view<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()),
                                                       reinterpret_cast<const uint8_t *>(bytes.data() + bytes.size())));
    writer.StoreAsync().get();
    writer.DetachStream();
    stream.Seek(0);
    auto decoder = Graphics::Imaging::BitmapDecoder::CreateAsync(stream).get();
    if (uint64_t(decoder.PixelWidth()) * decoder.PixelHeight() > 32000000)
        throw std::runtime_error("图像超过 3200 万像素，请裁剪后导入");
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
    if (stat.cbSize.QuadPart > 25 * 1024 * 1024)
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
nlohmann::json WindowsOcr(const Image &image)
{
    using namespace winrt::Windows;
    auto engine = Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine)
        throw std::runtime_error("本机未安装 OCR 语言包，请选择本地模型或云 OCR");
    if (image.width > Media::Ocr::OcrEngine::MaxImageDimension() ||
        image.height > Media::Ocr::OcrEngine::MaxImageDimension())
        throw std::runtime_error("Windows OCR 图像尺寸过大，请分区截图或选择文档 OCR 模型");
    auto buffer = Security::Cryptography::CryptographicBuffer::CreateFromByteArray(image.pixels);
    auto bitmap = Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
        buffer, Graphics::Imaging::BitmapPixelFormat::Bgra8, image.width, image.height,
        Graphics::Imaging::BitmapAlphaMode::Ignore);
    auto result = engine.RecognizeAsync(bitmap).get();
    nlohmann::json out = {
        {"text", winrt::to_string(result.Text())}, {"blocks", nlohmann::json::array()}, {"format", "text"}};
    std::string text;
    for (const auto &line : result.Lines())
    {
        std::string value = wstring_to_string(NormalizeChineseOcrSpacing(std::wstring(line.Text())));
        if (!text.empty())
            text += '\n';
        text += value;
        out["blocks"].push_back({{"text", value}});
    }
    out["text"] = text;
    out["raw"] = {{"text", winrt::to_string(result.Text())}};
    return out;
}
void ShowTranslationOverlay(RECT rectangle, const std::string &text)
{
    OverlayWindow().Set(rectangle, string_to_wstring(text), true);
}
void HideTranslationOverlay()
{
    OverlayWindow().Set({}, L"", false);
}
} // namespace Vision
