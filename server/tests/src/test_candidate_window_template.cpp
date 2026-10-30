#include "tests/includes/test_framework.h"
#include "webview2/candidate_window_template.h"
#include "window/candidate_view_model.h"

TEST_CASE(candidate_structured_slots_preserve_commas_private_glyphs_and_unicode)
{
    const std::wstring preedit = L"nǐ,hǎo\uF000";
    const std::vector<std::wstring> items{L"one,two", L"\uF000", L"你好😀", L"", L"five"};
    const auto encoded = BuildCandidateTemplatePayload(preedit, items);
    const auto decoded = SplitCandidateTemplatePayload(encoded);
    REQUIRE_EQ(decoded.size(), items.size() + 1);
    REQUIRE_EQ(decoded[0], preedit);
    for (size_t index = 0; index < items.size(); ++index)
        REQUIRE_EQ(decoded[index + 1], items[index]);
    REQUIRE_EQ(EscapeCandidateViewHtml("\xEF\x80\x80,"), std::string("&#xF000;\xEF\x80\x80"));
}

TEST_CASE(candidate_window_template_renders_ninth_candidate)
{
    const std::wstring templ = L"<!--0Anchor-->{0}<!--1Anchor-->{1}<!--8Anchor-->{8}<!--9Anchor-->{9}";
    const std::wstring text = L"preedit,one,two,three,four,five,six,seven,eight,nine";

    const std::wstring result = InflateCandidateTemplate(templ, text);

    REQUIRE(result.find(L"nine") != std::wstring::npos);
    REQUIRE(result.find(L"<!--9Anchor-->") != std::wstring::npos);
}

TEST_CASE(candidate_window_template_trims_after_eighth_candidate)
{
    const std::wstring templ = L"<!--0Anchor-->{0}<!--1Anchor-->{1}<!--8Anchor-->{8}<!--9Anchor-->{9}";
    const std::wstring text = L"preedit,one,two,three,four,five,six,seven,eight";

    const std::wstring result = InflateCandidateTemplate(templ, text);

    REQUIRE(result.find(L"eight") != std::wstring::npos);
    REQUIRE(result.find(L"{9}") == std::wstring::npos);
}
