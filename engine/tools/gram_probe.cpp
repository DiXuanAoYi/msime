// octagram .gram 探针：真实的 400MB 模型是另外下载的，没法放进仓库，所以这里用 darts-clone 现建一份小的
// 合成 .gram（格式和真模型一样：Metadata + 双数组镜像，键是编码后的“上下文 + 词”），再用真实的 gram::GramDb
// 在它上面跑编码、码点辅助函数和 query，当 Rust 读取器的行为规格。
//
// 用法：msime_gram_probe <合成 .gram 输出路径> [JSONL 输出文件]
// 输出 kind：encode、codepoints、query。字节串一律写成十六进制。
#include "ngram/octagram/octagram_gram.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

std::string utf8_of(char32_t cp)
{
    std::string out;
    if (cp < 0x80)
        out.push_back(static_cast<char>(cp));
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

std::string hex(const std::string &bytes)
{
    std::string out;
    char buffer[4];
    for (const unsigned char c : bytes)
    {
        std::snprintf(buffer, sizeof(buffer), "%02x", c);
        out += buffer;
    }
    return out;
}

std::string number(double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

// 字符池：常用汉字区（两字节编码）、4000 以下和 A000 以上（变长编码）、ASCII、补充平面。
std::vector<std::string> make_pool()
{
    std::vector<std::string> pool;
    for (char32_t cp = 0x4E00; cp < 0x4E00 + 36; ++cp)
        pool.push_back(utf8_of(cp));
    for (char32_t cp : {char32_t(0x5000), char32_t(0x5100), char32_t(0x6000), char32_t(0x9FA0), char32_t(0x3400), char32_t(0x3B00), char32_t(0xA001),
                        char32_t(0xAC00), char32_t(0x3FFF), char32_t(0x4000), char32_t(0x4100), char32_t(0x9FFF), char32_t(0xA000), char32_t(0x20000), char32_t(0x2A700), char32_t(0x3042), char32_t(0x30A2), char32_t(0xFF21)})
        pool.push_back(utf8_of(cp));
    for (const char *ascii : {"a", "b", "1", " "})
        pool.push_back(ascii);
    return pool;
}

std::string random_text(Lcg &rng, const std::vector<std::string> &pool, std::uint32_t min_length, std::uint32_t max_length)
{
    std::string text;
    const std::uint32_t length = min_length + rng.below(max_length - min_length + 1);
    for (std::uint32_t i = 0; i < length; ++i)
        text += pool[rng.below(static_cast<std::uint32_t>(pool.size()))];
    return text;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "用法：msime_gram_probe <合成 .gram 输出路径> [JSONL 输出文件]\n";
        return 2;
    }
    std::ofstream file;
    if (argc > 2)
        file.open(argv[2], std::ios::binary);
    std::ostream &out = argc > 2 ? static_cast<std::ostream &>(file) : std::cout;

    const auto pool = make_pool();
    Lcg rng{0x0c7a62a0ULL};

    // 合成模型：键 = encode(上下文) + encode(词) 或 encode(词) + "$"；值 = 随机的 int(ln(频次)*10000)。
    std::map<std::string, int> entries;
    std::vector<std::string> sources;
    for (int i = 0; i < 6000; ++i)
    {
        const std::string text = random_text(rng, pool, 2, 7);
        sources.push_back(text);
        entries[gram::GramDb::encode(text)] = static_cast<int>(rng.below(120000));
        if (rng.below(4) == 0)
            entries[gram::GramDb::encode(text) + "$"] = static_cast<int>(rng.below(90000));
    }
    std::vector<const char *> keys;
    std::vector<int> values;
    for (const auto &[key, value] : entries)
    {
        keys.push_back(key.c_str());
        values.push_back(value);
    }
    Darts::DoubleArray trie;
    trie.build(keys.size(), keys.data(), nullptr, values.data());

    {
        std::string image(44, '\0');
        std::memcpy(image.data(), "Rime::Grammar/1.0", 17);
        const std::uint32_t checksum = 0;
        const std::uint32_t units = static_cast<std::uint32_t>(trie.size());
        const std::int32_t offset = 4; // 相对 offset 字段自身：数组紧跟在头部之后
        std::memcpy(image.data() + 32, &checksum, 4);
        std::memcpy(image.data() + 36, &units, 4);
        std::memcpy(image.data() + 40, &offset, 4);
        image.append(static_cast<const char *>(trie.array()), trie.total_size());
        std::ofstream gram_file(argv[1], std::ios::binary);
        gram_file.write(image.data(), static_cast<std::streamsize>(image.size()));
    }

    gram::GramDb db;
    if (!db.open(argv[1]))
    {
        std::cerr << "合成模型打不开：" << db.error() << "\n";
        return 1;
    }

    // 编码：合法文本和带乱码字节的文本（续字节缺位、孤立续字节、截断）。
    for (int i = 0; i < 300; ++i)
    {
        std::string text = random_text(rng, pool, 0, 8);
        if (rng.below(6) == 0)
        {
            const char junk[] = {'\x80', '\xC3', '\xE4', '\xF0', '\xBF', '\xFF', '\0'};
            text.push_back(junk[rng.below(6)]);
            if (rng.below(2))
                text += pool[rng.below(static_cast<std::uint32_t>(pool.size()))];
        }
        if (rng.below(20) == 0)
            text.insert(rng.below(static_cast<std::uint32_t>(text.size() + 1)), 1, '\0');
        const std::string encoded = gram::GramDb::encode(text);
        out << "{\"kind\":\"encode\",\"text\":\"" << hex(text) << "\",\"encoded\":\"" << hex(encoded) << "\",\"count\":" << gram::codepoint_count(text)
            << ",\"unicode_length\":" << gram::unicode_length(encoded, encoded.size()) << ",\"first3\":\"" << hex(gram::first_codepoints(text, 3))
            << "\",\"last3\":\"" << hex(gram::last_codepoints(text, 3)) << "\"}\n";
    }

    // 查询：一半从模型里的词条切出“上下文 + 词”（会命中），一半随机。
    int hits = 0;
    for (int i = 0; i < 2500; ++i)
    {
        std::string context, word;
        if (rng.below(2) == 0)
        {
            // 把一条来源文本在某个字符边界上一分为二，必要时在上下文前面垫几个字，在词后面接几个字。
            const std::string &text = sources[rng.below(static_cast<std::uint32_t>(sources.size()))];
            std::vector<std::size_t> boundaries{0};
            for (std::size_t p = 0; p < text.size();)
            {
                const unsigned char lead = static_cast<unsigned char>(text[p]);
                p += lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
                boundaries.push_back(std::min(p, text.size()));
            }
            const std::size_t cut = boundaries[1 + rng.below(static_cast<std::uint32_t>(boundaries.size() - 1))];
            context = text.substr(0, cut);
            word = text.substr(cut);
            if (rng.below(3) == 0)
                context = random_text(rng, pool, 1, 3) + context;
            if (rng.below(3) == 0)
                word += random_text(rng, pool, 1, 4);
        }
        else
        {
            context = random_text(rng, pool, 0, 9);
            word = random_text(rng, pool, 0, 9);
        }
        gram::GrammarConfig config;
        config.collocation_max_length = 1 + static_cast<int>(rng.below(10));
        config.collocation_min_length = static_cast<int>(rng.below(5));
        config.collocation_penalty = -static_cast<double>(rng.below(30));
        config.non_collocation_penalty = -static_cast<double>(rng.below(20));
        config.weak_collocation_penalty = -static_cast<double>(rng.below(200));
        config.rear_penalty = -static_cast<double>(rng.below(30));
        const bool rear = rng.below(3) == 0;
        gram::QueryStats stats;
        const double score = db.query(context, word, rear, config, &stats);
        hits += stats.matches > 0;
        out << "{\"kind\":\"query\",\"context\":\"" << hex(context) << "\",\"word\":\"" << hex(word) << "\",\"rear\":" << (rear ? "true" : "false")
            << ",\"config\":[" << config.collocation_max_length << "," << config.collocation_min_length << "," << number(config.collocation_penalty) << ","
            << number(config.non_collocation_penalty) << "," << number(config.weak_collocation_penalty) << "," << number(config.rear_penalty)
            << "],\"score\":" << number(score) << ",\"stats\":[" << stats.context_windows << "," << stats.matched_windows << "," << stats.matches << ","
            << number(stats.best_value) << "]}\n";
    }
    std::cerr << "合成模型 " << entries.size() << " 个键，" << trie.size() << " 个单元；查询命中 " << hits << " / 2500\n";
    return 0;
}
