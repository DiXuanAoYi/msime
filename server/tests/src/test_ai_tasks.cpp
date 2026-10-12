#include "ai/ai_assistant.h"
#include "ai/ai_assistant_cache_key.h"
#include "ai/ai_assistant_request.h"
#include "settings/api_credential_test.h"
#include "settings/settings_webview_error.h"
#include "tests/includes/test_framework.h"

TEST_CASE(ai_local_endpoint_has_exact_loopback_boundary)
{
    REQUIRE(AiAssistant::IsLoopbackEndpoint("http://127.0.0.1:18769/v1/chat/completions"));
    REQUIRE(AiAssistant::IsLoopbackEndpoint("http://localhost:11434/v1/chat/completions"));
    REQUIRE(AiAssistant::IsLoopbackEndpoint("http://[::1]:11434/v1/chat/completions"));
    REQUIRE(!AiAssistant::IsLoopbackEndpoint("http://localhost.example.test/v1/chat/completions"));
    REQUIRE(!AiAssistant::IsLoopbackEndpoint("http://127.0.0.1.example.test/v1/chat/completions"));
    REQUIRE(!AiAssistant::IsLoopbackEndpoint("http://user:password@localhost:11434/v1/chat/completions"));
    REQUIRE(!AiAssistant::IsLoopbackEndpoint("file:///C:/model"));
    REQUIRE(!AiAssistant::IsLoopbackEndpoint("ftp://127.0.0.1/model"));
}
TEST_CASE(ai_rerank_marker_is_trusted_only_for_verified_local_pinyin)
{
    const nlohmann::json response = {{"capability", "rerank"}};
    const std::string local = "http://127.0.0.1:18769/v1/chat/completions";
    REQUIRE(AiAssistant::IsLocalRerankResponse(AiAssistant::Task::Pinyin, "local", local, response));
    REQUIRE(!AiAssistant::IsLocalRerankResponse(AiAssistant::Task::Question, "local", local, response));
    REQUIRE(!AiAssistant::IsLocalRerankResponse(AiAssistant::Task::Pinyin, "custom", local, response));
    REQUIRE(!AiAssistant::IsLocalRerankResponse(AiAssistant::Task::Pinyin, "local", "https://remote.example.test",
                                                response));
    REQUIRE(!AiAssistant::IsLocalRerankResponse(AiAssistant::Task::Pinyin, "local", local, {{"capability", 3}}));
}
TEST_CASE(ai_question_is_separate_and_omits_application_context)
{
    AiAssistant::Request request;
    request.task = AiAssistant::Task::Question;
    request.text = "三个直是什么字";
    request.context = "private application context";
    request.config.prompt = "保留原来的用户提示词内容";
    auto body = AiAssistant::detail::BuildRequestBody(request);
    const auto input = nlohmann::json::parse(body["messages"][1]["content"].get<std::string>());
    REQUIRE(input["task"] == "question");
    REQUIRE(input["question"] == request.text);
    REQUIRE(!input.contains("context"));
    REQUIRE(!input.contains("segmented_pinyin"));
    REQUIRE(body.dump().find(request.context) == std::string::npos);
    REQUIRE(body["messages"][0]["content"].get<std::string>().find(request.config.prompt) == 0);
}
TEST_CASE(ai_reranking_original_candidates_are_local_only)
{
    AiAssistant::Request request;
    request.source_candidates = {{"输入法", "shu'ru'fa", "词库"}};
    request.config.provider = "deepseek";
    request.config.endpoint = "https://api.example.test/chat/completions";
    const auto remote = AiAssistant::detail::BuildRequestBody(request).dump();
    REQUIRE(remote.find("original_candidates") == std::string::npos);
    request.config.provider = "local";
    request.config.endpoint = "http://127.0.0.1:18769/v1/chat/completions";
    const auto local = AiAssistant::detail::BuildRequestBody(request).dump();
    REQUIRE(local.find("original_candidates") != std::string::npos);
    request.config.endpoint = "https://api.example.test/chat/completions";
    REQUIRE(AiAssistant::detail::BuildRequestBody(request).dump().find("original_candidates") == std::string::npos);
}
TEST_CASE(ai_deepseek_thinking_budget_is_explicit_and_provider_scoped)
{
    AiAssistant::Request request;
    request.config.provider = "deepseek";
    request.config.thinking = "disabled";
    REQUIRE(AiAssistant::detail::BuildRequestBody(request)["thinking"]["type"] == "disabled");
    REQUIRE(AiAssistant::detail::BuildRequestBody(request)["max_tokens"] == 512);
    for (const std::string mode : {"low", "high", "max"})
    {
        request.config.thinking = mode;
        auto body = AiAssistant::detail::BuildRequestBody(request);
        REQUIRE(body["thinking"]["type"] == "enabled");
        REQUIRE(body["reasoning_effort"] == mode);
        REQUIRE(body["max_tokens"] == 8192);
        REQUIRE(AiAssistant::detail::RequestTimeoutMs(request.config) == 30000L);
    }
    request.config.provider = "custom";
    REQUIRE(!AiAssistant::detail::BuildRequestBody(request).contains("thinking"));
    REQUIRE(!AiAssistant::detail::BuildRequestBody(request).contains("reasoning_effort"));
}
TEST_CASE(ai_completion_returns_only_added_text)
{
    const auto result = AiAssistant::ParseTaskContent(AiAssistant::Task::Completion,
                                                      R"({"completion":"今天下午开会，记得带文件。"})", "今天下午", 3);
    REQUIRE(result.completion == "开会，记得带文件。");
    REQUIRE(result.candidates.empty());
}
TEST_CASE(ai_question_keeps_distinct_answers_and_supplied_readings)
{
    const auto result = AiAssistant::ParseTaskContent(
        AiAssistant::Task::Question,
        R"({"answers":[{"text":"三个直怎么读"},{"text":"矗","pinyin":"chù"},{"text":"矗","pinyin":"chù"},{"text":"森","pinyin":"sēn"}]})",
        "三个直怎么读", 10);
    REQUIRE(result.candidates.size() == 2);
    REQUIRE(result.candidates[0].text == "矗");
    REQUIRE(result.candidates[0].pinyin == "chù");
}
TEST_CASE(ai_offline_question_needs_no_enabled_model_or_token)
{
    AiAssistant::Request request;
    request.task = AiAssistant::Task::Question;
    request.text = "三个木是什么字";
    const auto result = AiAssistant::RunTask(request);
    REQUIRE(result.error.empty());
    REQUIRE(result.candidates.size() == 1);
    REQUIRE(result.candidates[0].text == "森");
    REQUIRE(result.candidates[0].pinyin == "sēn");
    REQUIRE(result.candidates[0].source == "本地叠字部件表");
    REQUIRE(AiAssistant::LookupOfflineQuestion("完全未知的字形结构").empty());
    request.text = "三个母是什么字"; // A dictionary's provisional top conversion can be wrong.
    request.pinyin_segments = {"san", "ge", "mu", "shi", "shen", "me", "zi"};
    const auto pinyinFallback = AiAssistant::RunTask(request);
    REQUIRE(pinyinFallback.error.empty());
    REQUIRE(pinyinFallback.candidates[0].text == "森");
    REQUIRE(pinyinFallback.candidates[0].pinyin == "sēn");
}
TEST_CASE(ai_cancel_and_invalid_utf8_do_not_start_http)
{
    AiAssistant::Request request;
    REQUIRE(!AiAssistant::RunTask(request, [] { return true; }).error.empty());
    request.config.enabled = true;
    request.config.provider = "local";
    request.config.endpoint = "http://127.0.0.1:18769/v1/chat/completions";
    request.config.model = "fixture";
    request.pinyin_segments = {"ni", "hao"};
    request.context = std::string(1, char(0xff));
    REQUIRE(AiAssistant::RunTask(request).error.find("编码") != std::string::npos);
}
TEST_CASE(ai_model_endpoint_and_cache_are_task_scoped)
{
    REQUIRE(ApiCredentialTest::ModelListEndpoint("https://example.test/v1/chat/completions") ==
            "https://example.test/v1/models");
    AiAssistant::Request request;
    request.task = AiAssistant::Task::Pinyin;
    const auto before = AiAssistant::detail::BuildSuggestionCacheKey(request);
    request.task = AiAssistant::Task::Completion;
    REQUIRE(AiAssistant::detail::BuildSuggestionCacheKey(request) != before);
    request.config.token = "secret-must-not-be-cache-key";
    REQUIRE(AiAssistant::detail::BuildSuggestionCacheKey(request).find(request.config.token) == std::string::npos);
}
TEST_CASE(settings_webview_failure_is_actionable_and_stage_specific)
{
    const auto text = SettingsWebView::FailureMessage(L"创建 WebView2 环境（异步）", E_ACCESSDENIED);
    REQUIRE(text.find(L"80070005") != std::wstring::npos);
    REQUIRE(text.find(L"创建 WebView2 环境（异步）") != std::wstring::npos);
    REQUIRE(text.find(L"Runtime") != std::wstring::npos);
    REQUIRE(text.find(L"不会关闭输入法") != std::wstring::npos);
}

TEST_CASE(ai_question_preserves_polyphonic_readings_for_same_answer)
{
    const auto result = AiAssistant::ParseTaskContent(
        AiAssistant::Task::Question,
        R"({"answers":[{"text":"重","pinyin":"zhòng"},{"text":"重","pinyin":"chóng"},{"text":"重","pinyin":"chóng"}]})",
        "多音字", 10);
    REQUIRE(result.candidates.size() == 2);
    REQUIRE(result.candidates[0].pinyin == "zhòng");
    REQUIRE(result.candidates[1].pinyin == "chóng");
}
