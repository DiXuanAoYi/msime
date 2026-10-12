#include "ai_workbench.h"
#include "ai/ai_assistant.h"
#include "config/ime_config.h"
#include "settings/api_credential_test.h"
#include <atomic>
#include <sstream>

namespace
{
std::atomic<uint64_t> generation{0};
}
namespace AiWorkbench
{
ApiCredentialTest::Request ReadConnection(const nlohmann::json &input)
{
    ApiCredentialTest::Request request;
    request.service = input.value("service", std::string("ai.assistant"));
    if (input.contains("config") && input["config"].is_object())
        for (auto it = input["config"].begin(); it != input["config"].end(); ++it)
            if (it.value().is_string())
                request.config[it.key()] = it.value().get<std::string>();
    return request;
}
nlohmann::json HandleModelList(const nlohmann::json &input)
{
    const auto result = ApiCredentialTest::FetchModels(ReadConnection(input));
    return {{"type", "apiModelListResult"},
            {"requestId", input.value("requestId", std::string())},
            {"ok", result.ok},
            {"message", result.message},
            {"models", result.models}};
}
nlohmann::json HandleCredentialTest(const nlohmann::json &input)
{
    const auto result = ApiCredentialTest::Run(ReadConnection(input));
    return {{"type", "apiCredentialTestResult"},
            {"requestId", input.value("requestId", std::string())},
            {"ok", result.ok},
            {"message", result.message}};
}
void Cancel()
{
    ++generation;
}
nlohmann::json Handle(const nlohmann::json &input)
{
    const auto id = input.value("requestId", std::string());
    const auto task = input.value("task", std::string("pinyin"));
    nlohmann::json response = {{"type", "aiTaskResult"}, {"requestId", id},  {"ok", false},
                               {"task", task},           {"completion", ""}, {"answers", nlohmann::json::array()},
                               {"message", ""}};
    if (input.value("action", std::string()) == "cancel")
    {
        Cancel();
        response["message"] = "已取消";
        return response;
    }
    const auto epoch = ++generation;
    try
    {
        AiAssistant::Request request;
        request.task = AiAssistant::ParseTask(task);
        request.text = input.value("text", std::string());
        request.context =
            request.task == AiAssistant::Task::Question ? std::string() : input.value("context", std::string());
        request.identity = id;
        request.config = GetConfiguredAiAssistantSnapshot();
        request.config.enabled = true; // This handler only runs for the user's explicit preview action.
        if (request.task == AiAssistant::Task::Pinyin)
        {
            std::string text = request.text;
            for (char &c : text)
                if (c == '\'')
                    c = ' ';
            std::istringstream stream(text);
            std::string segment;
            while (stream >> segment)
                request.pinyin_segments.push_back(segment);
        }
        const auto result = AiAssistant::RunTask(request, [epoch] { return generation != epoch; });
        if (generation != epoch)
        {
            response["message"] = "已取消或已有新任务";
            return response;
        }
        response["completion"] = result.completion;
        for (const auto &answer : result.candidates)
            response["answers"].push_back(
                {{"text", answer.text}, {"pinyin", answer.pinyin}, {"source", answer.source}});
        response["ok"] = result.error.empty();
        response["message"] = result.error.empty() ? "已生成，可编辑、复制或手动上屏；不会发送消息" : result.error;
    }
    catch (...)
    {
        response["message"] = "AI 任务参数或执行失败，请检查设置";
    }
    return response;
}
} // namespace AiWorkbench
