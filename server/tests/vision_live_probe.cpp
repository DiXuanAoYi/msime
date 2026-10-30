// Explicit renderer integration probe. Never registered as an unattended GUI test.
// Creates only its own synthetic window, reads only its client rectangle, and does
// not inject input, open microphones/cameras, download models, or call providers.
#include "settings/vision_capture.h"
#include "utils/common_utils.h"
#include <Windows.h>
#include <dwmapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <thread>

namespace
{
using nlohmann::json;
using Vision::Image;
struct Dib
{
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void *pixels = nullptr;
    unsigned width, height;
    Dib(unsigned w, unsigned h) : width(w), height(h)
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -LONG(h);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc || !bitmap || !pixels)
        {
            if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            throw std::runtime_error("Cannot create the synthetic test bitmap");
        }
        previous = SelectObject(dc, bitmap);
        std::memset(pixels, 255, size_t(w) * h * 4);
    }
    ~Dib()
    {
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    Image ImageBytes() const
    {
        Image image{width, height, {}};
        const auto p = static_cast<const unsigned char *>(pixels);
        image.pixels.assign(p, p + size_t(width) * height * 4);
        for (size_t i = 3; i < image.pixels.size(); i += 4)
            image.pixels[i] = 255;
        return image;
    }
};
Image Fixture(unsigned width, unsigned height, unsigned count, bool wide = false)
{
    Dib dib(width, height);
    auto font = CreateFontW(-28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto old = SelectObject(dib.dc, font);
    SetTextColor(dib.dc, RGB(0, 0, 0));
    SetBkMode(dib.dc, TRANSPARENT);
    for (unsigned i = 0; i < count; ++i)
    {
        wchar_t text[100]{};
        swprintf_s(text, L"ROW %03u local OCR marker number %03u", i + 1, i + 1);
        TextOutW(dib.dc, 48, 48 + i * 60, text, int(wcslen(text)));
    }
    if (wide)
        TextOutW(dib.dc, 2160, 240, L"RIGHT EDGE 2026", 15);
    SelectObject(dib.dc, old);
    DeleteObject(font);
    return dib.ImageBytes();
}
Image BoxFixture(const Image &source, const json &result)
{
    Dib dib(source.width, source.height);
    std::memcpy(dib.pixels, source.pixels.data(), source.pixels.size());
    auto pen = CreatePen(PS_SOLID, 2, RGB(225, 40, 50));
    auto oldPen = SelectObject(dib.dc, pen);
    auto oldBrush = SelectObject(dib.dc, GetStockObject(NULL_BRUSH));
    for (const auto &block : result.at("blocks"))
    {
        const auto &box = block.at("box");
        Rectangle(dib.dc, int(box[0][0].get<double>()), int(box[0][1].get<double>()), int(box[2][0].get<double>()),
                  int(box[2][1].get<double>()));
    }
    SelectObject(dib.dc, oldBrush);
    SelectObject(dib.dc, oldPen);
    DeleteObject(pen);
    return dib.ImageBytes();
}
void WriteImage(const std::filesystem::path &path, const Image &image)
{
    const auto bytes = Vision::EncodePng(image);
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    if (!file)
        throw std::runtime_error("Cannot save probe image");
}
void Pump(unsigned milliseconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < until);
    DwmFlush();
}
LRESULT CALLBACK ProbeProc(HWND h, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    if (message == WM_PAINT)
    {
        auto image = reinterpret_cast<const Image *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        PAINTSTRUCT paint{};
        auto dc = BeginPaint(h, &paint);
        if (image)
        {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = image->width;
            info.bmiHeader.biHeight = -LONG(image->height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            StretchDIBits(dc, 0, 0, image->width, image->height, 0, 0, image->width, image->height,
                          image->pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(h, &paint);
        return 0;
    }
    return DefWindowProcW(h, message, w, l);
}
RECT OwnedRectangle(HWND window)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!IsWindow(window) || pid != GetCurrentProcessId())
        throw std::runtime_error("The synthetic probe window was closed; no screen capture performed");
    RECT rectangle{};
    GetClientRect(window, &rectangle);
    POINT corners[2]{{rectangle.left, rectangle.top}, {rectangle.right, rectangle.bottom}};
    ClientToScreen(window, &corners[0]);
    ClientToScreen(window, &corners[1]);
    RECT bounds{corners[0].x, corners[0].y, corners[1].x, corners[1].y};
    struct Visibility
    {
        HWND source;
        RECT bounds;
        bool reached = false, blocked = false;
    } visibility{window, bounds};
    // Refuse a capture if another process has put any visible window above the
    // fixture rectangle. Reading HWND geometry is only a guard; no foreign
    // window is activated, captured, changed, or used for the test.
    EnumWindows(
        [](HWND h, LPARAM context) -> BOOL {
            auto &state = *reinterpret_cast<Visibility *>(context);
            if (h == state.source)
            {
                state.reached = true;
                return FALSE;
            }
            DWORD pid = 0, cloaked = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId() || !IsWindowVisible(h))
                return TRUE;
            if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                return TRUE;
            RECT rectangle{}, intersection{};
            if (GetWindowRect(h, &rectangle) && IntersectRect(&intersection, &rectangle, &state.bounds))
            {
                state.blocked = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&visibility));
    if (!visibility.reached || visibility.blocked || !IsWindowVisible(window))
        throw std::runtime_error("The owned fixture is covered; refusing to capture another window");
    return bounds;
}
HWND OwnedOverlay()
{
    HWND found = nullptr;
    EnumWindows(
        [](HWND h, LPARAM context) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId())
                return TRUE;
            wchar_t name[100]{};
            GetClassNameW(h, name, 100);
            if (std::wstring(name) != L"MetasequoiaVisionTranslation")
                return TRUE;
            *reinterpret_cast<HWND *>(context) = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}
std::set<unsigned> Markers(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const std::regex pattern(R"(row\s*([0-9]{3}))");
    std::set<unsigned> found;
    for (auto i = std::sregex_iterator(text.begin(), text.end(), pattern); i != std::sregex_iterator(); ++i)
        found.insert(unsigned(std::stoul((*i)[1].str())));
    return found;
}
std::pair<uint64_t, uint64_t> Differences(const Image &before, const Image &after,
                                          const std::vector<Vision::OverlayBlock> &blocks)
{
    if (before.width != after.width || before.height != after.height)
        throw std::runtime_error("Probe capture dimensions changed");
    uint64_t inside = 0, outside = 0;
    for (unsigned y = 0; y < before.height; ++y)
        for (unsigned x = 0; x < before.width; ++x)
        {
            const auto at = (size_t(y) * before.width + x) * 4;
            if (std::equal(before.pixels.begin() + at, before.pixels.begin() + at + 3, after.pixels.begin() + at))
                continue;
            const bool in = std::any_of(blocks.begin(), blocks.end(), [&](const auto &block) {
                return x >= unsigned(std::max(0L, block.bounds.left)) &&
                       x < unsigned(std::max(0L, block.bounds.right)) &&
                       y >= unsigned(std::max(0L, block.bounds.top)) && y < unsigned(std::max(0L, block.bounds.bottom));
            });
            ++(in ? inside : outside);
        }
    return {inside, outside};
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: MetasequoiaVisionLiveProbe.exe <output-directory>\n";
        return 2;
    }
    const auto output = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
    std::filesystem::create_directories(output);
    json report = {{"passed", false},
                   {"networkCalls", 0},
                   {"inputInjection", false},
                   {"newModelDownloads", 0},
                   {"translation", "Synthetic deterministic test labels"},
                   {"sourceFont", "Segoe UI"}};
    HWND ownedWindow = nullptr;
    Image visible;
    HWND overlay = nullptr;
    DWORD originalAffinity = 0;
    bool apartment = false;
    try
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        report["perMonitorV2"] = AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                                              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        const auto longImage = Fixture(900, 4296, 70);
        WriteImage(output / "long-source.png", longImage);
        auto longOcr = Vision::WindowsOcr(longImage);
        WriteImage(output / "long-bboxes.png", BoxFixture(longImage, longOcr));
        const auto markers = Markers(longOcr.at("text").get<std::string>());
        json missing = json::array();
        for (unsigned i = 1; i <= 70; ++i)
            if (!markers.count(i))
                missing.push_back(i);
        bool belowBoundary = false;
        for (const auto &block : longOcr.at("blocks"))
            belowBoundary |= block.at("box")[0][1].get<double>() > 2048;
        report["longOcr"] = {
            {"width", longImage.width},       {"height", longImage.height}, {"blocks", longOcr.at("blocks").size()},
            {"markersFound", markers.size()}, {"missingMarkers", missing},  {"bboxBeyond2048", belowBoundary}};
        const auto wideImage = Fixture(2600, 480, 5, true);
        WriteImage(output / "wide-source.png", wideImage);
        auto wideOcr = Vision::WindowsOcr(wideImage);
        bool rightEdge = false;
        for (const auto &block : wideOcr.at("blocks"))
            rightEdge |= block.at("box")[0][0].get<double>() > 2048 &&
                         block.at("text").get<std::string>().find("2026") != std::string::npos;
        report["wideOcr"] = {{"width", wideImage.width},
                             {"height", wideImage.height},
                             {"bboxBeyond2048", rightEdge},
                             {"blocks", wideOcr.at("blocks").size()}};
        std::ofstream(output / "ocr-results.json") << json{{"long", longOcr}, {"wide", wideOcr}}.dump(2);
        if (!missing.empty() || !belowBoundary || !rightEdge)
            throw std::runtime_error("Real Windows OCR did not preserve every probe marker and original-image box");

        visible = Fixture(900, 640, 9);
        WNDCLASSW c{};
        c.lpfnWndProc = ProbeProc;
        c.hInstance = GetModuleHandleW(nullptr);
        c.lpszClassName = L"MetasequoiaVisionLiveProbeFixture";
        RegisterClassW(&c);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int left = work.left + std::max(0L, (work.right - work.left - LONG(visible.width)) / 2);
        const int top = work.top + std::max(0L, (work.bottom - work.top - LONG(visible.height)) / 2);
        ownedWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, c.lpszClassName,
                                      L"Water fir OCR renderer integration probe", WS_POPUP, left, top, visible.width,
                                      visible.height, nullptr, nullptr, c.hInstance, &visible);
        if (!ownedWindow)
            throw std::runtime_error("Cannot create the owned synthetic probe window");
        SetWindowPos(ownedWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Pump(350);
        const auto bounds = OwnedRectangle(ownedWindow);
        const auto original = Vision::CaptureRectangle(bounds);
        WriteImage(output / "visible-original.png", original);
        const auto recognized = Vision::WindowsOcr(original);
        auto blocks = Vision::OverlayBlocksForImage(recognized, original.width, original.height);
        if (blocks.empty())
            throw std::runtime_error("Actual captured fixture did not produce positioned OCR blocks");
        for (size_t i = 0; i < blocks.size(); ++i)
            blocks[i].text = "Overlay verified " + std::to_string(i + 1);
        Vision::ShowTranslationOverlay(bounds, blocks);
        Pump(350);
        overlay = OwnedOverlay();
        if (!overlay || !GetWindowDisplayAffinity(overlay, &originalAffinity))
            throw std::runtime_error("Cannot find and inspect this probe's own product overlay");
        // Product overlays exclude themselves from capture. Temporarily permit a
        // screenshot of THIS PROCESS'S overlay to verify the actual renderer;
        // restore its affinity before exercising Hide and original capture.
        if (!SetWindowDisplayAffinity(overlay, WDA_NONE))
            throw std::runtime_error("Cannot allow capture of this probe's own overlay");
        Pump(200);
        OwnedRectangle(ownedWindow);
        const auto covered = Vision::CaptureRectangle(bounds);
        WriteImage(output / "visible-overlay.png", covered);
        const auto [inside, outside] = Differences(original, covered, blocks);
        SetWindowDisplayAffinity(overlay, originalAffinity);
        Vision::HideTranslationOverlay();
        Pump(250);
        OwnedRectangle(ownedWindow);
        const auto restored = Vision::CaptureRectangle(bounds);
        WriteImage(output / "visible-restored.png", restored);
        const auto [restoredInside, restoredOutside] = Differences(original, restored, blocks);
        report["overlay"] = {{"dpi", GetDpiForWindow(ownedWindow)},
                             {"blocks", blocks.size()},
                             {"changedInsideBoxes", inside},
                             {"changedOutsideBoxes", outside},
                             {"restoredChangedPixels", restoredInside + restoredOutside},
                             {"affinityDisabledOnlyForOwnedOverlayScreenshot", true}};
        if (!inside || outside || restoredInside || restoredOutside)
            throw std::runtime_error(
                "Overlay changed outside its boxes, did not render, or did not restore the original");
        report["passed"] = true;
    }
    catch (const std::exception &e)
    {
        report["error"] = e.what();
    }
    catch (const winrt::hresult_error &e)
    {
        report["error"] = winrt::to_string(e.message());
    }
    if (overlay)
        SetWindowDisplayAffinity(overlay, originalAffinity);
    Vision::HideTranslationOverlay();
    if (ownedWindow && IsWindow(ownedWindow))
        DestroyWindow(ownedWindow);
    if (apartment)
        winrt::uninit_apartment();
    std::ofstream(output / "report.json") << report.dump(2);
    std::cout << report.dump() << std::endl;
    return report.at("passed") == true ? 0 : 1;
}
