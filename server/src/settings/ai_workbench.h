#pragma once
#include <nlohmann/json.hpp>
namespace AiWorkbench
{
void Cancel();
nlohmann::json Handle(const nlohmann::json &request);
nlohmann::json HandleModelList(const nlohmann::json &request);
nlohmann::json HandleCredentialTest(const nlohmann::json &request);
} // namespace AiWorkbench
