#pragma once
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace Vision
{
struct Image
{
    unsigned width = 0, height = 0;
    std::vector<unsigned char> pixels;
};
RECT PickRectangle(HWND owner);
Image CaptureRectangle(RECT rectangle);
Image DecodeImage(const std::string &bytes);
std::string EncodePng(const Image &image);
std::wstring NormalizeChineseOcrSpacing(const std::wstring &text);
nlohmann::json WindowsOcr(const Image &image);
void ShowTranslationOverlay(RECT rectangle, const std::string &text);
void HideTranslationOverlay();
} // namespace Vision
