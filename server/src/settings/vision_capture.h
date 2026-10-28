#pragma once
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace Vision
{
struct Image
{
    unsigned width = 0, height = 0;
    std::vector<unsigned char> pixels;
};
inline constexpr uint64_t MaxImagePixels = 64000000;
inline constexpr size_t MaxEncodedImageBytes = 25 * 1024 * 1024;
struct OverlayBlock
{
    // Bounds are physical pixels relative to the captured image, never CSS/DIP coordinates.
    RECT bounds{};
    std::string source, text;
    bool region = false;
    std::string format = "text";
};
std::string OverlayDisplayText(const std::string &text, const std::string &format);
std::vector<OverlayBlock> OverlayBlocksForImage(const nlohmann::json &result, unsigned width, unsigned height);
std::vector<RECT> ImageTileRegions(unsigned width, unsigned height, unsigned tileSize = 2048, unsigned overlap = 96);
// magnifier: show a zoomed view of the pixels under the cursor with their colour while picking.
RECT PickRectangle(HWND owner, bool magnifier = false);
Image CaptureRectangle(RECT rectangle);
Image DecodeImage(const std::string &bytes);
std::string EncodePng(const Image &image);
std::wstring NormalizeChineseOcrSpacing(const std::wstring &text);
nlohmann::json WindowsOcr(const Image &image, const std::function<bool()> &cancelled = {});
void ShowTranslationOverlay(RECT rectangle, const std::vector<OverlayBlock> &blocks);
void HideTranslationOverlay();
} // namespace Vision
