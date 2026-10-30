#include "window/candidate_view_model.h"
#include "window/ui_backend_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(candidate_view_escapes_text_and_translation_without_turning_them_into_markup)
{
    CandidateViewItem item;
    item.text = "<&,\"";
    item.annotation = " AB";
    item.badge = " cloud";
    item.translation = "<gloss>,";
    item.fixed_position = true;
    REQUIRE_EQ(CandidateViewHtml(item),
               std::string("<span style=\"color:#379AD3\">&lt;&amp;\xEF\x80\x80&quot; AB cloud</span>"
                           "<span class=\"cand-translation\">&lt;gloss&gt;\xEF\x80\x80</span>"));
    REQUIRE_EQ(item.text, std::string("<&,\""));
}

TEST_CASE(fixed_position_candidates_get_pin_badge_visible_in_both_backends)
{
    CandidateViewItem item;
    item.text = "你好";
    item.badge = " cloud";
    item.fixed_position = true;
    ApplyFixedPositionBadge(item, true, "paperclip");
    // 追加而非覆盖：徽标并存时顺序稳定；WebView2 侧固定词条整体落在蓝色 span 内。
    REQUIRE_EQ(item.badge, std::string(" cloud \xF0\x9F\x93\x8E"));
    REQUIRE_EQ(CandidateViewHtml(item),
               std::string("<span style=\"color:#379AD3\">你好 cloud \xF0\x9F\x93\x8E</span>"));

    // 样式枚举逐个映射，存储的是名字而不是字面 emoji
    CandidateViewItem pinned;
    pinned.text = "钉";
    pinned.fixed_position = true;
    ApplyFixedPositionBadge(pinned, true, "pushpin");
    REQUIRE_EQ(pinned.badge, std::string(" \xF0\x9F\x93\x8C"));
    CandidateViewItem dotted;
    dotted.text = "点";
    dotted.fixed_position = true;
    ApplyFixedPositionBadge(dotted, true, "dot");
    REQUIRE_EQ(dotted.badge, std::string(" \xC2\xB7"));

    // 开关关闭：不加徽标，但整条仍走蓝色 span（固定状态本身还有颜色提示）
    CandidateViewItem hidden;
    hidden.text = "你好";
    hidden.fixed_position = true;
    ApplyFixedPositionBadge(hidden, false, "paperclip");
    REQUIRE(hidden.badge.empty());
    REQUIRE_EQ(CandidateViewHtml(hidden), std::string("<span style=\"color:#379AD3\">你好</span>"));

    // 未列入白名单的样式不加徽标，手改 TOML 写错也不会渲染出畸形标记
    CandidateViewItem bogus;
    bogus.text = "你好";
    bogus.fixed_position = true;
    ApplyFixedPositionBadge(bogus, true, "smiley");
    REQUIRE(bogus.badge.empty());

    // 未固定的词条不加徽标。
    CandidateViewItem plain;
    plain.text = "世界";
    ApplyFixedPositionBadge(plain, true, "paperclip");
    REQUIRE(plain.badge.empty());
    REQUIRE_EQ(CandidateViewHtml(plain), std::string("世界"));
}

TEST_CASE(ui_backend_policy_keeps_settings_web_and_small_windows_native_by_default)
{
    using namespace UiBackendPolicy;
    for (auto surface : {Surface::Candidate, Surface::Toolbar, Surface::Menu})
    {
        REQUIRE(Resolve(surface, "") == Backend::Native);
        REQUIRE(Resolve(surface, "sciter") == Backend::Native);
        REQUIRE(Resolve(surface, "webview2") == Backend::WebView2);
    }
    REQUIRE(Resolve(Surface::Settings, "d2d") == Backend::WebView2);
    REQUIRE(!IsSupported("sciter"));
    REQUIRE(!IsSupported("unknown"));
    REQUIRE(IsSupported("d2d"));
}

TEST_CASE(candidate_gloss_runs_separate_pos_familiarity_and_more_senses)
{
    CandidateViewItem item;
    item.translation = "int. hello; int. hi; greetings";
    DecorateCandidateTranslation(item, [](const std::string &word) { return word == "hi"; });
    REQUIRE_EQ(item.translation_runs.size(), size_t{6});
    REQUIRE(item.translation_runs[0].muted);
    REQUIRE(item.translation_runs[1].fresh);
    REQUIRE_EQ(item.translation_runs[1].text, "hello");
    REQUIRE(!item.translation_runs[4].fresh);
    REQUIRE_EQ(item.translation_runs.back().text, " · +1");
    REQUIRE_EQ(item.translation, "int. hello; int. hi; greetings");
    REQUIRE(CandidateViewHtml(item).find("color:#ed9409") != std::string::npos);
}
