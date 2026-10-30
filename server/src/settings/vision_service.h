#pragma once
#include <Windows.h>
#include "settings/vision_capture.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
namespace Vision
{
nlohmann::json Handle(const nlohmann::json &request, HWND owner);
void Stop();
// Recognizes one image with the configured provider (smart routing, line merging) and returns the OCR result.
nlohmann::json RecognizeImage(const Image &image, const std::string &task, const std::function<bool()> &cancelled);
// Starts the live screen translation of a region on the vision worker. False when another task is running.
bool StartLive(RECT rectangle, const std::string &source, const std::string &target, const std::string &task);
// line_mode "smart" joins hard-wrapped lines of one paragraph, "one" joins everything, "keep" changes nothing.
std::string MergeOcrLines(const std::string &text, const std::string &mode);
bool Busy();
void AddHistory(const std::string &kind, const std::string &text, const std::string &translation);
} // namespace Vision
