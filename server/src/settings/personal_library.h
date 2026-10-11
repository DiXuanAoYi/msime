#pragma once
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace PersonalLibrary
{
nlohmann::json Handle(const nlohmann::json &request, HWND owner);
std::vector<std::string> AppWords(const std::string &application, const std::string &code);
std::string ForegroundApplication();
} // namespace PersonalLibrary
