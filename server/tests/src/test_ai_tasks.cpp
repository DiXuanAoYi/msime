#include "ai/ai_assistant.h"
#include "ai/character_question.h"
#include "ai/ai_assistant_cache_key.h"
#include "ai/ai_assistant_request.h"
#include "settings/api_credential_test.h"
#include "settings/settings_webview_error.h"
#include "tests/includes/test_framework.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

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
    REQUIRE(AiAssistant::LookupOfflineQuestion("三个木加一个日").empty());
    REQUIRE(AiAssistant::LookupOfflineQuestion("不是三个木").empty());
}

TEST_CASE(ai_component_question_recurses_filters_layout_and_preserves_ambiguity)
{
    const nlohmann::json catalog = {
        {"version", 1},
        {"entries",
         nlohmann::json::array({nlohmann::json::array({"草", "cǎo", nlohmann::json::array({"⿱艹早"})}),
                                nlohmann::json::array({"早", "zǎo", nlohmann::json::array({"⿱日十"})}),
                                nlohmann::json::array({"杏", "xìng", nlohmann::json::array({"⿱木口"})}),
                                nlohmann::json::array({"束", "shù", nlohmann::json::array({"⿻木口"})}),
                                nlohmann::json::array({"休", "xiū", nlohmann::json::array({"⿰亻木"})}),
                                nlohmann::json::array({"朤", "lǎng", nlohmann::json::array({"⿱⿰月月⿰月月"})}),
                                nlohmann::json::array({"坏数据", "", nlohmann::json::array({"⿱"})})})}};
    for (const std::string text : {"草字头＋日＋十", "草：草字头加一个日再加一个十是什么字？", "caozitoujiarijiashi"})
    {
        const auto answer = AiAssistant::detail::LookupComponentQuestion(text, catalog);
        REQUIRE(answer.size() == 1);
        REQUIRE(answer[0].text == "草");
        REQUIRE(answer[0].pinyin == "cǎo");
        REQUIRE(answer[0].explanation.find("艹＋日＋十") != std::string::npos);
    }
    const auto ambiguous = AiAssistant::detail::LookupComponentQuestion("木＋口", catalog);
    REQUIRE(ambiguous.size() == 2);
    REQUIRE(AiAssistant::detail::LookupComponentQuestion("上下结构木＋口", catalog)[0].text == "杏");
    REQUIRE(AiAssistant::detail::LookupComponentQuestion("左右结构人＋木", catalog)[0].text == "休");
    REQUIRE(AiAssistant::detail::LookupComponentQuestion("月+月+月+月", catalog)[0].text == "朤");
    REQUIRE(AiAssistant::detail::LookupComponentQuestion("四个月是什么字", catalog)[0].text == "朤");
    REQUIRE(AiAssistant::detail::LookupComponentQuestion("日＋十＋木", catalog).empty());
    REQUIRE(AiAssistant::detail::LookupComponentQuestion("木＋口＋未知的部分", catalog).empty());
    REQUIRE(AiAssistant::detail::LookupComponentQuestion("木+口+日+十+月+土+女+子+火", catalog).empty());
    REQUIRE(AiAssistant::detail::LookupComponentQuestion(std::string(1, char(0xff)), catalog).empty());
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
TEST_CASE(ai_model_list_preserves_current_display_name_and_real_api_id)
{
    const auto result = ApiCredentialTest::ParseModelList(
        R"({"data":[null,7,{"id":true},{"id":"deepseek-flash","name":"DeepSeek-V4.1-Flash"},{"id":"deepseek-v4-pro","name":"DeepSeek-V4-Pro"},"manual-model"]})");
    REQUIRE(result.ok);
    REQUIRE(result.models.size() == 3);
    REQUIRE(result.details.size() == 2);
    REQUIRE(result.details[0].id == "deepseek-flash");
    REQUIRE(result.details[0].name == "DeepSeek-V4.1-Flash");
    REQUIRE(!ApiCredentialTest::ParseModelList(R"({"data":false})").ok);
    REQUIRE(ApiCredentialTest::ModelListEndpoint("https://api.deepseek.com/chat/completions") ==
            "https://api.deepseek.com/models");
}
TEST_CASE(settings_webview_failure_is_actionable_and_stage_specific)
{
    const auto text = SettingsWebView::FailureMessage(L"创建 WebView2 环境（异步）", E_ACCESSDENIED);
    REQUIRE(text.find(L"80070005") != std::wstring::npos);
    REQUIRE(text.find(L"创建 WebView2 环境（异步）") != std::wstring::npos);
    REQUIRE(text.find(L"Runtime") != std::wstring::npos);
    REQUIRE(text.find(L"不会关闭输入法") != std::wstring::npos);
}

TEST_CASE(ai_question_worker_delivers_errors_with_actual_generation_after_restart)
{
    std::mutex mutex;
    std::condition_variable completed;
    std::optional<AiAssistant::Result> received;
    struct StopWorker
    {
        ~StopWorker()
        {
            AiAssistant::Stop();
        }
    } cleanup;
    const auto before = AiAssistant::OnInputChanged({});
    AiAssistant::Start({}, [&](const AiAssistant::Result &result) {
        std::lock_guard lock(mutex);
        received = result;
        completed.notify_one();
    });
    AiAssistant::Request request;
    request.task = AiAssistant::Task::Question;
    request.text = "未知字形描述测试";
    request.identity = "fixture-question";
    request.config.enabled = true;
    request.config.provider = "deepseek";
    request.config.token.clear(); // Deterministic validation failure, no HTTP.
    const auto generation = AiAssistant::OnInputChanged(request);
    REQUIRE(generation > before);
    std::unique_lock lock(mutex);
    REQUIRE(completed.wait_for(lock, std::chrono::seconds(3), [&] { return received.has_value(); }));
    REQUIRE(!received->error.empty());
    REQUIRE(received->identity == request.identity);
    REQUIRE(received->generation == generation);
    lock.unlock();
    AiAssistant::Stop();
    REQUIRE(AiAssistant::OnInputChanged({}) > generation);
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
