// 直接辅助码拼写图探针：用真实的 direct_helpcode::build_spelling_graph / single_path / next_spelling_position
// 跑一批随机输入串，当 Rust 实现的行为规格。
//
// 辅码谓词用一个确定的哈希函数代替真实的辅码表（Rust 侧写同一个函数）：mode 0 是空谓词，
// mode 1-3 是哈希模 2/3/5 不为零，mode 4 恒真（路径最多）。输入串由方案里真能解出音节的两键码拼成，
// 掺入辅码字母、斜杠、分号、手动分隔符和大小写。
//
// 用法：msime_spelling_graph_probe [输出文件]
#include "direct_helpcode/spelling_graph.h"
#include "shuangpin/shuangpin_utils.h"

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
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}

bool aux_predicate(int mode, const std::string &quanpin, char first, char second)
{
    if (mode == 4)
        return true;
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&](unsigned char c) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    };
    for (const unsigned char c : quanpin)
        mix(c);
    mix(static_cast<unsigned char>(first));
    mix(static_cast<unsigned char>(second));
    static const std::uint64_t divisors[] = {0, 2, 3, 5};
    return hash % divisors[mode] != 0;
}

std::string random_typed(Lcg &rng, const std::vector<std::string> &codes)
{
    std::string typed;
    for (std::uint32_t i = 0, pieces = 1 + rng.below(5); i < pieces; ++i)
    {
        switch (rng.below(10))
        {
        case 0:
            typed += "'";
            break;
        case 1:
            typed += "/";
            break;
        case 2:
            typed += rng.below(4) == 0 ? "1" : ";"; // 数字既不是字母也不是分隔符，走不通时整串返回空图
            break;
        case 3:
            typed += static_cast<char>('a' + rng.below(26));
            break;
        case 4:
            typed += static_cast<char>('A' + rng.below(26));
            break;
        default:
        {
            std::string code = codes[rng.below(static_cast<std::uint32_t>(codes.size()))];
            if (rng.below(6) == 0)
                code[rng.below(2)] = static_cast<char>(std::toupper(static_cast<unsigned char>(code[rng.below(2)])));
            typed += code;
            // 辅码：一位或两位，第二位常大写，后面可补斜杠。
            if (rng.below(2))
            {
                typed += static_cast<char>('a' + rng.below(26));
                if (rng.below(2))
                {
                    typed += static_cast<char>((rng.below(3) ? 'a' : 'A') + rng.below(26));
                    if (rng.below(3) == 0)
                        typed += "/";
                }
            }
            break;
        }
        }
    }
    return typed;
}

const char *kind_name(direct_helpcode::SpellingKind kind)
{
    using direct_helpcode::SpellingKind;
    switch (kind)
    {
    case SpellingKind::Plain: return "plain";
    case SpellingKind::Aux1: return "aux1";
    case SpellingKind::Aux2Slash: return "aux2_slash";
    case SpellingKind::Aux2Upper: return "aux2_upper";
    case SpellingKind::Aux2Abbrev: return "aux2_abbrev";
    case SpellingKind::Initial: return "initial";
    }
    return "";
}

std::string spelling_json(const direct_helpcode::SyllableSpelling &s)
{
    return "[" + std::to_string(s.begin) + "," + std::to_string(s.end) + "," + quote(s.code) + "," + quote(s.quanpin) + "," +
           std::to_string(static_cast<int>(s.first)) + "," + std::to_string(static_cast<int>(s.second)) + "," + quote(kind_name(s.kind)) + "]";
}
} // namespace

int main(int argc, char **argv)
{
    std::ofstream file;
    if (argc > 1)
        file.open(argv[1], std::ios::binary);
    std::ostream &out = argc > 1 ? static_cast<std::ostream &>(file) : std::cout;

    static const char *names[] = {"xiaohe", "ziranma", "shoudao", "microsoft", "sogou", "ziguang", "zhinengabc", "guobiao", "pinyinjiajia"};
    Lcg rng{0x59e1119aULL};
    int rows = 0, nonempty = 0, single = 0, lenient = 0, with_aux = 0;
    for (const char *name : names)
    {
        const ShuangpinProfile &profile = GetShuangpinProfile(name);
        std::vector<std::string> codes;
        const std::string keys = "abcdefghijklmnopqrstuvwxyz;";
        for (const char a : keys)
            for (const char b : keys)
                if (a != ';' && ShuangpinUtil::is_accepted_syllable_code(std::string{a, b}, profile))
                    codes.push_back(std::string{a, b});
        for (int scenario = 0; scenario < 330; ++scenario)
        {
            const std::string typed = random_typed(rng, codes);
            const int mode = static_cast<int>(rng.below(5));
            direct_helpcode::SpellingOptions options;
            options.slash_marker = rng.below(4) != 0;
            options.uppercase_marker = rng.below(2) == 0;
            direct_helpcode::AuxPredicate predicate;
            if (mode != 0)
                predicate = [mode](const std::string &quanpin, char first, char second) { return aux_predicate(mode, quanpin, first, second); };
            const auto graph = direct_helpcode::build_spelling_graph(typed, profile, predicate, options);
            std::vector<direct_helpcode::SyllableSpelling> path;
            const bool is_single = direct_helpcode::single_path(graph, path);

            std::string edges = "[", skip = "[", next = "[", path_json = "[";
            for (std::size_t pos = 0; pos < graph.edges.size(); ++pos)
            {
                edges += std::string(pos ? "," : "") + "[";
                for (std::size_t i = 0; i < graph.edges[pos].size(); ++i)
                {
                    edges += std::string(i ? "," : "") + spelling_json(graph.edges[pos][i]);
                    with_aux += graph.edges[pos][i].has_aux();
                }
                edges += "]";
                skip += std::string(pos ? "," : "") + (graph.skip[pos] ? "1" : "0");
            }
            for (std::size_t pos = 0; pos <= graph.size && !graph.empty(); ++pos)
                next += std::string(pos ? "," : "") + std::to_string(direct_helpcode::next_spelling_position(graph, pos));
            for (std::size_t i = 0; i < path.size(); ++i)
                path_json += std::string(i ? "," : "") + spelling_json(path[i]);
            edges += "]";
            skip += "]";
            next += "]";
            path_json += "]";
            ++rows;
            nonempty += !graph.empty();
            single += is_single;
            out << "{\"kind\":\"graph\",\"profile\":" << quote(name) << ",\"typed\":" << quote(typed) << ",\"mode\":" << mode
                << ",\"slash\":" << (options.slash_marker ? "true" : "false") << ",\"upper\":" << (options.uppercase_marker ? "true" : "false")
                << ",\"size\":" << graph.size << ",\"empty\":" << (graph.empty() ? "true" : "false") << ",\"edges\":" << edges << ",\"skip\":" << skip
                << ",\"next\":" << next << ",\"single\":" << (is_single ? "true" : "false") << ",\"path\":" << path_json << "}\n";
        }
    }
    std::cerr << rows << " 条；成图 " << nonempty << "，唯一路径 " << single << "，带辅码的边 " << with_aux << "\n";
    return 0;
}
