#include "ai_assistant_request.h"
namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request)
{
    const auto &config = request.config;
    const bool thinking = config.provider == "deepseek" &&
                          (config.thinking == "low" || config.thinking == "high" || config.thinking == "max");
    nlohmann::json input = {{"task", TaskName(request.task)}, {"candidate_limit", config.candidate_limit}};
    std::string prompt = config.prompt;
    if (request.task == Task::Question)
    {
        input["question"] = request.text;
        prompt += "\n本任务是查字/问字，不是拼音转换。不要带入应用前文，不复述问题。只返回 JSON："
                  "{\"answers\":[{\"text\":\"森\",\"pinyin\":\"sēn\"}]}。可有多个答案；"
                  "text 仅放答案，读音放 pinyin，无法确定不要编造。";
    }
    else if (request.task == Task::Completion)
    {
        input["before"] = request.context + request.text;
        prompt += "\n本任务是文本续写，不是拼音转换。根据 before 接着写，只返回新增文字，不重复已有前文。"
                  "只返回 JSON：{\"completion\":\"新增文字\"}。";
    }
    else
    {
        input["segmented_pinyin"] = request.pinyin_segments;
        input["context"] = request.context;
        // Original candidates are for the owned local QJM reranker only. Never
        // expand the information sent to a remote model with these candidates.
        if (config.provider == "local" && IsLoopbackEndpoint(config.endpoint) && !request.source_candidates.empty())
        {
            input["original_candidates"] = nlohmann::json::array();
            for (const auto &answer : request.source_candidates)
                input["original_candidates"].push_back({{"text", answer.text}, {"pinyin", answer.pinyin}});
        }
    }
    nlohmann::json body = {
        {"model", config.model},
        {"stream", false},
        {"temperature", 0.2},
        {"max_tokens", thinking ? 8192 : (request.task == Task::Completion ? 1024 : 512)},
        {"response_format", {{"type", "json_object"}}},
        {"messages", {{{"role", "system"}, {"content", prompt}}, {{"role", "user"}, {"content", input.dump()}}}}};
    if (thinking)
    {
        body["thinking"] = {{"type", "enabled"}};
        body["reasoning_effort"] = config.thinking;
    }
    else if (config.provider == "deepseek")
        body["thinking"] = {{"type", "disabled"}};
    return body;
}
long RequestTimeoutMs(const AiAssistantConfig &config)
{
    return config.provider == "deepseek" && config.thinking != "disabled"
               ? 30000L
               : (config.provider == "local" ? 15000L : 8000L);
}
} // namespace AiAssistant::detail
