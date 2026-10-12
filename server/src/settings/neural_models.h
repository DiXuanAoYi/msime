#pragma once
#include <Windows.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
namespace NeuralModels
{
// Empty when absent. A managed desktop model wins; old data-root file remains intact.
std::string EffectiveDesktopModelPath();
std::filesystem::path PromptForModelFile(HWND owner, const std::string &id);
nlohmann::json Handle(const nlohmann::json &request, const std::filesystem::path &import = {});
} // namespace NeuralModels
