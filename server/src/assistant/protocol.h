#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace ReplyStudio
{
void Validate(const nlohmann::json &request);
nlohmann::json BuildPayload(const nlohmann::json &request, const std::string &model);
nlohmann::json ParseResult(const std::string &body, const std::string &action, int count);
} // namespace ReplyStudio
