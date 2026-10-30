// 输入形状契约探针：Shift+T 日期时间输入形状与双拼直接辅助码的按键形状，随机串 × 光标 × 字符。
//
// 用法：msime_input_contracts_probe <输出文件>
#include "contracts/date_time_input.h"
#include "contracts/direct_helpcode.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
struct Lcg
{
    std::uint64_t state;
    std::uint32_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    }
    std::uint32_t below(std::uint32_t n)
    {
        return next() % n;
    }
};
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "用法：msime_input_contracts_probe <输出文件>\n";
        return 2;
    }
    std::ofstream out(argv[1], std::ios::binary);
    Lcg rng{0x3c6a91d5ULL};
    static const char date_alphabet[] = "0123456789012345678901234567890123456789//::";
    static const char key_alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ;;;;//''";
    int date_records = 0, date_accepted = 0, key_records = 0, key_accepted = 0;
    for (int i = 0; i < 2500; ++i)
    {
        std::string text = "T";
        const std::uint32_t length = rng.below(18);
        for (std::uint32_t k = 0; k < length; ++k)
            text += date_alphabet[rng.below(sizeof(date_alphabet) - 1)];
        if (rng.below(20) == 0)
            text[0] = 'x';
        const std::size_t caret = rng.below(static_cast<std::uint32_t>(text.size() + 2));
        static const char chars[] = "0123456789/:a-T";
        const char ch = chars[rng.below(sizeof(chars) - 1)];
        const bool accepts = FanyImeDateTimeInput::AcceptsAt(text.data(), text.size(), caret, ch);
        const std::string body = text.substr(1);
        const bool viable = FanyImeDateTimeInput::IsViablePrefix(body.data(), body.size());
        const auto match = FanyImeDateTimeInput::MatchComplete(body.data(), body.size());
        out << "{\"kind\":\"date\",\"text\":\"" << text << "\",\"caret\":" << caret << ",\"ch\":" << static_cast<int>(ch) << ",\"accepts\":" << accepts << ",\"viable\":" << viable
            << ",\"shape\":" << static_cast<int>(match.shape) << ",\"groups\":" << match.group_count << ",\"specific\":" << FanyImeDateTimeInput::IsSpecificBody(body.data(), body.size()) << "}\n";
        ++date_records;
        date_accepted += accepts;
    }
    for (int i = 0; i < 2500; ++i)
    {
        std::string text;
        const std::uint32_t length = rng.below(10);
        for (std::uint32_t k = 0; k < length; ++k)
            text += key_alphabet[rng.below(sizeof(key_alphabet) - 1)];
        const std::size_t caret = rng.below(static_cast<std::uint32_t>(text.size() + 2));
        const bool slash = FanyImeDirectHelpcode::AcceptsSlashAt(text.data(), text.size(), caret);
        const bool semicolon = FanyImeDirectHelpcode::AcceptsSemicolonFinalAt(text.data(), text.size(), caret);
        out << "{\"kind\":\"direct\",\"text\":\"" << text << "\",\"caret\":" << caret << ",\"slash\":" << slash << ",\"semicolon\":" << semicolon << "}\n";
        ++key_records;
        key_accepted += slash;
    }
    std::cerr << "输入形状：日期 " << date_records << " 条（接受 " << date_accepted << "），直接辅助码 " << key_records << " 条（斜杠接受 " << key_accepted << "）\n";
    return 0;
}
