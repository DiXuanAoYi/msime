#include "tests/includes/test_framework.h"
#include "settings/vision_capture.h"
#include "settings/vision_subtitles.h"
#include "settings/vision_protocol.h"
#include <limits>
#include <stdexcept>
#include <future>
#include <atomic>
#include <thread>
#include <chrono>
using nlohmann::json;
TEST_CASE(vision_overlay_retains_physical_box_coordinates_and_clips_at_image_edges)
{
    auto blocks = Vision::OverlayBlocksForImage(
        {{"text", "all"},
         {"blocks", json::array({{{"text", "line"}, {"box", {{-2, 4}, {25, 4}, {25, 18}, {-2, 18}}}},
                                 {{"text", "tile"},
                                  {"region", true},
                                  {"box", json::array({{{"x", 50}, {"y", 20}},
                                                       {{"x", 120}, {"y", 20}},
                                                       {{"x", 120}, {"y", 80}},
                                                       {{"x", 50}, {"y", 80}}})}},
                                 {{"text", "outside"}, {"box", {{200, 200}, {220, 200}, {220, 220}, {200, 220}}}}})}},
        100, 60);
    REQUIRE(blocks.size() == 2);
    REQUIRE(blocks[0].bounds.left == 0);
    REQUIRE(blocks[0].bounds.top == 4);
    REQUIRE(blocks[0].bounds.right == 25);
    REQUIRE(blocks[1].bounds.left == 50);
    REQUIRE(blocks[1].bounds.right == 100);
    REQUIRE(blocks[1].bounds.bottom == 60);
    REQUIRE(blocks[1].region);
}
TEST_CASE(vision_overlay_rejects_invalid_geometry_and_marks_region_fallback_explicitly)
{
    auto blocks = Vision::OverlayBlocksForImage(
        {{"text", "fallback"}, {"blocks", json::array({{{"text", "bad"}, {"box", {{1, 1}, {1, 1}, {1, 1}, {1, 1}}}}})}},
        1200, 480);
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].region);
    REQUIRE(blocks[0].bounds.right == 1200);
    REQUIRE(blocks[0].source == "fallback");
    REQUIRE(Vision::OverlayBlocksForImage({{"text", ""}}, 1200, 480).empty());
}
TEST_CASE(vision_table_overlay_displays_cells_without_executing_or_destroying_original_html)
{
    const std::string table = "<table><tr><th>A</th><th>B</th></tr><tr><td>unit_price &lt; "
                              "2</td><td>&lt;script&gt;&amp;公式</td></tr></table>";
    REQUIRE(Vision::OverlayDisplayText(table, "html") == "A\tB\nunit_price < 2\t<script>&公式");
    REQUIRE(table.find("<table>") == 0);
    REQUIRE(Vision::OverlayDisplayText("if (x < 2) return x;", "text") == "if (x < 2) return x;");
    REQUIRE(Vision::OverlayDisplayText("\\frac{1}{2}", "latex") == "\\frac{1}{2}");
}
TEST_CASE(vision_long_ocr_tiles_cover_the_full_image_at_original_resolution)
{
    auto tiles = Vision::ImageTileRegions(2050, 6100);
    REQUIRE(tiles.front().left == 0);
    REQUIRE(tiles.front().top == 0);
    REQUIRE(tiles.back().right == 2050);
    REQUIRE(tiles.back().bottom == 6100);
    for (const auto &tile : tiles)
    {
        REQUIRE(tile.right - tile.left <= 2048);
        REQUIRE(tile.bottom - tile.top <= 2048);
    }
    REQUIRE(Vision::ImageTileRegions(1200, 480).size() == 1);
    bool rejected = false;
    try
    {
        Vision::ImageTileRegions(10000, 10000);
    }
    catch (const std::runtime_error &)
    {
        rejected = true;
    }
    REQUIRE(rejected);
}
TEST_CASE(vision_subtitle_import_preserves_multiline_code_and_time_axis)
{
    auto lines =
        Vision::ParseSubtitleDocument("1\r\n00:00:01,125 --> 00:00:03,500\r\nunit_price = count * 2\r\n第二行\r\n\r\n"
                                      "2\r\n00:00:04,000 --> 00:00:05,000\r\n你好\r\n");
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0]["start"] == 1.125);
    REQUIRE(lines[0]["end"] == 3.5);
    REQUIRE(lines[0]["text"] == "unit_price = count * 2\n第二行");
    auto srt = Vision::SubtitleDocumentToSrt(lines);
    REQUIRE(srt.find("00:00:01,125 --> 00:00:03,500") != std::string::npos);
    REQUIRE(srt.find("unit_price = count * 2\n第二行") != std::string::npos);
}
TEST_CASE(vision_webvtt_import_keeps_timing_and_skips_notes)
{
    auto lines = Vision::ParseSubtitleDocument(
        "WEBVTT\nKind: captions\nLanguage: en\n\nNOTE comment\nnot a cue\n\n"
        "named-cue\n00:01.500 --> 00:04.750 align:start position:10%\nHello <b>world</b>\n\n"
        "00:06.000 --> 00:07.000\nNext\n");
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0]["start"] == 1.5);
    REQUIRE(lines[0]["settings"] == " align:start position:10%");
    REQUIRE(lines[0]["text"] == "Hello <b>world</b>");
    bool rejected = false;
    try
    {
        Vision::ParseSubtitleDocument("1\n00:00:05,000 --> 00:00:01,000\nwrong\n");
    }
    catch (const std::runtime_error &)
    {
        rejected = true;
    }
    REQUIRE(rejected);
}
TEST_CASE(vision_subtitle_translation_caches_repeats_and_retains_partial_results_on_cancel)
{
    auto lines = Vision::ParseSubtitleDocument(
        "1\n00:00:01,000 --> 00:00:02,000\nhello\n\n2\n00:00:03,000 --> 00:00:04,000\nhello\n");
    int calls = 0;
    auto translated = Vision::TranslateSubtitleSegments(
        lines,
        [&](const std::string &) {
            ++calls;
            return "你好";
        },
        [] { return false; });
    REQUIRE(calls == 1);
    REQUIRE(translated[1]["translation"] == "你好");
    REQUIRE(translated[1]["start"] == lines[1]["start"]);
    const auto srt = Vision::SubtitleDocumentToSrt(translated);
    REQUIRE(srt.find("hello\n你好") != std::string::npos);
    auto cancelled = Vision::TranslateSubtitleSegments(
        lines,
        [&](const std::string &) {
            ++calls;
            return "bad";
        },
        [] { return true; });
    REQUIRE(cancelled == lines);
    REQUIRE(calls == 1);
    bool stopped = false;
    auto partial = Vision::TranslateSubtitleSegments(
        lines, [](const std::string &) { return "第一段已完成"; }, [&] { return stopped; },
        [&](const json &, const std::string &) { stopped = true; });
    REQUIRE(partial[0]["translation"] == "第一段已完成");
    REQUIRE(partial[1]["translation"] == "");
}
TEST_CASE(vision_batch_translation_maps_explicit_ids_and_rejects_incomplete_or_ambiguous_output)
{
    auto mapped =
        Vision::ParseBlockTranslations(R"({"translations":[{"id":1,"text":"第二行"},{"id":0,"text":"第一行"}]})", 2);
    REQUIRE(mapped[0] == "第一行");
    REQUIRE(mapped[1] == "第二行");
    REQUIRE(Vision::ParseBlockTranslations(
                "```json\n{\"translations\":[{\"id\":0,\"text\":\"保留 LaTeX \\\\frac{1}{2}\"}]}\n```", 1)[0] ==
            "保留 LaTeX \\frac{1}{2}");
    for (const auto &bad :
         {R"({"translations":[{"id":0,"text":"a"}]})", R"({"translations":[{"id":0,"text":"a"},{"id":0,"text":"b"}]})",
          R"({"translations":[{"id":0,"text":"a"},{"id":2,"text":"b"}]})", R"({"translations":["a","b"]})",
          R"({"translations":[{"id":0,"text":"a"},{"id":1,"text":""}]})"})
    {
        bool rejected = false;
        try
        {
            Vision::ParseBlockTranslations(bad, 2);
        }
        catch (const std::runtime_error &)
        {
            rejected = true;
        }
        REQUIRE(rejected);
    }
    auto stopped = Vision::TranslateBlocks({"hello", "world"}, "en", "zh", [] { return true; });
    REQUIRE(stopped.size() == 2);
    REQUIRE(stopped[0].empty());
}
TEST_CASE(vision_code_geometry_reconstructs_relative_monospace_indent_without_repairing_symbols)
{
    const std::vector<std::string> text = {"if ready:", "print(value)", "unit price = count", "}"};
    const std::vector<int> x = {16, 48, 80, 16};
    auto blocks = json::array();
    std::string original;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const int y = int(i) * 24, right = x[i] + int(text[i].size()) * 8;
        blocks.push_back({{"text", text[i]}, {"box", {{x[i], y}, {right, y}, {right, y + 16}, {x[i], y + 16}}}});
        if (i)
            original += '\n';
        original += text[i];
    }
    auto out = Vision::NormalizeOcr("local", {{"text", original}, {"blocks", blocks}, {"format", "text"}}, "code");
    REQUIRE(out["text"] == "if ready:\n    print(value)\n        unit price = count\n}");
    REQUIRE(out["ocr_text"] == original);
    REQUIRE(out["code_indentation"]["estimated"] == true);
    REQUIRE(out["blocks"][2]["ocr_text"] == "unit price = count");
    REQUIRE(out["text"].get<std::string>().find("unit_price") == std::string::npos);
    auto overlay = Vision::OverlayBlocksForImage(out, 1200, 480);
    REQUIRE(overlay[1].source == "print(value)");
    REQUIRE(overlay[1].bounds.left == 48);
    const auto once = out;
    Vision::ReconstructCodeIndentation(out);
    REQUIRE(out == once);
    for (const auto &task : {"text", "table", "formula"})
    {
        auto untouched =
            Vision::NormalizeOcr("local", {{"text", original}, {"blocks", blocks}, {"format", "text"}}, task);
        REQUIRE(untouched["text"] == original);
        REQUIRE(!untouched.contains("code_indentation"));
    }
}
TEST_CASE(vision_code_geometry_does_not_guess_variable_font_or_document_region_indentation)
{
    const std::string source = "abcdefgh\nijklmnop";
    auto unreliable =
        json{{"text", source},
             {"blocks", json::array({{{"text", "abcdefgh"}, {"box", {{0, 0}, {64, 0}, {64, 18}, {0, 18}}}},
                                     {{"text", "ijklmnop"}, {"box", {{32, 24}, {160, 24}, {160, 42}, {32, 42}}}}})}};
    Vision::ReconstructCodeIndentation(unreliable);
    REQUIRE(unreliable["text"] == source);
    REQUIRE(unreliable["code_indentation"]["estimated"] == false);
    REQUIRE(unreliable["warning"].get<std::string>().find("未重建") != std::string::npos);
    auto document = json{{"text", "if ready:\n    print(value)"},
                         {"blocks", json::array({{{"text", "if ready:\n    print(value)"},
                                                  {"region", true},
                                                  {"box", {{0, 0}, {400, 0}, {400, 400}, {0, 400}}}}})}};
    Vision::ReconstructCodeIndentation(document);
    REQUIRE(document["text"] == "if ready:\n    print(value)");
    REQUIRE(document["code_indentation"]["estimated"] == false);
}
TEST_CASE(vision_code_geometry_keeps_complete_text_when_blocks_omit_blank_lines)
{
    auto out = json{
        {"text", "if ready:\n\nprint(value)"},
        {"blocks", json::array({{{"text", "if ready:"}, {"box", {{0, 0}, {72, 0}, {72, 16}, {0, 16}}}},
                                {{"text", "print(value)"}, {"box", {{32, 48}, {128, 48}, {128, 64}, {32, 64}}}}})}};
    Vision::ReconstructCodeIndentation(out);
    REQUIRE(out["text"] == "if ready:\n\nprint(value)");
    REQUIRE(out["code_indentation"]["estimated"] == false);
}
TEST_CASE(vision_subtitle_finish_stops_capture_while_recognition_is_still_blocked)
{
    std::atomic<bool> finish{false};
    std::atomic<int> stops{0};
    std::promise<void> stopped;
    auto completed = stopped.get_future();
    std::thread::id startThread, stopThread;
    {
        Vision::SubtitleCaptureSession session(
            [&] {
                startThread = std::this_thread::get_id();
                return true;
            },
            [&] {
                stopThread = std::this_thread::get_id();
                if (++stops == 1)
                    stopped.set_value();
            },
            [&] { return finish.load(); }, [] { return false; });
        REQUIRE(session.started());
        finish = true;
        // The recognition thread is deliberately occupied, just like an in-flight ASR request.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        REQUIRE(completed.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        REQUIRE(stops == 1);
        REQUIRE(startThread == stopThread);
        REQUIRE(startThread != std::this_thread::get_id());
    }
    REQUIRE(stops == 1);
}
TEST_CASE(vision_subtitle_cancel_and_failed_start_release_device_on_the_same_control_thread)
{
    std::atomic<bool> cancel{false};
    std::promise<void> stopped;
    auto completed = stopped.get_future();
    {
        Vision::SubtitleCaptureSession session([] { return true; }, [&] { stopped.set_value(); }, [] { return false; },
                                               [&] { return cancel.load(); });
        REQUIRE(session.started());
        cancel = true;
        REQUIRE(completed.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    }
    std::atomic<int> stoppedAfterFailure{0};
    {
        Vision::SubtitleCaptureSession failed([] { return false; }, [&] { ++stoppedAfterFailure; },
                                              [] { return false; }, [] { return false; });
        REQUIRE(!failed.started());
    }
    REQUIRE(stoppedAfterFailure == 1);
}
