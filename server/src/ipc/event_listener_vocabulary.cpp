#include "ipc/event_listener_internal.h"
#include "english/vocabulary_pending.h"
#include "english/vocabulary_service.h"
#include "ipc/candidate_translation_policy.h"
#include "config/ime_config.h"
#include "global/globals.h"
#include "utils/common_utils.h"
#include <algorithm>

namespace event_listener_detail
{
namespace
{
std::optional<Vocabulary::Batch> staged;
Vocabulary::Pending pending;
std::string translation_page_language;
bool IsAsciiWord(const std::string &text)
{
    return !text.empty() && std::any_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }) && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 128; });
}
} // namespace
void BeginVocabularySelection()
{
    staged.reset();
    if (!GetConfiguredVocabularyEnabled())
        pending.Reset();
}
void ResetVocabularySelections()
{
    staged.reset();
    pending.Reset();
}
void BeginVocabularyTranslationPage(int index)
{
    const auto &views = Global::candidate_ui.page_views;
    translation_page_language =
        index >= 0 && static_cast<size_t>(index) < views.size() && !IsAsciiWord(views[index].text)
            ? GetConfiguredTencentTmt().target_language
            : "";
}
void CaptureVocabularySelection(int index, bool use_translation)
{
    if (!FanyImeIpc::InputLearningAllowed())
        return;
    if (!GetConfiguredVocabularyEnabled() || !GetConfiguredCandidateTranslationsEnabled() || IsUiLessMode() ||
        !Global::candidate_window_rendered_visible.load() ||
        Global::candidate_page_generation.load() != Global::rendered_candidate_page_generation.load())
        return;
    const auto &ui = Global::candidate_ui;
    if (index < 0 || static_cast<size_t>(index) >= ui.page_views.size())
        return;
    Vocabulary::Batch batch;
    if (use_translation && !g_translation_candidates_active && IsAsciiWord(ui.page_views[index].text))
        return;
    batch.language =
        g_translation_candidates_active ? translation_page_language : GetConfiguredTencentTmt().target_language;
    if (batch.language.empty())
        return;
    batch.timestamp_ms = Vocabulary::NowMs();
    for (size_t i = 0; i < ui.page_views.size(); ++i)
    {
        const auto &view = ui.page_views[i];
        // Track target-language glosses. English->Chinese explanations are not foreign vocabulary.
        if (!g_translation_candidates_active && IsAsciiWord(view.text))
            continue;
        auto words = g_translation_candidates_active ? std::vector<std::string>{view.text}
                                                     : FanyImeIpc::SplitTranslationGloss(view.translation);
        if (!g_translation_candidates_active && !view.translation_runs.empty() && words.size() > 2)
            words.resize(2);
        batch.seen.insert(batch.seen.end(), words.begin(), words.end());
        if (i == static_cast<size_t>(index))
        {
            batch.selected = words;
            if (use_translation)
                batch.used = words;
        }
    }
    if (!batch.seen.empty())
        staged = std::move(batch);
}
void ArmVocabularySelection(uint64_t client, uint64_t epoch, uint64_t request)
{
    if (staged && GetConfiguredVocabularyEnabled() && request && ClientNegotiatedVocabularyCommit(client))
        pending.Arm({client, epoch, request}, std::move(*staged), GetTickCount64());
    staged.reset();
}
void ConfirmVocabularySelection(uint64_t client, uint64_t epoch, uint64_t request)
{
    if (!FanyImeIpc::InputLearningAllowed())
    {
        ResetVocabularySelections();
        return;
    }
    if (!GetConfiguredVocabularyEnabled())
    {
        ResetVocabularySelections();
        return;
    }
    if (auto batch = pending.Confirm({client, epoch, request}, GetTickCount64()))
        Vocabulary::Enqueue(std::move(*batch));
}
} // namespace event_listener_detail
