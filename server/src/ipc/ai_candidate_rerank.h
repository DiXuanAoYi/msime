#pragma once

#include "engine/core/word_item.h"
#include <algorithm>
#include <string>
#include <vector>

namespace FanyImeIpc
{
// Reranking only permutes the original whole-input rows sent to the local
// model. Preserve every row's source/reading/domain metadata, pinned positions
// and all intervening quick phrases or asynchronous candidates. Model output
// cannot invent a candidate or promote a candidate for a shorter input.
inline bool ApplyStableAiRerank(std::vector<WordItem> &items, const std::vector<std::string> &original_words,
                                const std::vector<std::string> &ranked_words)
{
    std::vector<size_t> positions;
    std::vector<WordItem> rows;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto &item = items[i];
        if (item.fixed_position != 0 || item.source == CandidateSource::QuickPhrase ||
            item.source == CandidateSource::EnglishDictionary || item.source == CandidateSource::CloudSuggestion ||
            item.source == CandidateSource::AiSuggestion || item.source == CandidateSource::Emoji ||
            item.source == CandidateSource::Kaomoji || item.source == CandidateSource::DateTime ||
            std::find(original_words.begin(), original_words.end(), item.word) == original_words.end())
            continue;
        positions.push_back(i);
        rows.push_back(item);
    }
    std::vector<WordItem> reordered;
    for (const auto &word : ranked_words)
    {
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto &item) { return item.word == word; });
        if (row != rows.end())
        {
            reordered.push_back(std::move(*row));
            rows.erase(row);
        }
    }
    reordered.insert(reordered.end(), rows.begin(), rows.end());
    bool changed = false;
    for (size_t i = 0; i < positions.size(); ++i)
    {
        changed = changed || items[positions[i]].word != reordered[i].word;
        items[positions[i]] = std::move(reordered[i]);
    }
    return changed;
}
} // namespace FanyImeIpc
