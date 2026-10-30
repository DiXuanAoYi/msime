#include "key_map.h"

namespace msime
{
namespace
{
Action command(Command value)
{
    Action action;
    action.kind = ActionKind::Command;
    action.command = value;
    return action;
}

Action simple(ActionKind kind, char value = 0)
{
    Action action;
    action.kind = kind;
    action.value = value;
    return action;
}

bool isLetter(std::uint32_t sym)
{
    return (sym >= 'a' && sym <= 'z') || (sym >= 'A' && sym <= 'Z');
}

bool isPunctuation(std::uint32_t sym)
{
    return sym >= 0x21 && sym <= 0x7e && !isLetter(sym) && !(sym >= '0' && sym <= '9');
}
} // namespace

Action mapKey(std::uint32_t sym, Modifiers modifiers, bool composing)
{
    // Ctrl、Alt、Super 是应用的快捷键，输入法不碰。
    if (modifiers.control || modifiers.alt || modifiers.super)
        return {};

    if (isLetter(sym))
    {
        Action action = simple(ActionKind::Character, static_cast<char>(sym));
        // Shift+字母是本地模式的触发键；只开着大写锁定时是普通的大写字母。
        action.shiftOnly = modifiers.shift && sym >= 'A' && sym <= 'Z';
        return action;
    }

    if (composing)
    {
        switch (sym)
        {
        case keysym::Space:
            return simple(ActionKind::CommitHighlighted);
        case keysym::Return:
        case keysym::KpEnter:
            return command(Command::CommitRaw);
        case keysym::BackSpace:
            return command(Command::Backspace);
        case keysym::Escape:
            return command(Command::Cancel);
        case keysym::Left:
            return command(Command::MoveLeft);
        case keysym::Right:
            return command(Command::MoveRight);
        case keysym::Home:
            return command(Command::MoveHome);
        case keysym::End:
            return command(Command::MoveEnd);
        case keysym::Delete:
            return command(Command::DeleteForward);
        case keysym::PageUp:
            return simple(ActionKind::PageUp);
        case keysym::PageDown:
            return simple(ActionKind::PageDown);
        case keysym::Up:
        case keysym::Down:
        case keysym::Tab:
            return simple(ActionKind::Swallow);
        default:
            break;
        }
        if (sym >= '1' && sym <= '9')
            return simple(ActionKind::CandidateDigit, static_cast<char>(sym - '0'));
        // 撇号是音节分隔符，组合里属于输入。
        if (sym == '\'')
            return simple(ActionKind::Character, '\'');
    }

    if (isPunctuation(sym))
        return simple(ActionKind::Punctuation, static_cast<char>(sym));
    return {};
}
} // namespace msime
