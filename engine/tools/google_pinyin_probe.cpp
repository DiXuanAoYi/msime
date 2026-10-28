// Google 拼音解码器探针：用真实的 PinyinDecoder（googlepinyinime-rev 的 MatrixSearch）在随包 dict_pinyin.dat 上
// 导出，当 Rust 实现的行为规格。
//
//  splid    —— SpellingParser::get_splid_by_str：每个音节、音节的各个前缀、带隔音符/大写/乱码的串；记编号和“没打完”。
//  qsort    —— 运行库 qsort（myqsort）排带大量相同键的数组后的次序；Google 解码器靠它排候选，同分次序全看它。
//  sentence —— PinyinDecoder::sentence：每次先 im_reset_search，所以是从头解。输入有整句、简拼、没打完的末音节、
//              隔音符、乱码、NUL、超过 64 个音节、127/128/129 字节边界；约三成带句中辅助码约束。
//  typing   —— 同一个串逐键输入再逐键删回去，中间不重置（适配层真实的用法），记每一步的结果；
//              Rust 每次从头解，所以这里也检验增量解码和从头解结果一致。
//
// 用法：msime_google_pinyin_probe <dict_pinyin.dat> <临时用户词库路径> <输出文件>
#include "core/pinyin_decoder.h"
#include "googlepinyinime-rev/src/include/mystdlib.h"
#include "googlepinyinime-rev/src/include/pinyinime.h"
#include "googlepinyinime-rev/src/include/spellingtrie.h"
#include "googlepinyinime-rev/src/include/splparser.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
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

std::vector<std::string> utf8_chars(const std::string &text)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        out.push_back(text.substr(i, n));
        i += n;
    }
    return out;
}

struct Input
{
    std::string text;
    std::vector<std::size_t> starts; // 每个音节的起点（只有按音节拼出来的输入才有）
};

struct Context
{
    Lcg rng;
    std::vector<std::string> spellings; // 全部音节，小写
    std::vector<std::string> pool;      // 已见过的汉字
};

std::string lower(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Input make_input(Context &ctx)
{
    auto &rng = ctx.rng;
    const auto syllable = [&]() -> const std::string & { return ctx.spellings[rng.below(static_cast<std::uint32_t>(ctx.spellings.size()))]; };
    Input in;
    const auto mode = rng.below(14);
    const auto add = [&](const std::string &s) {
        in.starts.push_back(in.text.size());
        in.text += s;
    };
    if (mode <= 4)
    {
        const auto n = 1 + rng.below(8);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            if (i > 0 && rng.below(8) == 0)
                in.text += '\'';
            add(syllable());
        }
    }
    else if (mode == 5)
    {
        const auto n = 1 + rng.below(10);
        for (std::uint32_t i = 0; i < n; ++i)
            add(std::string(1, syllable()[0]));
    }
    else if (mode == 6)
    {
        const auto n = 2 + rng.below(7);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            const auto &s = syllable();
            add(rng.below(2) ? s : std::string(1, s[0]));
        }
    }
    else if (mode == 7 || mode == 8)
    {
        const auto n = 1 + rng.below(5);
        for (std::uint32_t i = 0; i < n; ++i)
            add(syllable());
        const auto &s = syllable();
        add(s.substr(0, 1 + rng.below(static_cast<std::uint32_t>(s.size()))));
    }
    else if (mode == 9)
    {
        const auto n = 1 + rng.below(30);
        for (std::uint32_t i = 0; i < n; ++i)
            in.text += static_cast<char>('a' + rng.below(26));
    }
    else if (mode == 10)
    {
        // 长串：靠近 64 个音节的上限和 127/128 字节的边界。
        const auto target = 60 + rng.below(75);
        while (in.text.size() < target)
        {
            if (rng.below(4) == 0)
                add(std::string(1, syllable()[0]));
            else
                add(syllable());
        }
    }
    else if (mode == 11)
    {
        const auto n = 40 + rng.below(40);
        for (std::uint32_t i = 0; i < n; ++i)
            add(rng.below(2) ? "a" : (rng.below(2) ? "e" : "wo"));
    }
    else
    {
        const auto n = 2 + rng.below(6);
        for (std::uint32_t i = 0; i < n; ++i)
            add(syllable());
        if (rng.below(2) && in.text.size() > 1)
            in.text.pop_back();
    }
    if (rng.below(16) == 0 && !in.text.empty())
    {
        static const char junk[] = {'1', '9', ',', '.', ' ', 'V', 'Z', 'v', 'u', 'i', '\0', '\'', '`', ';'};
        const auto at = rng.below(static_cast<std::uint32_t>(in.text.size()) + 1);
        in.text.insert(in.text.begin() + at, junk[rng.below(sizeof(junk))]);
        in.starts.clear();
    }
    if (rng.below(12) == 0)
        for (auto &c : in.text)
            if (rng.below(3) == 0)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return in;
}

struct Constraint
{
    std::size_t offset = 0;
    std::set<std::string> allowed;
    std::string allowed_text;
};

std::vector<metasequoia::DecoderCharConstraint> to_decoder(const std::vector<Constraint> &constraints)
{
    std::vector<metasequoia::DecoderCharConstraint> out;
    for (const auto &c : constraints)
    {
        const auto *allowed = &c.allowed;
        out.push_back({c.offset, [allowed](const std::string &hanzi) { return allowed->count(hanzi) > 0; }});
    }
    return out;
}

std::string constraints_json(const std::vector<Constraint> &constraints)
{
    std::string out = "[";
    for (std::size_t i = 0; i < constraints.size(); ++i)
        out += (i ? "," : "") + std::string("[") + std::to_string(constraints[i].offset) + "," + quote(constraints[i].allowed_text) + "]";
    return out + "]";
}

struct Item
{
    unsigned key;
    unsigned index;
};

int compare_items(const void *a, const void *b)
{
    const auto x = static_cast<const Item *>(a)->key, y = static_cast<const Item *>(b)->key;
    return x < y ? -1 : x > y ? 1 : 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "用法：msime_google_pinyin_probe <dict_pinyin.dat> <临时用户词库路径> <输出文件>\n";
        return 2;
    }
    const std::filesystem::path model = argv[1], user = argv[2];
    std::filesystem::remove(user);
    std::ofstream out(argv[3], std::ios::binary);
    metasequoia::PinyinDecoder decoder(model, user);
    if (decoder.sentence("nihao").empty())
    {
        std::cerr << "解码器没起来：" << model << "\n";
        return 1;
    }

    Context ctx{{0x6d73696d65ULL}, {}, {}};
    {
        auto &trie = ime_pinyin::SpellingTrie::get_instance();
        const auto count = trie.get_spelling_num();
        for (std::size_t i = 0; i < count; ++i)
            ctx.spellings.push_back(lower(trie.get_spelling_str(static_cast<ime_pinyin::uint16>(ime_pinyin::kFullSplIdStart + i))));
        out << "{\"kind\":\"info\",\"spellings\":" << count << "}\n";
    }

    // splid
    {
        ime_pinyin::SpellingParser parser;
        std::vector<std::string> samples;
        for (const auto &s : ctx.spellings)
        {
            for (std::size_t n = 1; n <= s.size(); ++n)
                samples.push_back(s.substr(0, n));
            std::string up = s;
            up[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(up[0])));
            samples.push_back(up);
            samples.push_back(s + "'");
            samples.push_back(s + "'" + s);
            samples.push_back(s + s);
        }
        for (const char *extra : {"", "'", "''", "a'", "'a", "xi'an", "xi''an", "v", "u", "i", "iu", "zh", "ch", "sh", "zhh", "shi'", "ng", "n g", "a1", "Zhong", "ZHONG", "zHoNg", "hao.", "m", "mm", "hm"})
            samples.push_back(extra);
        Lcg local{0x5a11};
        for (int i = 0; i < 1500; ++i)
        {
            std::string s;
            const auto n = 1 + local.below(9);
            for (std::uint32_t k = 0; k < n; ++k)
                s += static_cast<char>("abcdefghijklmnopqrstuvwxyz'ABC"[local.below(30)]);
            samples.push_back(s);
        }
        for (const auto &s : samples)
        {
            bool pre = false;
            const auto id = parser.get_splid_by_str(s.data(), static_cast<ime_pinyin::uint16>(s.size()), &pre);
            out << "{\"kind\":\"splid\",\"s\":" << quote(s) << ",\"id\":" << id << ",\"pre\":" << (pre ? "true" : "false") << "}\n";
        }
    }

    // qsort
    {
        Lcg local{0x9507};
        for (int round = 0; round < 600; ++round)
        {
            const auto n = round < 40 ? static_cast<std::uint32_t>(round) : 1 + local.below(round < 400 ? 60 : 400);
            const auto range = 1 + local.below(round % 3 == 0 ? 3 : round % 3 == 1 ? 12 : 200);
            std::vector<Item> items(n);
            for (std::uint32_t i = 0; i < n; ++i)
                items[i] = {local.below(range), i};
            std::string keys = "[";
            for (std::uint32_t i = 0; i < n; ++i)
                keys += (i ? "," : "") + std::to_string(items[i].key);
            ime_pinyin::myqsort(items.data(), items.size(), sizeof(Item), compare_items);
            std::string order = "[";
            for (std::uint32_t i = 0; i < n; ++i)
                order += (i ? "," : "") + std::to_string(items[i].index);
            out << "{\"kind\":\"qsort\",\"keys\":" << keys << "],\"ret\":" << order << "]}\n";
        }
    }

    // sentence
    std::size_t constrained = 0, nonempty = 0, total = 0;
    for (int round = 0; round < 7000; ++round)
    {
        const auto in = make_input(ctx);
        ime_pinyin::im_reset_search();
        const auto base = decoder.sentence(in.text);
        std::vector<Constraint> constraints;
        if (round % 10 < 3 && !in.text.empty())
        {
            const auto count = 1 + ctx.rng.below(2);
            const auto base_chars = utf8_chars(base);
            for (std::uint32_t k = 0; k < count; ++k)
            {
                Constraint c;
                const auto syllable = in.starts.empty() ? 0 : ctx.rng.below(static_cast<std::uint32_t>(in.starts.size()));
                c.offset = in.starts.empty() ? ctx.rng.below(static_cast<std::uint32_t>(in.text.size())) : in.starts[syllable];
                if (!in.starts.empty())
                {
                    // 约束的汉字取三种来源：整句解出的字（约束不改变结果）、这个音节单独解出的字（常与整句里的不同，
                    // 约束会改写结果）、什么都不放（逼出空结果）。
                    const auto roll = ctx.rng.below(8);
                    if (roll < 2 && base_chars.size() == in.starts.size())
                        c.allowed.insert(base_chars[syllable]);
                    else if (roll < 6)
                    {
                        const auto end = syllable + 1 < in.starts.size() ? in.starts[syllable + 1] : in.text.size();
                        auto piece = in.text.substr(c.offset, end - c.offset);
                        while (!piece.empty() && piece.back() == '\'')
                            piece.pop_back();
                        ime_pinyin::im_reset_search();
                        for (const auto &hz : utf8_chars(decoder.sentence(piece)))
                            c.allowed.insert(hz);
                    }
                }
                const auto extra = ctx.rng.below(6);
                for (std::uint32_t e = 0; e < extra && !ctx.pool.empty(); ++e)
                    c.allowed.insert(ctx.pool[ctx.rng.below(static_cast<std::uint32_t>(ctx.pool.size()))]);
                for (const auto &hz : c.allowed)
                    c.allowed_text += hz;
                constraints.push_back(std::move(c));
            }
            // 同一个偏移的约束并存时 C++ 按下标逐条套，不去重；这里不特别避开。
        }
        for (const auto &hz : utf8_chars(base))
            if (ctx.pool.size() < 400 && ctx.rng.below(8) == 0)
                ctx.pool.push_back(hz);
        ime_pinyin::im_reset_search();
        const auto decoder_constraints = to_decoder(constraints);
        const auto ret = decoder.sentence(in.text, decoder_constraints);
        ++total;
        constrained += !constraints.empty();
        nonempty += !ret.empty();
        out << "{\"kind\":\"sentence\",\"pinyin\":" << quote(in.text) << ",\"cons\":" << constraints_json(constraints) << ",\"ret\":" << quote(ret) << "}\n";
    }

    // typing
    for (int round = 0; round < 500; ++round)
    {
        auto in = make_input(ctx);
        if (in.text.empty() || in.text.find('\0') != std::string::npos)
            continue;
        if (in.text.size() > 100)
            in.text.resize(100);
        ime_pinyin::im_reset_search();
        std::string rets = "[";
        std::size_t steps = 0;
        const auto step = [&](std::size_t length) {
            rets += (steps++ ? "," : "") + quote(decoder.sentence(in.text.substr(0, length)));
        };
        for (std::size_t length = 1; length <= in.text.size(); ++length)
            step(length);
        for (std::size_t length = in.text.size(); length-- > 1;)
            step(length);
        out << "{\"kind\":\"typing\",\"pinyin\":" << quote(in.text) << ",\"rets\":" << rets << "]}\n";
    }
    std::cerr << "splid 记录数见文件；sentence 记录 " << total << "，带约束 " << constrained << "，解出句子 " << nonempty << "\n";
    return 0;
}
