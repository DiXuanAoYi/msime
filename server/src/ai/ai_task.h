#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace AiAssistant
{
enum class Task
{
    Pinyin,
    Completion,
    Question
};
struct Answer
{
    std::string text;
    std::string pinyin;
    std::string source;
    std::string explanation;
};
struct Result
{
    Task task = Task::Pinyin;
    std::vector<Answer> candidates;
    std::string completion;
    std::string error;
    std::string identity;
    uint64_t generation = 0;
    bool rerank_only = false;
};
using TaskCallback = std::function<void(const Result &)>;
std::string TaskName(Task task);
Task ParseTask(const std::string &task);
bool IsLoopbackEndpoint(const std::string &endpoint);
bool IsLocalRerankResponse(Task task, const std::string &provider, const std::string &endpoint,
                           const nlohmann::json &response);
std::vector<Answer> LookupOfflineQuestion(const std::string &text);
Result ParseTaskContent(Task task, const std::string &content, const std::string &precedingText, int limit);
} // namespace AiAssistant
