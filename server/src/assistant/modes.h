#pragma once
#include <string_view>
#include <vector>
namespace ReplyStudio
{
struct Mode
{
    std::string_view id, name, description, prompt;
};
const std::vector<Mode> &Modes();
const Mode *FindMode(std::string_view id);
} // namespace ReplyStudio
