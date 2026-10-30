#pragma once

#include <string>
#include <vector>
#include <functional>
#include "ipc/candidate_translation_policy.h"

struct TranslationDisplayRun
{
    enum class Kind
    {
        Text,
        PartOfSpeech,
        Separator
    };
    std::string text;
    bool muted = false;
    bool fresh = false;
    Kind kind = Kind::Text;
};

// Built once on the candidate worker. Renderers only adapt this presentation data;
// they do not query dictionaries, derive helpcodes or decide which source gets a badge.
struct CandidateViewItem
{
    std::string text;
    std::string annotation;
    // 来源/状态徽标（☁️/🤖、固定排位 📎）。纯展示字段：D2D（text+badge 拼接）与
    // WebView2（CandidateViewHtml）都只把它渲染进词条文本，page_words 与上屏内容不含它。
    std::string badge;
    std::string translation;
    bool fixed_position = false;
    std::vector<TranslationDisplayRun> translation_runs;
    std::string reading;
    std::vector<std::string> input_domains;
};

inline void ApplyInputDomainBadge(CandidateViewItem &item)
{
    const std::vector<std::pair<std::string, std::string>> names{
        {"animals", "动物"},        {"automotive", "汽车"}, {"finance", "财经"}, {"food", "饮食"},
        {"historical", "历史人物"}, {"idioms", "成语"},     {"it", "IT"},        {"law", "法律"},
        {"medicine", "医学"},       {"places", "地名"},     {"poetry", "诗词"}};
    std::string label;
    size_t displayed = 0;
    for (const auto &id : item.input_domains)
    {
        for (const auto &[key, name] : names)
            if (key == id && displayed < 2)
            {
                if (!label.empty())
                    label += "·";
                label += name;
                ++displayed;
                break;
            }
    }
    if (!label.empty())
    {
        if (item.input_domains.size() > displayed)
            label += "+" + std::to_string(item.input_domains.size() - displayed);
        item.badge += " 〔" + label + "〕";
    }
}

// Show two senses like Qingjian. The complete gloss remains available through
// Ctrl+Enter, and only the displayed senses count as vocabulary exposure.
inline void DecorateCandidateTranslation(CandidateViewItem &item,
                                         const std::function<bool(const std::string &)> &is_familiar)
{
    item.translation_runs.clear();
    const auto output = FanyImeIpc::SplitTranslationGloss(item.translation);
    size_t start = 0, count = 0;
    while (start < item.translation.size() && count < 2)
    {
        const auto ascii = item.translation.find(';', start);
        const auto full = item.translation.find("；", start);
        const auto end = (std::min)(ascii, full);
        auto sense = item.translation.substr(start, end == std::string::npos ? end : end - start);
        const auto first = sense.find_first_not_of(" \t\r\n");
        if (first != std::string::npos)
        {
            sense = sense.substr(first, sense.find_last_not_of(" \t\r\n") - first + 1);
            if (count)
                item.translation_runs.push_back({" · ", true, false, TranslationDisplayRun::Kind::Separator});
            for (const auto pos : {"proper n. ", "n. ", "v. ", "vt. ", "vi. ", "adj. ", "adv. ", "prep. ", "pron. ",
                                   "conj. ", "int. ", "num. ", "aux. ", "det. ", "part. ", "phr. ", "m. "})
                if (sense.rfind(pos, 0) == 0)
                {
                    item.translation_runs.push_back({pos, true, false, TranslationDisplayRun::Kind::PartOfSpeech});
                    sense.erase(0, std::char_traits<char>::length(pos));
                    break;
                }
            item.translation_runs.push_back({sense, false, count < output.size() && !is_familiar(output[count])});
            ++count;
        }
        if (end == std::string::npos)
            break;
        start = end + (end == full ? std::string("；").size() : 1);
    }
    if (output.size() > count)
        item.translation_runs.push_back(
            {" · +" + std::to_string(output.size() - count), true, false, TranslationDisplayRun::Kind::Separator});
}

inline std::string EscapeCandidateViewHtml(const std::string &text)
{
    std::string result;
    for (size_t index = 0; index < text.size(); ++index)
    {
        const char ch = text[index];
        // The old TSF display transport uses this code point as a comma escape. Preserve real PUA text as HTML.
        if (text.compare(index, 3, "\xEF\x80\x80") == 0)
        {
            result += "&#xF000;";
            index += 2;
            continue;
        }
        switch (ch)
        {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        // The existing WebView transport splits on commas before restoring U+F000.
        case ',':
            result += "\xEF\x80\x80";
            break;
        default:
            result += ch;
            break;
        }
    }
    return result;
}

// 固定排位的词条补固定徽标：D2D 原生候选窗原本不显示任何固定标记，WebView2 侧也只有
// 整条变蓝这一种弱提示。复用 badge 的既有展示路径而非新增字段——D2D 拼 text+badge，
// WebView2 经 CandidateViewHtml 把 badge 转义进主文本 run（固定词条落在蓝色 span 内）。
// show/style 由调用方从配置求值后传入：本头是纯展示层，不读配置。样式默认 📎 而非 📌——
// 实心红钉在蓝底词条上像贴了张贴纸，回形针同样是「固定」语义但安静得多。
// 未列出的样式不加徽标，与配置层白名单同宽，手改 TOML 写错也不会渲染出畸形标记。
inline void ApplyFixedPositionBadge(CandidateViewItem &view, bool show, const std::string &style)
{
    if (!view.fixed_position || !show)
        return;
    if (style == "pushpin")
        view.badge += " \xF0\x9F\x93\x8C"; // 📌
    else if (style == "dot")
        view.badge += " \xC2\xB7"; // ·
    else if (style == "paperclip")
        view.badge += " \xF0\x9F\x93\x8E"; // 📎
}

inline std::string CandidateViewHtml(const CandidateViewItem &item)
{
    std::string html = EscapeCandidateViewHtml(item.text + item.annotation + item.badge);
    if (item.fixed_position)
        html = "<span style=\"color:#379AD3\">" + html + "</span>";
    if (!item.reading.empty())
        html = "<span class=\"cand-reading\" "
               "style=\"display:inline-flex;flex-direction:column;vertical-align:middle\"><span "
               "style=\"font-size:.55em;line-height:1.35;opacity:.8\">" +
               EscapeCandidateViewHtml(item.reading) + "</span><span>" + html + "</span></span>";
    if (!item.translation_runs.empty())
    {
        html += "<span class=\"cand-translation\" style=\"opacity:1\">";
        for (const auto &run : item.translation_runs)
        {
            const auto style = run.fresh ? "color:#ed9409" : run.muted ? "opacity:.58" : "opacity:.75";
            const std::string cls = run.kind == TranslationDisplayRun::Kind::PartOfSpeech ? "cand-pos"
                                    : run.kind == TranslationDisplayRun::Kind::Separator  ? "cand-sep"
                                                                                          : "cand-sense";
            html += "<span class=\"" + cls + "\" style=\"" + std::string(style) + "\">" +
                    EscapeCandidateViewHtml(run.text) + "</span>";
        }
        html += "</span>";
    }
    else if (!item.translation.empty())
        html += "<span class=\"cand-translation\">" + EscapeCandidateViewHtml(item.translation) + "</span>";
    return html;
}
