#pragma once
#include "ai_task.h"
namespace AiAssistant
{
// Component matching never calls a model, records application context or commits text.
std::vector<Answer> LookupComponentQuestion(const std::string &text);
namespace detail
{
std::vector<Answer> LookupComponentQuestion(const std::string &text, const nlohmann::json &catalog);
}
} // namespace AiAssistant
