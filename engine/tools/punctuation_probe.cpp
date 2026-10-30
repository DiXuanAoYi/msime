// 标点策略探针：真实的 PunctuationPolicy 吃随机字符序列（支持的标点、成对的引号和书名号、不支持的字符），
// 每个序列用一个新策略，导出每个字符的译文（不支持为 null）。
//
// 用法：msime_punctuation_probe <输出文件>
#include "core/punctuation_policy.h"

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
        std::cerr << "用法：msime_punctuation_probe <输出文件>\n";
        return 2;
    }
    std::ofstream out(argv[1], std::ios::binary);
    Lcg rng{0x7a3f19c5ULL};
    // 偏向引号和书名号：它们带状态。
    static const char alphabet[] = "\"\"\"''<<<>>>,.?!;:()[]\\`$^_a1 -/=~@#%&*{}|+";
    const std::uint32_t alphabet_size = sizeof(alphabet) - 1;
    int sequences = 0, produced = 0;
    for (int round = 0; round < 400; ++round)
    {
        metasequoia::PunctuationPolicy policy;
        std::string input;
        std::string output = "[";
        const std::uint32_t length = 1 + rng.below(40);
        for (std::uint32_t i = 0; i < length; ++i)
        {
            const char c = alphabet[rng.below(alphabet_size)];
            input += c;
            const char *text = policy.translate(c);
            output += (i ? "," : "");
            if (text)
            {
                output += std::string("\"") + text + "\"";
                ++produced;
            }
            else
                output += "null";
        }
        output += "]";
        std::string escaped;
        for (const char c : input)
        {
            if (c == '"' || c == '\\')
                escaped += '\\';
            escaped += c;
        }
        out << "{\"round\":" << round << ",\"input\":\"" << escaped << "\",\"output\":" << output << "}\n";
        ++sequences;
    }
    std::cerr << "标点：" << sequences << " 个序列，有译文 " << produced << " 个字符\n";
    return 0;
}
