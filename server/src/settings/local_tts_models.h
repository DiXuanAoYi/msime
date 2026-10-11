#pragma once
#include <nlohmann/json.hpp>
namespace LocalTts
{
nlohmann::json Handle(const nlohmann::json &request);
}
