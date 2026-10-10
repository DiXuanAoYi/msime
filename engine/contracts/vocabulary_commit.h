#pragma once
#include "ipc_protocol_limits.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace FanyImeVocabulary
{
struct Commit
{
    uint64_t request;
    std::wstring_view text;
};
inline std::optional<std::wstring> Encode(uint64_t token, const std::wstring &text)
{
    if (token <= (uint64_t{1} << 63) || token == UINT64_MAX || text.empty() || text.find(L'\0') != std::wstring::npos)
        return std::nullopt;
    auto payload = std::to_wstring(token) + L'\t' + text;
    if (payload.size() > FanyImePipeLimits::CandidateTextMaxLength)
        return std::nullopt;
    return payload;
}
inline std::optional<Commit> Decode(std::wstring_view payload)
{
    if (payload.size() > FanyImePipeLimits::CandidateTextMaxLength)
        return std::nullopt;
    const auto tab = payload.find(L'\t');
    if ((tab != 19 && tab != 20) || tab + 1 >= payload.size())
        return std::nullopt;
    uint64_t token = 0;
    for (const wchar_t ch : payload.substr(0, tab))
    {
        if (ch < L'0' || ch > L'9')
            return std::nullopt;
        const auto digit = static_cast<uint64_t>(ch - L'0');
        if (token > (UINT64_MAX - digit) / 10)
            return std::nullopt;
        token = token * 10 + digit;
    }
    if (token <= (uint64_t{1} << 63) || token == UINT64_MAX)
        return std::nullopt;
    return Commit{token, payload.substr(tab + 1)};
}
} // namespace FanyImeVocabulary
