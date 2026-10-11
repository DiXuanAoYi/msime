#pragma once
#include <nlohmann/json.hpp>
namespace LocalTts
{
nlohmann::json Handle(const nlohmann::json &request);
nlohmann::json ModelInfo(const std::string &id);
} // namespace LocalTts
