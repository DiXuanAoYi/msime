#pragma once

#include <string>

// The server reply covers the remaining spelling, whereas TSF replaces the
// entire composition range. Carry the already selected Han prefix exactly once.
inline std::wstring BuildPunctuationCommitText(const std::wstring &creatingWordPrefix,
                                               const std::wstring &punctuationText)
{
    return creatingWordPrefix + punctuationText;
}
