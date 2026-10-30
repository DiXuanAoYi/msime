// 把平台按键翻译成对引擎 C 接口的一次调用。不依赖 fcitx5：keysym 用 X11 的数值（xkbcommon 和 fcitx5 的 FcitxKey_* 同值）。
#pragma once
#include <cstdint>

namespace msime
{
namespace keysym
{
constexpr std::uint32_t Space = 0x0020;
constexpr std::uint32_t BackSpace = 0xff08;
constexpr std::uint32_t Tab = 0xff09;
constexpr std::uint32_t Return = 0xff0d;
constexpr std::uint32_t Escape = 0xff1b;
constexpr std::uint32_t Home = 0xff50;
constexpr std::uint32_t Left = 0xff51;
constexpr std::uint32_t Up = 0xff52;
constexpr std::uint32_t Right = 0xff53;
constexpr std::uint32_t Down = 0xff54;
constexpr std::uint32_t PageUp = 0xff55;
constexpr std::uint32_t PageDown = 0xff56;
constexpr std::uint32_t End = 0xff57;
constexpr std::uint32_t KpEnter = 0xff8d;
constexpr std::uint32_t Delete = 0xffff;
} // namespace keysym

// 与 msime_command 的 enum msime_command 同值。
enum class Command : int
{
    Backspace = 0,
    CommitCandidate = 1,
    CommitRaw = 2,
    Cancel = 3,
    MoveLeft = 4,
    MoveRight = 5,
    MoveHome = 6,
    MoveEnd = 7,
    DeleteForward = 8,
};

enum class ActionKind
{
    PassThrough,   // 不是我们的键，交还给应用
    Character,     // msime_character(value, shiftOnly)
    Punctuation,   // msime_punctuation(value)
    Command,       // msime_command(command)
    CandidateDigit, // 数字键选当前页第 value 个（1 起）；页内序号换成总序号由界面层做
    CommitHighlighted, // 空格：上屏当前页的高亮候选
    PageUp,
    PageDown,
    Swallow,       // 组合中但没有对应操作（上下方向键等）：吞掉，免得应用收到
};

struct Action
{
    ActionKind kind = ActionKind::PassThrough;
    char value = 0;
    bool shiftOnly = false;
    Command command = Command::Cancel;
};

struct Modifiers
{
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
};

// composing：引擎里现在有没有组合。没有组合时只有字母、标点会开始一次输入，其余都放行。
Action mapKey(std::uint32_t sym, Modifiers modifiers, bool composing);
} // namespace msime
