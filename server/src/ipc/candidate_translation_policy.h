#pragma once

#include <cstdint>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "input_key_policy.h"

namespace FanyImeIpc
{
inline constexpr uint32_t kVirtualKeyReturn = 0x0D;

// Ctrl+Enter commits the translation shown to the right of the highlighted
// candidate instead of the candidate itself. Shift or Alt must not be down:
// those combinations still belong to the application.
constexpr bool IsTranslationCommitKey(uint32_t keycode, uint32_t modifiers_down)
{
    return keycode == kVirtualKeyReturn && (modifiers_down & kKeyModifierMask) == kModifierControl;
}

// english.db stores one gloss string per entry with the senses already joined:
// en->zh uses the fullwidth '；' ("苹果；家伙"), zh->en the ASCII "; " ("string; trail").
// A cloud gloss is a single sentence and has no separator, so it stays one entry.
// Header-only pure policy so tests can pin it without linking the server stack.
inline std::vector<std::string> SplitTranslationGloss(std::string_view gloss)
{
    static constexpr std::string_view kFullwidthSemicolon = "\xEF\xBC\x9B";
    std::vector<std::string> senses;
    size_t start = 0;
    const auto push = [&senses](std::string_view sense) {
        const size_t first = sense.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return;
        const size_t last = sense.find_last_not_of(" \t\r\n");
        sense = sense.substr(first, last - first + 1);
        // POS and Japanese readings are display annotations, not output text.
        for (const auto pos : {"n. ", "v. ", "adj. ", "adv. ", "prep. ", "pron. ", "conj. ", "int. ", "num. ", "aux. ",
                               "phr. ", "m. ", "det. ", "part. "})
            if (sense.substr(0, std::char_traits<char>::length(pos)) == pos)
            {
                sense.remove_prefix(std::char_traits<char>::length(pos));
                break;
            }
        const auto reading = sense.rfind(" (");
        if (reading != std::string_view::npos && sense.back() == ')' &&
            (sense.substr(reading).find("\xE3\x81") != std::string_view::npos ||
             sense.substr(reading).find("\xE3\x82") != std::string_view::npos ||
             sense.substr(reading).find("\xE3\x83") != std::string_view::npos))
            sense = sense.substr(0, reading);
        if (!sense.empty())
            senses.emplace_back(sense);
    };
    while (start <= gloss.size())
    {
        const size_t ascii = gloss.find(';', start);
        const size_t fullwidth = gloss.find(kFullwidthSemicolon, start);
        const size_t cut = (std::min)(ascii, fullwidth);
        if (cut == std::string_view::npos)
        {
            push(gloss.substr(start));
            break;
        }
        push(gloss.substr(start, cut - start));
        start = cut + (cut == fullwidth ? kFullwidthSemicolon.size() : 1);
    }
    return senses;
}
} // namespace FanyImeIpc
