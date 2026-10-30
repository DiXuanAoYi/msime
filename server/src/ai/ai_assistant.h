#pragma once

#include "config/ime_config.h"
#include "ai_task.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AiAssistant
{
struct Request
{
    std::vector<std::string> pinyin_segments;
    std::string context;
    std::string identity;
    AiAssistantConfig config;
    Task task = Task::Pinyin;
    std::string text;
    std::vector<Answer> source_candidates;
};

using ApplyCallback = std::function<void(const std::string &, const std::string &, uint64_t)>;
void Start(ApplyCallback callback, TaskCallback taskCallback = {});
Result RunTask(const Request &request, const std::function<bool()> &cancelled = {});
void Stop();
uint64_t OnInputChanged(Request request);
void Clear();
} // namespace AiAssistant
