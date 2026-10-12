#include "ai/ai_assistant.h"
#include "ai/ai_assistant_request.h"
#include "settings/api_credential_test.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

int RunAiTaskFixtureProbe(const char *endpoint, const char *reportPath)
{
    if (!AiAssistant::IsLoopbackEndpoint(endpoint))
        return 2;
    AiAssistant::Request request;
    request.config.enabled = true;
    request.config.provider = "local";
    request.config.endpoint = endpoint;
    request.config.model = "fixture";
    request.config.token = "fixture-local-test-auth";
    request.config.candidate_limit = 3;
    nlohmann::json report = nlohmann::json::object();
    request.pinyin_segments = {"ni", "hao"};
    const auto pinyin = AiAssistant::RunTask(request);
    request.task = AiAssistant::Task::Completion;
    request.text = "今天下午";
    request.context = "";
    const auto completion = AiAssistant::RunTask(request);
    request.task = AiAssistant::Task::Question;
    request.text = "多音字人工夹具";
    request.context = "fixture-context-must-be-omitted";
    const auto question = AiAssistant::RunTask(request);
    request.task = AiAssistant::Task::Pinyin;
    request.config.model = "fixture-rerank";
    request.source_candidates = {{"你好", "ni'hao", "词库"}};
    const auto rerank = AiAssistant::RunTask(request);
    request.config.provider = "custom";
    const auto untrustedRerank = AiAssistant::RunTask(request);
    ApiCredentialTest::Request listRequest;
    listRequest.service = "ai.assistant";
    listRequest.config = {{"provider", "local"}, {"endpoint", endpoint}, {"token", "fixture-local-test-auth"}};
    const auto models = ApiCredentialTest::FetchModels(listRequest);
    ApiCredentialTest::Request testRequest = listRequest;
    testRequest.config["model"] = "fixture";
    const auto connection = ApiCredentialTest::Run(testRequest);
    const bool ok = pinyin.error.empty() && pinyin.candidates.size() == 1 && pinyin.candidates[0].text == "你好" &&
                    completion.error.empty() && completion.completion == "开会，记得带文件。" &&
                    question.error.empty() && question.candidates.size() == 2 &&
                    question.candidates[0].pinyin == "zhòng" && question.candidates[1].pinyin == "chóng" && models.ok &&
                    models.models.size() == 2 && connection.ok && rerank.rerank_only && !untrustedRerank.rerank_only;
    report["ok"] = ok;
    report["pinyin"] = {{"error", pinyin.error}, {"count", pinyin.candidates.size()}};
    report["completion"] = {{"error", completion.error}, {"text", completion.completion}};
    report["question"] = {{"error", question.error}, {"count", question.candidates.size()}};
    report["models"] = {{"ok", models.ok}, {"count", models.models.size()}};
    report["connection"] = {{"ok", connection.ok}, {"message", connection.message}};
    report["localRerankTrusted"] = rerank.rerank_only && !untrustedRerank.rerank_only;
    report["source"] = "owned synthetic loopback HTTP fixture, no cloud provider or actual model";
    std::ofstream output(std::filesystem::u8path(reportPath));
    output << report.dump(2);
    return ok && output ? 0 : 1;
}
