#pragma once

namespace VimMode
{
constexpr bool ShouldSwitchToEnglish(unsigned key, bool eaten, bool repeat, unsigned modifiers, bool winDown,
                                     bool compositionActive, bool deferredKeysPending)
{
    return key == 0x1B && !eaten && !repeat && modifiers == 0 && !winDown && !compositionActive && !deferredKeysPending;
}
} // namespace VimMode
