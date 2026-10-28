// 全拼纠错探针：用真实的 quanpin 纠错函数导出五类记录，当 Rust 实现的行为规格。
//
//  kbest   —— autocorrect_cut_kbest：随机输入（真音节拼接后做漏字/多字/换位/邻键/远键替换，另有手动分隔符、超长串、
//             大写字母）× 16 种纠错类型掩码 × k=1/3/9，逐条记各段（音节、敲的字母、起点、是否纠正、权重）、
//             纠正边数、权重和、是否带生成式边。
//  legal   —— legal_input_correction_cuts：合法拼音串（含零声母音节、罕见合法音节 lia/dia）× 掩码 × k。
//  reading —— correction_cut_for_reading 与 corrected_reading_raw_length：读音取自 k-best 结果的前缀或随机音节。
//  jianpin —— looks_like_jianpin_spelling。
//  cut1    —— autocorrect_cut / autocorrect_cut_detail（k=1 的投影）。
//
// 用法：msime_quanpin_autocorrect_probe <输出文件>
#include "quanpin/quanpin_query.h"
#include "quanpin/quanpin_utils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
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
    template <typename T> const T &pick(const std::vector<T> &items) { return items[below(static_cast<std::uint32_t>(items.size()))]; }
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

std::string cut_json(const quanpin::AutocorrectCut &cut)
{
    std::string json = "{\"segments\":[";
    for (std::size_t i = 0; i < cut.segments.size(); ++i)
    {
        const auto &s = cut.segments[i];
        json += std::string(i ? "," : "") + "[" + quote(s.syllable) + "," + quote(s.raw_text) + "," + std::to_string(s.start) + "," + (s.corrected ? "1" : "0") + "," +
                std::to_string(s.weight) + "]";
    }
    return json + "],\"edges\":" + std::to_string(cut.edge_count) + ",\"weight\":" + std::to_string(cut.weight) + ",\"generated\":" + (cut.has_generated ? "1" : "0") + "}";
}

std::string cuts_json(const std::vector<quanpin::AutocorrectCut> &cuts)
{
    std::string json = "[";
    for (std::size_t i = 0; i < cuts.size(); ++i)
        json += (i ? "," : "") + cut_json(cuts[i]);
    return json + "]";
}

std::string strings_json(const std::vector<std::string> &items)
{
    std::string json = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        json += (i ? "," : "") + quote(items[i]);
    return json + "]";
}

// 随机输入：真音节拼接后做各种手误。
std::string random_input(Lcg &rng, const std::vector<std::string> &syllables)
{
    std::string input;
    const std::uint32_t count = 1 + rng.below(rng.below(12) == 0 ? 14 : 5);
    for (std::uint32_t k = 0; k < count; ++k)
        input += rng.pick(syllables);
    const std::uint32_t damages = rng.below(4);
    for (std::uint32_t d = 0; d < damages && !input.empty(); ++d)
    {
        switch (rng.below(7))
        {
        case 0: // 漏字
            input.erase(rng.below(static_cast<std::uint32_t>(input.size())), 1);
            break;
        case 1: // 多字（重复或随机）
        {
            const auto at = rng.below(static_cast<std::uint32_t>(input.size() + 1));
            input.insert(at, 1, rng.below(2) && at > 0 ? input[at - 1] : static_cast<char>('a' + rng.below(26)));
            break;
        }
        case 2: // 换位
            if (input.size() > 1)
            {
                const auto at = rng.below(static_cast<std::uint32_t>(input.size() - 1));
                std::swap(input[at], input[at + 1]);
            }
            break;
        case 3: // 邻键或远键替换
            input[rng.below(static_cast<std::uint32_t>(input.size()))] = static_cast<char>('a' + rng.below(26));
            break;
        case 4: // 元音挤出：ia -> ai 之类
            if (input.size() > 2)
            {
                const auto at = rng.below(static_cast<std::uint32_t>(input.size() - 1));
                std::swap(input[at], input[at + 1]);
            }
            break;
        default:
            break;
        }
    }
    const std::uint32_t special = rng.below(40);
    if (special == 0 && !input.empty())
        input.insert(rng.below(static_cast<std::uint32_t>(input.size())), "'");
    else if (special == 1 && !input.empty())
        input[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(input[0])));
    else if (special == 2)
        while (input.size() < 66)
            input += rng.pick(syllables);
    return input;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "用法：msime_quanpin_autocorrect_probe <输出文件>\n";
        return 2;
    }
    std::ofstream out(argv[1], std::ios::binary);
    const auto &syllables = quanpin::intact_pinyin_list();
    Lcg rng{0xac0ffee5ULL};
    int id = 0;

    // kbest / cut1
    for (int i = 0; i < 3000; ++i)
    {
        const std::string input = random_input(rng, syllables);
        const unsigned mask = rng.below(4) == 0 ? rng.below(16) : (rng.below(2) ? 0xf : (rng.below(2) ? 0xd : 0xe));
        const std::size_t k = rng.below(3) == 0 ? (rng.below(2) ? 1 : 3) : 9;
        out << "{\"kind\":\"kbest\",\"id\":" << id++ << ",\"input\":" << quote(input) << ",\"mask\":" << mask << ",\"k\":" << k << ",\"ret\":" << cuts_json(quanpin::autocorrect_cut_kbest(input, mask, k)) << "}\n";
        if (i % 5 == 0)
            out << "{\"kind\":\"cut1\",\"id\":" << id++ << ",\"input\":" << quote(input) << ",\"mask\":" << mask << ",\"ret\":" << cut_json(quanpin::autocorrect_cut_detail(input, mask)) << ",\"plain\":" << strings_json(quanpin::autocorrect_cut(input, mask)) << "}\n";
    }

    // jianpin
    const std::vector<std::string> jianpin_pool = {"zheg", "ywen", "sshang", "nbn", "bqng", "gau", "zaijinut", "zher", "n", "ni", "nih", "zh", "hauz"};
    for (int i = 0; i < 1500; ++i)
    {
        std::string input = rng.below(5) == 0 ? rng.pick(jianpin_pool) : random_input(rng, syllables);
        if (rng.below(3) == 0)
        {
            // 合法音节后面带一两个字母，或前面带一个声母。
            std::string base = rng.pick(syllables) + rng.pick(syllables);
            input = rng.below(2) ? base + static_cast<char>('a' + rng.below(26)) : std::string(1, static_cast<char>('a' + rng.below(26))) + base;
        }
        out << "{\"kind\":\"jianpin\",\"id\":" << id++ << ",\"input\":" << quote(input) << ",\"ret\":" << (quanpin::looks_like_jianpin_spelling(input) ? "true" : "false") << "}\n";
    }

    // legal
    const std::vector<std::string> zero_initials = {"a", "o", "e", "ai", "an", "ang", "ao", "ou", "en", "er", "ei", "lia", "dia", "ia"};
    for (int i = 0; i < 2500; ++i)
    {
        quanpin::Segments segments;
        const std::uint32_t count = 1 + rng.below(6);
        for (std::uint32_t k = 0; k < count; ++k)
            segments.push_back(rng.below(3) == 0 ? rng.pick(zero_initials) : rng.pick(syllables));
        if (rng.below(40) == 0)
            segments[rng.below(static_cast<std::uint32_t>(segments.size()))] = "zh"; // 不完整，必须返回空
        std::string pinyin;
        for (const auto &s : segments)
            pinyin += s;
        if (rng.below(30) == 0)
            pinyin += "x"; // 与切分不一致，必须返回空
        const unsigned mask = rng.below(4) == 0 ? rng.below(16) : (rng.below(2) ? 0xf : 0xd);
        const std::size_t k = rng.below(4) == 0 ? 1 + rng.below(4) : 3;
        out << "{\"kind\":\"legal\",\"id\":" << id++ << ",\"pinyin\":" << quote(pinyin) << ",\"segments\":" << strings_json(segments) << ",\"mask\":" << mask << ",\"k\":" << k
            << ",\"ret\":" << cuts_json(quanpin::legal_input_correction_cuts(pinyin, segments, mask, k)) << "}\n";
    }

    // reading
    int with_cut = 0;
    for (int i = 0; i < 2500; ++i)
    {
        const std::string input = random_input(rng, syllables);
        const unsigned mask = rng.below(4) == 0 ? rng.below(16) : (rng.below(2) ? 0xf : 0xd);
        quanpin::Segments reading;
        const auto cuts = quanpin::autocorrect_cut_kbest(input, mask, 9);
        if (!cuts.empty() && rng.below(5) != 0)
        {
            const auto &cut = cuts[rng.below(static_cast<std::uint32_t>(cuts.size()))];
            const std::uint32_t take = 1 + rng.below(static_cast<std::uint32_t>(cut.segments.size()));
            for (std::uint32_t s = 0; s < take; ++s)
                reading.push_back(cut.segments[s].syllable);
            if (rng.below(12) == 0)
                reading.back() = rng.pick(syllables);
        }
        else
        {
            const std::uint32_t count = rng.below(4);
            for (std::uint32_t s = 0; s < count; ++s)
                reading.push_back(rng.pick(syllables));
        }
        const auto detail = quanpin::correction_cut_for_reading(input, reading, mask);
        const auto raw_length = quanpin::corrected_reading_raw_length(input, reading, mask);
        with_cut += detail.has_value();
        out << "{\"kind\":\"reading\",\"id\":" << id++ << ",\"input\":" << quote(input) << ",\"reading\":" << strings_json(reading) << ",\"mask\":" << mask
            << ",\"cut\":" << (detail ? cut_json(*detail) : "null") << ",\"raw_length\":" << (raw_length ? std::to_string(*raw_length) : "null") << "}\n";
    }
    std::cerr << "写完，有对应切分的读音 " << with_cut << "\n";
    return 0;
}
