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
#include <filesystem>
using nlohmann::json;
int RunVisionCodeJsonFixtureProbe(const char *path, const char *expected)
{
    try
    {
        const std::filesystem::path fixture(path);
        const auto expectedPath =
            expected ? std::filesystem::path(expected) : fixture.parent_path() / "v0917-ocr-code.expected.txt";
        const auto read = [](const std::filesystem::path &p) {
            if (!std::filesystem::is_regular_file(p) || std::filesystem::file_size(p) > 8 * 1024 * 1024)
                throw std::runtime_error("code JSON/expected fixture missing or exceeds 8 MB");
            std::ifstream file(p, std::ios::binary);
            if (!file)
                throw std::runtime_error("could not read code JSON/expected fixture");
            return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        };
        const auto canonical = [](const std::string &input) {
            std::string out;
            for (size_t i = input.rfind("\xef\xbb\xbf", 0) == 0 ? 3 : 0; i < input.size(); ++i)
            {
                if (input[i] == '\r')
                {
                    out += '\n';
                    if (i + 1 < input.size() && input[i + 1] == '\n')
                        ++i;
                }
                else
                    out += input[i];
            }
            // Only normalize file line endings; meaningful code whitespace is compared exactly.
            while (!out.empty() && out.back() == '\n')
                out.pop_back();
            return out;
        };
        const auto model = json::parse(read(fixture));
        auto normalized = Vision::NormalizeOcr("local", model, "code");
        const bool estimated =
            normalized.contains("code_indentation") && normalized.at("code_indentation").value("estimated", false);
        const bool matches = canonical(normalized.at("text").get<std::string>()) == canonical(read(expectedPath));
        json report = {{"ok", estimated && matches},
                       {"source", fixture.filename().u8string()},
                       {"matches_expected", matches},
                       {"ocr", normalized.at("text")},
                       {"original_ocr", normalized.value("ocr_text", std::string())},
                       {"warning", normalized.value("warning", std::string())},
                       {"raw", normalized}};
        std::cout << report.dump() << std::endl;
        if (!estimated || !matches)
        {
            std::cerr << (!estimated ? "native code indentation correctly refused unreliable geometry"
                                     : "native normalized code does not exactly match the expected fixture")
                      << std::endl;
            return 1;
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
int RunVisionFixtureProbe(const char *path, bool code)
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
        Vision::Handle({{"action", "recognize"}, {"task", code ? "code" : "text"}}, nullptr);
        auto recognized = wait();
        auto text = recognized.value("text", "");
        if (text.find("12345") == std::string::npos)
            throw std::runtime_error("OCR did not recognize fixture digits");
        if (code)
        {
            const auto &details = recognized.at("raw");
            if (!details.contains("code_indentation") || !details.at("code_indentation").value("estimated", false) ||
                details.at("code_indentation").value("changed_lines", 0) < 2)
                throw std::runtime_error("code fixture did not provide reliable indentation geometry");
        }
        Vision::Handle({{"action", "translate"}, {"text", "hello"}, {"source", "en"}, {"target", "zh"}}, nullptr);
        auto translated = wait();
        json report = {{"ocr", text},
                       {"translation", translated.value("translation", "")},
                       {"width", recognized.value("width", 0)},
                       {"height", recognized.value("height", 0)}};
        if (code)
        {
            report["raw"] = recognized.at("raw");
            report["warning"] = recognized.value("warning", "");
        }
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
    auto out = Vision::NormalizeOcr("shiliu",
                                    {{"data",
                                      {{"content", json::array({{{"text", "函数 f(x)"},
                                                                 {"prob", 0.9},
                                                                 {"keypoints", json::array({{{"x", 2}, {"y", 3}},
                                                                                            {{"x", 42}, {"y", 3}},
                                                                                            {{"x", 42}, {"y", 23}},
                                                                                            {{"x", 2}, {"y", 23}}})}},
                                                                {{"text", "return x < 2"}}})}}}},
                                    "code");
    REQUIRE(out.at("text") == "函数 f(x)\nreturn x < 2");
    REQUIRE(out.at("blocks").size() == 2);
    REQUIRE(out.at("blocks")[0].at("box")[0][0] == 2);
    REQUIRE(out.at("blocks")[0].at("kind") == "code");
    REQUIRE(out.contains("raw"));
}
TEST_CASE(vision_baidu_formula_uses_current_doc_analysis_fields_without_duplicating_fused_text)
{
    const json response = {
        {"formula_result",
         json::array({{{"form_words", R"(x = \frac{1}{n-1})"},
                       {"form_location", {{"left", 97}, {"top", 0}, {"width", 151}, {"height", 77}}}}})},
        {"words_result", json::array({{{"words", R"(其中 x = \frac{1}{n-1} 是结果。)"}}})}};
    auto formula = Vision::NormalizeOcr("baidu", response, "formula");
    REQUIRE(formula.at("text") == R"(x = \frac{1}{n-1})");
    REQUIRE(formula.at("format") == "latex");
    REQUIRE(formula.at("blocks").size() == 1);
    REQUIRE(formula.at("blocks")[0].at("box")[2] == json::array({248, 77}));
    REQUIRE(formula.at("blocks")[0].at("kind") == "formula");
    auto document = Vision::NormalizeOcr("baidu", response, "document");
    REQUIRE(document.at("text") == R"(其中 x = \frac{1}{n-1} 是结果。)");
    REQUIRE(document.at("blocks").size() == 1);
}
TEST_CASE(vision_ocr_geometry_rejects_invalid_boxes_without_losing_original_text)
{
    auto result =
        Vision::NormalizeOcr("local",
                             {{"text", "    unit_price = count * 2"},
                              {"format", "text"},
                              {"blocks", json::array({{{"text", "    unit_price = count * 2"},
                                                       {"box", json::array({{0, 0}, {50, 0}, {50, -1}, {0, 10}})}}})}},
                             "code");
    REQUIRE(result.at("text") == "    unit_price = count * 2");
    REQUIRE(result.at("blocks")[0].at("box").empty());
    REQUIRE(result.at("blocks")[0].at("kind") == "code");
}
TEST_CASE(vision_baidu_table_preserves_headers_spans_footers_and_geometry)
{
    auto result = Vision::NormalizeOcr(
        "baidu",
        {{"tables_result",
          json::array(
              {{{"header", json::array({{{"words", "<季度报表>"}}})},
                {"footer", json::array({{{"words", "合计 & 备注"}}})},
                {"table_location", json::array({{10, 20}, {110, 20}, {110, 80}, {10, 80}})},
                {"body",
                 json::array(
                     {{{"row_start", 0}, {"row_end", 2}, {"col_start", 0}, {"col_end", 1}, {"words", "项目"}},
                      {{"row_start", 0}, {"row_end", 1}, {"col_start", 1}, {"col_end", 3}, {"words", "本期数"}}})}}})}},
        "table");
    const auto text = result.at("text").get<std::string>();
    REQUIRE(text.find("&lt;季度报表&gt;") != std::string::npos);
    REQUIRE(text.find("rowspan=\"2\"") != std::string::npos);
    REQUIRE(text.find("colspan=\"2\"") != std::string::npos);
    REQUIRE(text.find("合计 &amp; 备注") != std::string::npos);
    REQUIRE(result.at("blocks")[1].at("box")[0] == json::array({10, 20}));
    REQUIRE(result.at("blocks")[1].at("region") == true);
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
