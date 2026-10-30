#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace FanyImeRuntimeSettings
{
constexpr std::uint32_t EnglishCandidates = 1u << 0;
constexpr std::uint32_t TrilingualCycle = 1u << 1;
constexpr std::uint32_t VimMode = 1u << 2;
constexpr std::uint32_t AiCompletion = 1u << 3;
constexpr std::uint32_t AiQuestion = 1u << 4;
constexpr std::uint32_t KnownFlags = EnglishCandidates | TrilingualCycle | VimMode | AiCompletion | AiQuestion;

struct Snapshot
{
    std::uint64_t revision = 0;
    std::uint32_t flags = EnglishCandidates;
};

inline bool ShouldApply(const Snapshot &next, std::uint32_t generation, std::uint32_t previous_generation,
                        std::uint64_t previous_revision)
{
    return generation != 0 && next.revision != 0 && (next.flags & ~KnownFlags) == 0 &&
           (generation != previous_generation || next.revision > previous_revision);
}

inline bool ShouldOfferEnglishCandidates(std::uint32_t flags, bool existing_composition)
{
    return (flags & EnglishCandidates) != 0 || existing_composition;
}

inline std::wstring Encode(Snapshot snapshot)
{
    return L"1\t" + std::to_wstring(snapshot.revision) + L'\t' + std::to_wstring(snapshot.flags);
}

inline std::optional<std::uint64_t> ParseUnsigned(std::wstring_view text)
{
    if (text.empty() || text.size() > 20)
        return std::nullopt;
    std::uint64_t value = 0;
    for (const wchar_t ch : text)
    {
        if (ch < L'0' || ch > L'9')
            return std::nullopt;
        const auto digit = static_cast<unsigned>(ch - L'0');
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

inline std::optional<Snapshot> Decode(std::wstring_view text)
{
    if (text.size() > 48 || text.substr(0, 2) != L"1\t")
        return std::nullopt;
    const auto delimiter = text.find(L'\t', 2);
    if (delimiter == std::wstring_view::npos)
        return std::nullopt;
    const auto revision = ParseUnsigned(text.substr(2, delimiter - 2));
    const auto flags = ParseUnsigned(text.substr(delimiter + 1));
    if (!revision || *revision == 0 || !flags || *flags > KnownFlags)
        return std::nullopt;
    return Snapshot{*revision, static_cast<std::uint32_t>(*flags)};
}
} // namespace FanyImeRuntimeSettings
