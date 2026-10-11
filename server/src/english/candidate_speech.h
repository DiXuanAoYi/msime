#pragma once
#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/candidate_translation_policy.h"
#include "utils/common_utils.h"
#include "speech_service.h"
#include <wrl/client.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#pragma comment(lib, "sapi.lib")

namespace CandidateSpeech
{
inline std::string Text(const CandidateViewItem &item, bool translation)
{
    if (!translation)
        return item.text;
    auto senses = FanyImeIpc::SplitTranslationGloss(item.translation);
    std::string text;
    for (size_t i = 0; i < (std::min)(size_t{2}, senses.size()); ++i)
    {
        if (!text.empty())
            text += ". ";
        text += senses[i];
    }
    return text;
}
inline void Speak(size_t index, bool translation)
{
    const auto page = Global::LoadCandidatePageSnapshot();
    if (!page || index >= page->page_views.size())
        return;
    auto text = Text(page->page_views[index], translation);
    if (text.empty() || text.size() > 2000)
        return;
    std::string lang = "zh";
    const auto &source = page->page_views[index].text;
    const bool sourceAscii = std::all_of(source.begin(), source.end(), [](unsigned char c) { return c < 128; });
    if (!translation && sourceAscii)
        lang = "en";
    else if (translation && !sourceAscii)
    {
        const auto target = GetConfiguredTencentTmt().target_language;
        if (target == "en")
            lang = "en";
        else if (target == "ja")
            lang = "ja";
        else if (target == "es")
            lang = "es";
        else if (target == "vi")
            lang = "vi";
        else if (target == "fr")
            lang = "fr";
        else if (target == "de")
            lang = "de";
        else if (target == "ru")
            lang = "ru";
        else if (target == "ko")
            lang = "ko";
    }
    Speech::Speak(std::move(text), lang);
}
} // namespace CandidateSpeech
