#pragma once

// Text form of a global hotkey, shared by the Server (RegisterHotKey) and the Settings host
// (validation before the value is written). The text form is what the settings page shows:
// "Ctrl+Alt+Shift+T". Modifier order is normalized to Ctrl, Alt, Shift, Win on output.

#include <Windows.h>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace HotkeyCombo
{
struct Combo
{
    UINT modifiers = 0; // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
    UINT vk = 0;
};

inline std::vector<std::string> SplitPlus(const std::string &text)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text)
    {
        if (ch == '+')
        {
            parts.push_back(current);
            current.clear();
        }
        else if (ch != ' ')
            current.push_back(ch);
    }
    parts.push_back(current);
    return parts;
}

inline std::optional<UINT> KeyFromName(const std::string &name)
{
    if (name.size() == 1)
    {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        if (ch >= 'A' && ch <= 'Z')
            return static_cast<UINT>(ch);
        if (ch >= '0' && ch <= '9')
            return static_cast<UINT>(ch);
        return std::nullopt;
    }
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f'))
    {
        int number = 0;
        for (size_t i = 1; i < name.size(); ++i)
        {
            if (name[i] < '0' || name[i] > '9')
                return std::nullopt;
            number = number * 10 + (name[i] - '0');
        }
        if (number >= 1 && number <= 24)
            return static_cast<UINT>(VK_F1 + number - 1);
        return std::nullopt;
    }
    struct Named
    {
        const char *name;
        UINT vk;
    };
    static const Named named[] = {
        {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Tab", VK_TAB},      {"Insert", VK_INSERT}, {"Delete", VK_DELETE},
        {"Home", VK_HOME},   {"End", VK_END},      {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Up", VK_UP},
        {"Down", VK_DOWN},   {"Left", VK_LEFT},    {"Right", VK_RIGHT},  {"Backspace", VK_BACK}};
    for (const auto &item : named)
        if (_stricmp(name.c_str(), item.name) == 0)
            return item.vk;
    return std::nullopt;
}

inline std::string KeyName(UINT vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk)
    {
    case VK_SPACE:
        return "Space";
    case VK_RETURN:
        return "Enter";
    case VK_TAB:
        return "Tab";
    case VK_INSERT:
        return "Insert";
    case VK_DELETE:
        return "Delete";
    case VK_HOME:
        return "Home";
    case VK_END:
        return "End";
    case VK_PRIOR:
        return "PageUp";
    case VK_NEXT:
        return "PageDown";
    case VK_UP:
        return "Up";
    case VK_DOWN:
        return "Down";
    case VK_LEFT:
        return "Left";
    case VK_RIGHT:
        return "Right";
    case VK_BACK:
        return "Backspace";
    default:
        return {};
    }
}

// Empty text means "disabled" and parses to nullopt; callers distinguish that from invalid text
// with IsValid. A combo needs Ctrl, Alt or Win so a bare letter can never swallow typing.
inline std::optional<Combo> Parse(const std::string &text)
{
    if (text.empty() || text.size() > 40)
        return std::nullopt;
    Combo combo;
    const auto parts = SplitPlus(text);
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const auto &part = parts[i];
        const bool last = i + 1 == parts.size();
        if (_stricmp(part.c_str(), "Ctrl") == 0 && !last)
            combo.modifiers |= MOD_CONTROL;
        else if (_stricmp(part.c_str(), "Alt") == 0 && !last)
            combo.modifiers |= MOD_ALT;
        else if (_stricmp(part.c_str(), "Shift") == 0 && !last)
            combo.modifiers |= MOD_SHIFT;
        else if (_stricmp(part.c_str(), "Win") == 0 && !last)
            combo.modifiers |= MOD_WIN;
        else if (last)
        {
            const auto vk = KeyFromName(part);
            if (!vk)
                return std::nullopt;
            combo.vk = *vk;
        }
        else
            return std::nullopt;
    }
    if (!combo.vk || !(combo.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)))
        return std::nullopt;
    return combo;
}

inline std::string Format(const Combo &combo)
{
    std::string text;
    if (combo.modifiers & MOD_CONTROL)
        text += "Ctrl+";
    if (combo.modifiers & MOD_ALT)
        text += "Alt+";
    if (combo.modifiers & MOD_SHIFT)
        text += "Shift+";
    if (combo.modifiers & MOD_WIN)
        text += "Win+";
    return text + KeyName(combo.vk);
}

inline bool IsValid(const std::string &text)
{
    return text.empty() || Parse(text).has_value();
}

// Normalized text, so "Alt+Ctrl+o" style variants compare equal when checking duplicates.
inline std::string Normalize(const std::string &text)
{
    const auto combo = Parse(text);
    return combo ? Format(*combo) : std::string();
}
} // namespace HotkeyCombo
