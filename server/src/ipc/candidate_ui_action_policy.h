#pragma once
#include "engine/core/word_item.h"

namespace FanyImeIpc
{
inline constexpr int kMinCandidateUiOneBasedIndex = 1;
inline constexpr int kMaxCandidateUiOneBasedIndex = 10;

constexpr bool IsValidCandidateUiOneBasedIndex(int index)
{
    return index >= kMinCandidateUiOneBasedIndex && index <= kMaxCandidateUiOneBasedIndex;
}

constexpr bool SupportsCandidateDictionaryMutation(CandidateSource source)
{
    return source != CandidateSource::QuickPhrase && source != CandidateSource::Emoji &&
           source != CandidateSource::Kaomoji && source != CandidateSource::Generated &&
           source != CandidateSource::AiCompletion && source != CandidateSource::AiQuestion;
}
} // namespace FanyImeIpc
