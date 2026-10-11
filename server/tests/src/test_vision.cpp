#include "tests/includes/test_framework.h"
#include "settings/vision_protocol.h"
#include "settings/vision_subtitles.h"
#include "settings/vision_config.h"
#include "settings/personal_library.h"
#include "settings/vision_service.h"
#include "settings/vision_capture.h"
#include <fstream>
#include <thread>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <limits>
using nlohmann::json;
int RunVisionFixtureProbe(const char *path)
{
    try
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("fixture missing");
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        VisionConfig::Load(toml::table{});
        auto wait = [] {
            json state;
            for (int i = 0; i < 200; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                state = Vision::Handle({{"action", "status"}, {"imageRevision", 1}}, nullptr);
                if (!state.value("busy", false))
                    break;
            }
            if (state.value("busy", false) || !state.value("error", "").empty())
                throw std::runtime_error(state.value("error", "timeout"));
            return state;
        };
        auto loaded = Vision::Handle({{"action", "import"}, {"image", Vision::Base64(bytes)}}, nullptr);
        if (!loaded.value("ok", false))
            throw std::runtime_error(loaded.value("message", "failed"));
        wait();
        Vision::Handle({{"action", "recognize"}, {"task", "text"}}, nullptr);
        auto recognized = wait();
        auto text = recognized.value("text", "");
        if (text.find("12345") == std::string::npos)
            throw std::runtime_error("OCR did not recognize fixture digits");
        Vision::Handle({{"action", "translate"}, {"text", "hello"}, {"source", "en"}, {"target", "zh"}}, nullptr);
        auto translated = wait();
        json report = {{"ocr", text},
                       {"translation", translated.value("translation", "")},
                       {"width", recognized.value("width", 0)},
                       {"height", recognized.value("height", 0)}};
        std::cout << report.dump() << std::endl;
        Vision::Stop();
        return 0;
    }
    catch (const std::exception &e)
    {
        Vision::Stop();
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
TEST_CASE(vision_shiliu_keeps_line_order_geometry_and_unicode)
{
    auto out = Vision::NormalizeOcr(
        "shiliu",
        {{"data",
          {{"content",
            json::array({{{"text", "函数 f(x)"}, {"prob", 0.9}, {"keypoints", json::array({{{"x", 2}, {"y", 3}}})}},
                         {{"text", "return x < 2"}}})}}}},
        "code");
    REQUIRE(out.at("text") == "函数 f(x)\nreturn x < 2");
    REQUIRE(out.at("blocks").size() == 2);
    REQUIRE(out.at("blocks")[0].at("box")[0].at("x") == 2);
    REQUIRE(out.contains("raw"));
}
TEST_CASE(vision_chinese_spacing_preserves_latin_code_and_line_boundaries)
{
    REQUIRE(Vision::NormalizeChineseOcrSpacing(L"今 天 下 午 ， 见 面 。") == L"今天下午，见面。");
    REQUIRE(Vision::NormalizeChineseOcrSpacing(L"水 杉 OCR\n  unit_price = count * 2") ==
            L"水杉 OCR\n  unit_price = count * 2");
}
TEST_CASE(vision_cloud_table_preserves_spans_and_escapes_cell_markup)
{
    auto out = Vision::NormalizeOcr(
        "tencent",
        {{"Response",
          {{"TableDetections",
            json::array(
                {{{"Cells",
                   json::array(
                       {{{"RowTl", 0}, {"ColTl", 0}, {"RowBr", 2}, {"ColBr", 3}, {"Text", "<script>&公式"}}})}}})}}}},
        "table");
    const auto text = out.at("text").get<std::string>();
    REQUIRE(text.find("rowspan=\"2\"") != std::string::npos);
    REQUIRE(text.find("colspan=\"3\"") != std::string::npos);
    REQUIRE(text.find("&lt;script&gt;&amp;公式") != std::string::npos);
    REQUIRE(text.find("<script>") == std::string::npos);
}
TEST_CASE(vision_empty_result_retains_original_response_for_editing)
{
    auto out = Vision::NormalizeOcr("shiliu", {{"data", {{"content", json::array()}}}}, "text");
    REQUIRE(out.at("text") == "");
    REQUIRE(out.contains("raw"));
    REQUIRE(out.contains("warning"));
}
TEST_CASE(vision_base64_binary_roundtrip_and_subtitle_clock)
{
    std::string bytes({'a', '\0', static_cast<char>(255), 'b'});
    REQUIRE(Vision::Unbase64(Vision::Base64(bytes)) == bytes);
    REQUIRE(Vision::SubtitleTime(3661.125) == "01:01:01,125");
    REQUIRE(Vision::SubtitleTime(-1) == "00:00:00,000");
    REQUIRE(Vision::SubtitleTime(std::numeric_limits<double>::infinity()) == "00:00:00,000");
    REQUIRE(VisionConfig::Defaults().at("provider") == "windows");
    REQUIRE(VisionConfig::Defaults().at("translation_provider") == "dictionary");
}
TEST_CASE(personal_dictionary_is_scoped_toggleable_and_invalidates_cache)
{
    const std::string id = "test-v0916-app-dictionary";
    struct Cleanup
    {
        std::string id;
        ~Cleanup()
        {
            PersonalLibrary::Handle({{"action", "remove"}, {"section", "rules"}, {"id", id}}, nullptr);
        }
    } cleanup{id};
    json rule = {{"action", "putRule"},     {"id", id},
                 {"name", "隔离测试"},      {"application", "FixtureEditor.exe"},
                 {"code", "isolationtest"}, {"text", "应用专用词"},
                 {"enabled", true}};
    REQUIRE(PersonalLibrary::Handle(rule, nullptr).at("ok") == true);
    REQUIRE(PersonalLibrary::AppWords("fixtureeditor.exe", "isolationtest") == std::vector<std::string>{"应用专用词"});
    REQUIRE(PersonalLibrary::AppWords("other.exe", "isolationtest").empty());
    REQUIRE(PersonalLibrary::AppWords("fixtureeditor.exe", "different").empty());
    rule["enabled"] = false;
    REQUIRE(PersonalLibrary::Handle(rule, nullptr).at("ok") == true);
    REQUIRE(PersonalLibrary::AppWords("fixtureeditor.exe", "isolationtest").empty());
    rule["application"] = "../bad.exe";
    REQUIRE(PersonalLibrary::Handle(rule, nullptr).at("ok") == false);
}
