#pragma once
#include <cstdint>

namespace FanyImeAiInput
{
enum class Task
{
    None,
    Completion,
    Question
};
constexpr Task Shortcut(std::uint32_t key, std::uint32_t modifiers, bool composition, bool completion_enabled,
                        bool question_enabled)
{
    if (!composition || (modifiers & 7u) != 6u)
        return Task::None;
    if (key == 0x0du && completion_enabled)
        return Task::Completion;
    if (key == static_cast<std::uint32_t>('Q') && question_enabled)
        return Task::Question;
    return Task::None;
}
} // namespace FanyImeAiInput
