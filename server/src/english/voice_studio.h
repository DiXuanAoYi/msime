#pragma once
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <vector>

namespace VoiceStudio
{
std::string LoadSavedDraft(const std::string &filename);
std::string TranscribeAudio(const std::vector<float> &samples, const std::function<bool()> &cancelled);
// Local drafts and explicit user actions only; no chat send/input automation.
nlohmann::json Handle(const nlohmann::json &request, HWND owner);
void Stop();
std::vector<float> ReadAudioFile(const std::filesystem::path &path, const std::function<bool()> &cancelled);
} // namespace VoiceStudio
