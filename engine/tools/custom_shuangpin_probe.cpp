// 自定义双拼方案探针：对内置方案做随机改动（换键、删项、加错项、大小写、非法按键），用真实的
// BuildCustomShuangpinProfile 构建并校验，导出布局、错误信息和成功时的方案表，当 Rust 实现的行为规格。
//
// 用法：msime_custom_shuangpin_probe [输出文件]
// 输出 kind：custom_layout。布局的三张表写成 [键, 值] 数组（顺序无意义，探针里已排序）；
// 方案表同样排序后写出。
#include "shuangpin/shuangpin_profile.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

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
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

std::string quote(const std::string &text)
{
    std::string out = "\"";
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\')
            out += std::string("\\") + static_cast<char>(c);
        else if (c < 0x20)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
        }
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string table_json(const std::unordered_map<std::string, std::string> &table)
{
    const std::map<std::string, std::string> sorted(table.begin(), table.end());
    std::string out = "[";
    bool first = true;
    for (const auto &[key, value] : sorted)
    {
        out += std::string(first ? "" : ",") + "[" + quote(key) + "," + quote(value) + "]";
        first = false;
    }
    return out + "]";
}

const char *rule_name(ShuangpinZeroInitialRule rule)
{
    switch (rule)
    {
    case ShuangpinZeroInitialRule::LetterO: return "o";
    case ShuangpinZeroInitialRule::LetterA: return "a";
    case ShuangpinZeroInitialRule::FirstLetter: return "first_letter";
    case ShuangpinZeroInitialRule::FullPinyin: return "full_pinyin";
    }
    return "o";
}

std::string random_key(Lcg &rng, bool allow_semicolon)
{
    const std::uint32_t pick = rng.below(allow_semicolon ? 27 : 26);
    return pick == 26 ? ";" : std::string(1, static_cast<char>('a' + pick));
}

std::string upper_first(std::string text)
{
    if (!text.empty())
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}
} // namespace

int main(int argc, char **argv)
{
    std::ofstream file;
    if (argc > 1)
        file.open(argv[1], std::ios::binary);
    std::ostream &out = argc > 1 ? static_cast<std::ostream &>(file) : std::cout;

    static const char *builtin[] = {"xiaohe", "ziranma", "shoudao", "microsoft", "sogou", "ziguang", "zhinengabc", "guobiao", "pinyinjiajia"};
    static const char *rules[] = {"o", "a", "first_letter", "full_pinyin"};
    Lcg rng{0xc057040fULL};
    int succeeded = 0;
    for (int scenario = 0; scenario < 1200; ++scenario)
    {
        const ShuangpinProfile &base = GetShuangpinProfile(builtin[scenario % 9]);
        CustomShuangpinLayout layout;
        layout.name = "custom" + std::to_string(scenario);
        layout.zero_initial_rule = *ParseShuangpinZeroInitialRule(rules[rng.below(4)]);
        // 声母表里内置方案只列出需要改的（zh/ch/sh 和被挪走的），原样拿过来。
        layout.initials = base.initials;
        layout.finals = base.finals;
        if (rng.below(3) == 0)
            for (const auto &[final, code] : base.zero_initials)
                if (rng.below(3) == 0)
                    layout.zero_initial_overrides[final] = code;

        const std::uint32_t mutations = scenario < 40 ? 0 : rng.below(4);
        for (std::uint32_t m = 0; m < mutations; ++m)
        {
            switch (rng.below(12))
            {
            case 0: // 换一个声母的键
                if (!layout.initials.empty())
                {
                    auto it = layout.initials.begin();
                    std::advance(it, rng.below(static_cast<std::uint32_t>(layout.initials.size())));
                    it->second = random_key(rng, false);
                }
                break;
            case 1: // 换一个韵母的键
            {
                auto it = layout.finals.begin();
                std::advance(it, rng.below(static_cast<std::uint32_t>(layout.finals.size())));
                it->second = random_key(rng, true);
                break;
            }
            case 2: // 删一个韵母
            {
                auto it = layout.finals.begin();
                std::advance(it, rng.below(static_cast<std::uint32_t>(layout.finals.size())));
                layout.finals.erase(it);
                break;
            }
            case 3: // 删 zh/ch/sh
                layout.initials.erase(std::vector<std::string>{"zh", "ch", "sh"}[rng.below(3)]);
                break;
            case 4: // 未知声母 / 韵母
                if (rng.below(2))
                    layout.initials["xx"] = "a";
                else
                    layout.finals["zz"] = "b";
                break;
            case 5: // 非法按键
            {
                const char *bad[] = {"ab", "1", "", ";", "A"};
                if (rng.below(2) && !layout.initials.empty())
                    layout.initials.begin()->second = bad[rng.below(5)];
                else
                    layout.finals.begin()->second = bad[rng.below(5)];
                break;
            }
            case 6: // 大写写法
            {
                std::unordered_map<std::string, std::string> upper;
                for (const auto &[key, value] : layout.finals)
                    upper[rng.below(2) ? upper_first(key) : key] = rng.below(2) ? value : upper_first(value);
                layout.finals = upper;
                break;
            }
            case 7: // 韵母 er
                layout.finals["er"] = rng.below(5) == 0 ? "1" : random_key(rng, true);
                break;
            case 8: // 只写 ue 或只写 ve
                layout.finals.erase(rng.below(2) ? "ue" : "ve");
                break;
            case 9: // 零声母覆盖
            {
                const char *finals[] = {"a", "ai", "an", "ang", "ao", "e", "ei", "en", "eng", "er", "o", "ou", "ia"};
                const char *codes[] = {"aa", "oa", "ee", "a;", "ab", "a", "1a", "ez", "AA"};
                layout.zero_initial_overrides[finals[rng.below(13)]] = codes[rng.below(9)];
                break;
            }
            case 10: // 两个声母共用一个键
            {
                auto it = layout.initials.begin();
                std::advance(it, rng.below(static_cast<std::uint32_t>(layout.initials.size())));
                layout.initials["b"] = it->second;
                break;
            }
            default: // 两个韵母共用一个键
            {
                auto it = layout.finals.begin();
                std::advance(it, rng.below(static_cast<std::uint32_t>(layout.finals.size())));
                const std::string key = it->second;
                layout.finals[rng.below(2) ? "an" : "ing"] = key;
                break;
            }
            }
        }

        ShuangpinProfile profile;
        const auto errors = BuildCustomShuangpinProfile(layout, profile);
        succeeded += errors.empty();
        out << "{\"kind\":\"custom_layout\",\"name\":" << quote(layout.name) << ",\"rule\":" << quote(rule_name(layout.zero_initial_rule))
            << ",\"initials\":" << table_json(layout.initials) << ",\"finals\":" << table_json(layout.finals)
            << ",\"overrides\":" << table_json(layout.zero_initial_overrides) << ",\"errors\":[";
        for (std::size_t i = 0; i < errors.size(); ++i)
            out << (i ? "," : "") << quote(errors[i]);
        out << "],\"profile\":";
        if (errors.empty())
            out << "{\"initials\":" << table_json(profile.initials) << ",\"zero_initials\":" << table_json(profile.zero_initials)
                << ",\"finals\":" << table_json(profile.finals) << "}";
        else
            out << "null";
        out << "}\n";
    }
    for (const char *name : {"o", "a", "first_letter", "full_pinyin", "", "O", "x"})
        out << "{\"kind\":\"rule\",\"name\":" << quote(name) << ",\"parsed\":"
            << (ParseShuangpinZeroInitialRule(name) ? quote(rule_name(*ParseShuangpinZeroInitialRule(name))) : std::string("null")) << "}\n";
    std::cerr << "成功 " << succeeded << " / 1200\n";
    return 0;
}
