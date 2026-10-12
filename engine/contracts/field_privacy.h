#pragma once

#include <cstdint>

namespace FanyImeFieldPrivacy
{
enum class State : std::uint32_t
{
    Unknown = 0,
    Regular = 1,
    Sensitive = 2,
};

// KeyEvent and FieldPrivacyChanged only. They do not overlap UiLess, Caps
// snapshots or the optional trilingual cycle state bits.
constexpr std::uint32_t KnownFlag = 0x02000000u;
constexpr std::uint32_t SensitiveFlag = 0x01000000u;
constexpr std::uint32_t Mask = KnownFlag | SensitiveFlag;

constexpr std::uint32_t Encode(State state)
{
    return state == State::Sensitive ? Mask : state == State::Regular ? KnownFlag : 0u;
}

constexpr State Decode(std::uint32_t flags, bool negotiated)
{
    if (!negotiated)
        return State::Unknown;
    // An invalid sensitive-without-known shape still fails closed.
    return (flags & SensitiveFlag) ? State::Sensitive : (flags & KnownFlag) ? State::Regular : State::Unknown;
}

constexpr bool AllowsAutomaticNetwork(State state)
{
    return state == State::Regular;
}

constexpr bool AllowsLocalHistory(State state)
{
    // Hosts without InputScope remain usable offline. Unknown never authorizes
    // automatic network requests, and must not be relabelled as Regular.
    return state != State::Sensitive;
}
} // namespace FanyImeFieldPrivacy
