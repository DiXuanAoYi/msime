#pragma once
#include "ai_assistant.h"
#include <nlohmann/json.hpp>
namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request);
long RequestTimeoutMs(const AiAssistantConfig &config);
} // namespace AiAssistant::detail
