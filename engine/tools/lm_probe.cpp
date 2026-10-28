// 语言模型探针：用真实的 ngram::LanguageModel（kenlm）载入出货的 sc.lm，导出词下标和逐词累计分数，
// 当 Rust 读取器的行为规格。
//
// 用法：msime_lm_probe <sc.lm> <词表文件> [输出文件]
// 词表文件每行一个词（UTF-8）。输出 JSONL：
//   {"kind":"info","valid":true,"unknown_penalty":…}
//   {"kind":"words","rows":[[词,下标,单词分数],…]}           单词分数从无上下文起算
//   {"kind":"sentence","words":[…],"indices":[…],"null_scores":[…],"begin_scores":[…]}
//     null_scores 从 null_state 起逐词累计、begin_scores 从 begin_state 起；每个数是这一步的分数，不是累计和。
// 句子分两类，都由固定种子的线性同余生成，重复运行逐字节相同：400 条随机词序，300 条贪心链（每步挑分数最高的词，落在真实的高阶 n-gram 上）。
#include "ngram/language_model.h"

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

std::string number(float value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "用法：msime_lm_probe <sc.lm> <词表文件> [输出文件]\n";
        return 2;
    }
    const ngram::LanguageModel model{std::filesystem::path(argv[1])};
    if (!model.valid())
    {
        std::cerr << "载入失败：" << model.error() << "\n";
        return 1;
    }
    std::vector<std::string> words;
    {
        std::ifstream input(argv[2]);
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                words.push_back(line);
        }
    }
    std::ofstream file;
    if (argc > 3)
        file.open(argv[3], std::ios::binary);
    std::ostream &out = argc > 3 ? static_cast<std::ostream &>(file) : std::cout;

    out << "{\"kind\":\"info\",\"valid\":true,\"unknown_penalty\":" << number(model.unknown_penalty()) << "}\n";

    std::string rows;
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        rows += std::string(i ? "," : "") + "[" + quote(words[i]) + "," + std::to_string(model.index(words[i])) + "," +
                number(model.single_word_score(model.null_state(), words[i])) + "]";
    }
    out << "{\"kind\":\"words\",\"rows\":[" << rows << "]}\n";

    Lcg rng{0x1a2b3c4dULL};
    for (int sentence = 0; sentence < 400; ++sentence)
    {
        const std::uint32_t length = 1 + rng.below(8);
        std::vector<std::string> picked;
        for (std::uint32_t i = 0; i < length; ++i)
        {
            // 常用词（词表前部）出现得多一些，句子才会命中高阶 n-gram；偶尔夹未登录词。
            const std::uint32_t head = static_cast<std::uint32_t>(words.size() < 600 ? words.size() : 600);
            picked.push_back(words[rng.below(3) == 0 ? rng.below(static_cast<std::uint32_t>(words.size())) : rng.below(head)]);
        }
        std::string word_json = "[", index_json = "[", null_json = "[", begin_json = "[";
        ngram::State null_in = model.null_state(), begin_in = model.begin_state(), next;
        for (std::size_t i = 0; i < picked.size(); ++i)
        {
            const std::string sep = i ? "," : "";
            word_json += sep + quote(picked[i]);
            index_json += sep + std::to_string(model.index(picked[i]));
            null_json += sep + number(model.score(null_in, picked[i], next));
            null_in = next;
            begin_json += sep + number(model.score(begin_in, picked[i], next));
            begin_in = next;
        }
        out << "{\"kind\":\"sentence\",\"words\":" << word_json << "],\"indices\":" << index_json
            << "],\"null_scores\":" << null_json << "],\"begin_scores\":" << begin_json << "]}\n";
    }

    // 贪心链：每一步从随机抽的一批候选里挑当前上下文下分数最高的词，句子就会落在真实存在的二元、三元组上
    // （随机词序几乎凑不出三元组，高阶查找和 Bhiksha 范围就测不到）。
    Lcg chain_rng{0x5ca1ab1eULL};
    for (int chain = 0; chain < 300; ++chain)
    {
        const std::uint32_t head = static_cast<std::uint32_t>(words.size() < 600 ? words.size() : 600);
        std::vector<std::string> picked;
        ngram::State state = model.null_state(), next, best_state;
        picked.push_back(words[chain_rng.below(head)]);
        model.score(state, picked.back(), next);
        state = next;
        const std::uint32_t length = 4 + chain_rng.below(5);
        while (picked.size() < length)
        {
            float best_score = -1e30F;
            std::string best_word;
            for (int candidate = 0; candidate < 300; ++candidate)
            {
                const std::string &word = words[chain_rng.below(head)];
                const float value = model.score(state, word, next);
                if (value > best_score)
                {
                    best_score = value;
                    best_word = word;
                    best_state = next;
                }
            }
            picked.push_back(best_word);
            state = best_state;
        }
        std::string word_json = "[", index_json = "[", null_json = "[", begin_json = "[";
        ngram::State null_in = model.null_state(), begin_in = model.begin_state();
        for (std::size_t i = 0; i < picked.size(); ++i)
        {
            const std::string sep = i ? "," : "";
            word_json += sep + quote(picked[i]);
            index_json += sep + std::to_string(model.index(picked[i]));
            null_json += sep + number(model.score(null_in, picked[i], next));
            null_in = next;
            begin_json += sep + number(model.score(begin_in, picked[i], next));
            begin_in = next;
        }
        out << "{\"kind\":\"sentence\",\"words\":" << word_json << "],\"indices\":" << index_json
            << "],\"null_scores\":" << null_json << "],\"begin_scores\":" << begin_json << "]}\n";
    }
    return 0;
}
