#pragma once
#include <Windows.h>
#include <nlohmann/json.hpp>
namespace Vision
{
nlohmann::json Handle(const nlohmann::json &request, HWND owner);
void Stop();
} // namespace Vision
