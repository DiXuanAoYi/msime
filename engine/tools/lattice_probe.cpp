// 词格探针：对真实的 quanpin::decode_word_lattice 和 merge_lattice_candidates 跑一批随机场景，
// 把输入（含解码过程中实际被查询到的词表）和输出一起写成 JSONL，当 Rust 实现的行为规格。
//
// 用法：msime_lattice_probe <输出文件> [<sc.lm> <词表文件> <带模型的输出文件>]
// 给了后三个参数时，另外导出带真实 kenlm 模型的解码场景（kind 为 decode_lm）。
//
// 场景由固定种子的线性同余生成，重复运行逐字节相同。不加载语言模型：C++ 的 LanguageModel 不能被替换，
// 所以这里核对的是无模型时的启发式打分、惩罚分、字级搭配回调、字约束、束宽与 n-best 去重，以及合并进候选列表的全部规则。
// 每个场景里所有词的汉字和权重都互不相同、音节也不重复，避免 std::sort / partial_sort 在平局上的实现定义行为影响比较。
//
// 输出两类行：
//   decode  {"kind":"decode","syllables":[…],"options":{…},"constraints":[…],"lexicon":{跨度:[行…]},
//            "constrained":{跨度:[行…]},"paths":[…]}
//   merge   {"kind":"merge","syllables":[…],"typed":…,"options":{…},"lexicon":{…},"candidates":[…],
//            "rerankers":[{"source":…,"mode":…}…],"result":[…]}
#include "ngram/language_model.h"
#include "quanpin/word_lattice.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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

std::string utf8(std::uint32_t cp)
{
    std::string out;
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

std::uint32_t first_codepoint(const std::string &text)
{
    if (text.empty())
        return 0;
    const auto lead = static_cast<unsigned char>(text[0]);
    if (lead < 0x80)
        return lead;
    if ((lead >> 5) == 0x6)
        return ((lead & 0x1F) << 6) | (static_cast<unsigned char>(text[1]) & 0x3F);
    if ((lead >> 4) == 0xE)
        return ((lead & 0x0F) << 12) | ((static_cast<unsigned char>(text[1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(text[2]) & 0x3F);
    return ((lead & 0x07) << 18) | ((static_cast<unsigned char>(text[1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(text[2]) & 0x3F) << 6) | (static_cast<unsigned char>(text[3]) & 0x3F);
}

std::uint32_t last_codepoint(const std::string &text)
{
    std::size_t i = text.size();
    while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xC0) == 0x80)
        --i;
    return i == 0 ? 0 : first_codepoint(text.substr(i - 1));
}

std::size_t count_codepoints(const std::string &text)
{
    std::size_t n = 0;
    for (const unsigned char c : text)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

// 字级搭配回调，Rust 测试里有一份逐字相同的实现。
double collocation(std::string_view tail, std::string_view word, bool rear)
{
    const std::string tail_text(tail), word_text(word);
    const std::uint32_t value = last_codepoint(tail_text) * 31u + first_codepoint(word_text) * 17u +
                                static_cast<std::uint32_t>(count_codepoints(tail_text)) * 3u + (rear ? 5u : 0u);
    return (static_cast<int>(value % 13u) - 6) * 0.25;
}

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

std::string number(double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

std::string strings(const std::vector<std::string> &items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? "," : "") + quote(items[i]);
    return out + "]";
}

const std::vector<std::string> kSyllables = {"ni",   "hao",  "wo",   "men",  "shi",   "zhong", "guo", "ren", "ta",  "de",
                                             "yi",   "ge",   "you",  "bu",   "zai",   "le",    "da",  "xue", "sheng", "jia",
                                             "qing", "xiang", "fa",  "zhan", "gong",  "zuo",   "kan", "lai", "hui", "tian"};

struct Lexicon
{
    Lcg rng;
    std::uint32_t next_codepoint = 0x4E00;
    std::uint32_t counter = 0;
    bool zero_weight_used = false;
    // 非空时词条的值取自真词（按字数分组），语言模型才有东西可打分。
    const std::vector<std::vector<std::string>> *pool = nullptr;
    std::map<std::string, std::vector<quanpin::LatticeLexeme>> rows;

    // 一个场景里所有行的汉字、权重都不重复。
    std::vector<quanpin::LatticeLexeme> make(const quanpin::Segments &span, bool wrong_length_allowed)
    {
        std::string key;
        for (std::size_t i = 0; i < span.size(); ++i)
            key += (i ? "'" : "") + span[i];
        std::vector<quanpin::LatticeLexeme> out;
        // 单音节跨度行多一点，长词少一点，贴近真实词库。
        const std::uint32_t count = span.size() == 1 ? 1 + rng.below(5) : rng.below(4);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            quanpin::LatticeLexeme row;
            std::size_t chars = span.size();
            if (wrong_length_allowed && rng.below(8) == 0)
                chars = chars + 1;
            if (pool && chars < pool->size() && !(*pool)[chars].empty())
                row.value = (*pool)[chars][rng.below(static_cast<std::uint32_t>((*pool)[chars].size()))];
            else if (pool && !(*pool)[1].empty())
                for (std::size_t c = 0; c < chars; ++c)
                    row.value += (*pool)[1][rng.below(static_cast<std::uint32_t>((*pool)[1].size()))];
            else
                for (std::size_t c = 0; c < chars; ++c)
                    row.value += utf8(next_codepoint++);
            row.weight = static_cast<std::int64_t>(1 + rng.below(1u << 20)) * 1024 + (counter++ & 1023);
            if (!zero_weight_used && rng.below(40) == 0)
            {
                row.weight = 0;
                zero_weight_used = true;
            }
            row.key = rng.below(6) == 0 ? std::string() : key; // 空键由解码器补成跨度键
            if (rng.below(7) == 0)
                row.penalty = 0.5 + static_cast<double>(rng.below(1000)) / 100.0 + counter * 1e-6;
            if (rng.below(5) == 0)
                row.input_domains.push_back("domain" + std::to_string(rng.below(3)));
            if (rng.below(9) == 0)
                row.input_domains.push_back("domain" + std::to_string(rng.below(3)));
            out.push_back(std::move(row));
        }
        return out;
    }
};

std::string rows_json(const std::map<std::string, std::vector<quanpin::LatticeLexeme>> &rows)
{
    std::string out = "{";
    bool first_key = true;
    for (const auto &[key, list] : rows)
    {
        out += (first_key ? "" : ",") + quote(key) + ":[";
        first_key = false;
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            const auto &row = list[i];
            out += (i ? "," : "") + std::string("{\"key\":") + quote(row.key) + ",\"value\":" + quote(row.value) +
                   ",\"weight\":" + std::to_string(row.weight) + ",\"penalty\":" + number(row.penalty) +
                   ",\"domains\":" + strings(row.input_domains) + "}";
        }
        out += "]";
    }
    return out + "}";
}

std::string paths_json(const std::vector<quanpin::LatticePath> &paths)
{
    std::string out = "[";
    for (std::size_t i = 0; i < paths.size(); ++i)
    {
        const auto &path = paths[i];
        out += (i ? "," : "") + std::string("{\"sentence\":") + quote(path.sentence) + ",\"key\":" + quote(path.key) +
               ",\"log_prob\":" + number(path.log_prob) + ",\"words\":" + strings(path.words) +
               ",\"domains\":" + strings(path.input_domains) + "}";
    }
    return out + "]";
}

quanpin::Segments random_syllables(Lcg &rng, std::size_t min_count, std::size_t max_count)
{
    // 不放回地取：同一个音节出现两次会让它们共用一份词表，不同排列的总分恰好相等，
    // 而 std::sort 对平局的次序是实现定义的，没法当规格。
    quanpin::Segments out;
    const std::size_t count = min_count + rng.below(static_cast<std::uint32_t>(max_count - min_count + 1));
    std::vector<std::string> pool = kSyllables;
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t pick = rng.below(static_cast<std::uint32_t>(pool.size()));
        out.push_back(pool[pick]);
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(pick));
    }
    return out;
}

struct Constraint
{
    std::size_t syllable;
    std::uint32_t modulus;
    std::uint32_t reject;
};

const char *source_name(CandidateSource source)
{
    switch (source)
    {
    case CandidateSource::Database:
        return "Database";
    case CandidateSource::UserDatabase:
        return "UserDatabase";
    case CandidateSource::Fallback:
        return "Fallback";
    case CandidateSource::Generated:
        return "Generated";
    case CandidateSource::Collocation:
        return "Collocation";
    case CandidateSource::NeuralDesktop:
        return "NeuralDesktop";
    case CandidateSource::NeuralKeyboard:
        return "NeuralKeyboard";
    case CandidateSource::QuickPhrase:
        return "QuickPhrase";
    case CandidateSource::Emoji:
        return "Emoji";
    default:
        return "Other";
    }
}

std::string options_json(const quanpin::WordLatticeOptions &o, bool collocation_on)
{
    return std::string("{\"beam\":") + std::to_string(o.beam) + ",\"nbest\":" + std::to_string(o.nbest) +
           ",\"span_limit\":" + std::to_string(o.span_limit) +
           ",\"max_phrase_syllables\":" + std::to_string(o.max_phrase_syllables) +
           ",\"unigram_z\":" + number(o.unigram_z) + ",\"phrase_length_bonus\":" + number(o.phrase_length_bonus) +
           ",\"collocation\":" + (collocation_on ? "true" : "false") +
           ",\"collocation_weight\":" + number(o.collocation_weight) +
           ",\"include_lattice_best\":" + (o.include_lattice_best ? "true" : "false") +
           ",\"include_collocation_best\":" + (o.include_collocation_best ? "true" : "false") +
           ",\"show_next_on_duplicate\":" + (o.show_next_on_duplicate ? "true" : "false") +
           ",\"language_model\":" + (o.language_model ? "true" : "false") +
           ",\"dictionary_tiebreak\":" + number(o.dictionary_tiebreak) + ",\"reading_prior\":" + number(o.reading_prior) +
           ",\"reading_prior_share\":" + number(o.reading_prior_share) +
           ",\"reading_prior_floor\":" + number(o.reading_prior_floor) + "}";
}

quanpin::WordLatticeOptions random_options(Lcg &rng, bool &collocation_on)
{
    quanpin::WordLatticeOptions o;
    o.beam = rng.below(4) == 0 ? 1 + static_cast<int>(rng.below(4)) : 8 + static_cast<int>(rng.below(32));
    o.nbest = 1 + static_cast<int>(rng.below(6));
    o.span_limit = rng.below(5) == 0 ? 1 + static_cast<int>(rng.below(3)) : 32;
    o.max_phrase_syllables = rng.below(4) == 0 ? 1 + static_cast<int>(rng.below(3)) : 7;
    o.unigram_z = rng.below(5) == 0 ? 0.5 : 1e6 * (1 + rng.below(9));
    o.phrase_length_bonus = 1.0 + static_cast<double>(rng.below(60)) / 10.0;
    collocation_on = rng.below(2) == 0;
    if (collocation_on)
    {
        o.collocation_scorer = [](std::string_view tail, std::string_view word, bool rear) {
            return collocation(tail, word, rear);
        };
        o.collocation_weight = rng.below(6) == 0 ? 0.0 : 0.1 + static_cast<double>(rng.below(30)) / 10.0;
    }
    return o;
}
void emit_decode_scenarios(std::ostream &out, const char *kind, std::uint64_t seed_base, int count,
                          const ngram::LanguageModel *model, const std::vector<std::vector<std::string>> *pool)
{
    // 带模型时未登录词的罚分相同，容易出现分数完全相等的路径；C++ 的 partial_sort 对平局的次序没有规定，
    // 所以这类场景（含平局恰好卡在 n-best 边界的）不进基线，序号顺延到凑够 count 个。
    for (int scenario = 0, emitted = 0; emitted < count; ++scenario)
    {
        Lcg rng{seed_base + static_cast<std::uint64_t>(scenario)};
        bool collocation_on = false;
        auto options = random_options(rng, collocation_on);
        if (model)
        {
            // 带模型的场景：另用一条随机序列调读音先验等参数，免得动老场景的序列。
            Lcg extra{seed_base * 3 + static_cast<std::uint64_t>(scenario)};
            options.language_model = model;
            options.dictionary_tiebreak = extra.below(4) == 0 ? 0.0 : 0.002 * (1 + extra.below(20));
            options.reading_prior = extra.below(4) == 0 ? 0.0 : 0.5 + extra.below(10) / 4.0;
            options.reading_prior_share = extra.below(5) == 0 ? 0.0 : 0.001 * (1 + extra.below(100));
            options.reading_prior_floor = extra.below(5) == 0 ? 0.0 : 1.0 + extra.below(60) / 10.0;
        }
        const auto syllables = random_syllables(rng, 1, 8);

        std::vector<Constraint> constraints;
        if (rng.below(3) == 0)
        {
            const std::size_t pinned = rng.below(static_cast<std::uint32_t>(syllables.size()));
            constraints.push_back({pinned, 2 + rng.below(3), rng.below(2)});
            if (syllables.size() > 3 && rng.below(3) == 0)
                constraints.push_back({(pinned + 2) % syllables.size() > pinned ? (pinned + 2) % syllables.size() : pinned + 1,
                                       2 + rng.below(3), rng.below(2)});
            std::sort(constraints.begin(), constraints.end(),
                      [](const Constraint &a, const Constraint &b) { return a.syllable < b.syllable; });
            constraints.erase(std::unique(constraints.begin(), constraints.end(),
                                          [](const Constraint &a, const Constraint &b) { return a.syllable == b.syllable; }),
                              constraints.end());
        }
        for (const auto &c : constraints)
        {
            quanpin::LatticeCharConstraint constraint;
            constraint.syllable = c.syllable;
            constraint.accept = [c](const std::string &hanzi) { return first_codepoint(hanzi) % c.modulus != c.reject; };
            options.char_constraints.push_back(constraint);
        }
        const bool separate_constrained_lookup = !constraints.empty() && rng.below(2) == 0;

        Lexicon lexicon{Lcg{rng.next() + 1ULL}};
        lexicon.pool = pool;
        Lexicon constrained_lexicon{Lcg{rng.next() + 2ULL}};
        constrained_lexicon.next_codepoint = 0x6000;
        constrained_lexicon.pool = pool;
        const auto lookup = [&](const quanpin::Segments &span) {
            std::string key;
            for (std::size_t i = 0; i < span.size(); ++i)
                key += (i ? "'" : "") + span[i];
            auto found = lexicon.rows.find(key);
            if (found == lexicon.rows.end())
                found = lexicon.rows.emplace(key, lexicon.make(span, true)).first;
            return found->second;
        };
        if (separate_constrained_lookup)
        {
            options.constrained_lookup = [&](const quanpin::Segments &span) {
                std::string key;
                for (std::size_t i = 0; i < span.size(); ++i)
                    key += (i ? "'" : "") + span[i];
                auto found = constrained_lexicon.rows.find(key);
                if (found == constrained_lexicon.rows.end())
                    found = constrained_lexicon.rows.emplace(key, constrained_lexicon.make(span, false)).first;
                return found->second;
            };
        }

        const auto paths = quanpin::decode_word_lattice(syllables, lookup, options);
        if (model)
        {
            auto wider = options;
            wider.nbest = options.nbest + 4;
            const auto more = quanpin::decode_word_lattice(syllables, lookup, wider);
            bool ambiguous = more.size() < paths.size();
            for (std::size_t i = 0; !ambiguous && i < more.size() && i <= paths.size(); ++i)
            {
                if (i < paths.size() && more[i].sentence != paths[i].sentence)
                    ambiguous = true;
                if (i > 0 && more[i].log_prob == more[i - 1].log_prob)
                    ambiguous = true;
            }
            if (ambiguous)
                continue;
        }
        ++emitted;

        std::string constraint_json = "[";
        for (std::size_t i = 0; i < constraints.size(); ++i)
            constraint_json += (i ? "," : "") + std::string("{\"syllable\":") + std::to_string(constraints[i].syllable) +
                               ",\"modulus\":" + std::to_string(constraints[i].modulus) +
                               ",\"reject\":" + std::to_string(constraints[i].reject) + "}";
        constraint_json += "]";
        out << "{\"kind\":\"" << kind << "\",\"syllables\":" << strings(syllables)
            << ",\"options\":" << options_json(options, collocation_on) << ",\"constraints\":" << constraint_json
            << ",\"separate_constrained_lookup\":" << (separate_constrained_lookup ? "true" : "false")
            << ",\"lexicon\":" << rows_json(lexicon.rows) << ",\"constrained\":" << rows_json(constrained_lexicon.rows)
            << ",\"paths\":" << paths_json(paths) << "}\n";
    }
}
} // namespace

// 用法：msime_lattice_probe <输出> [<sc.lm> <词表文件> <带模型的输出>]
int main(int argc, char **argv)
{
    std::ofstream file;
    if (argc > 1)
        file.open(argv[1], std::ios::binary);
    std::ostream &out = argc > 1 ? static_cast<std::ostream &>(file) : std::cout;

    emit_decode_scenarios(out, "decode", 0xd3c0de00ULL, 150, nullptr, nullptr);

    // 合并场景
    const CandidateSource candidate_sources[] = {CandidateSource::Database, CandidateSource::UserDatabase,
                                                 CandidateSource::Fallback, CandidateSource::Generated,
                                                 CandidateSource::QuickPhrase, CandidateSource::Emoji};
    const CandidateSource reranker_sources[] = {CandidateSource::NeuralDesktop, CandidateSource::NeuralKeyboard,
                                                CandidateSource::Collocation, CandidateSource::Generated};
    for (int scenario = 0; scenario < 150; ++scenario)
    {
        Lcg rng{0xa11ce000ULL + static_cast<std::uint64_t>(scenario)};
        bool collocation_on = false;
        auto options = random_options(rng, collocation_on);
        options.include_lattice_best = rng.below(5) != 0;
        options.include_collocation_best = rng.below(3) == 0;
        options.show_next_on_duplicate = rng.below(2) == 0;
        options.nbest = 2 + static_cast<int>(rng.below(5));
        auto syllables = random_syllables(rng, 1, 5);
        if (rng.below(12) == 0)
            syllables.push_back("zh"); // 不完整的音节：不合并
        std::string typed;
        for (const auto &s : syllables)
            typed += s;
        std::string joined;
        for (std::size_t i = 0; i < syllables.size(); ++i)
            joined += (i ? "'" : "") + syllables[i];

        Lexicon lexicon{Lcg{rng.next() + 3ULL}};
        const auto lookup = [&](const quanpin::Segments &span) {
            std::string key;
            for (std::size_t i = 0; i < span.size(); ++i)
                key += (i ? "'" : "") + span[i];
            auto found = lexicon.rows.find(key);
            if (found == lexicon.rows.end())
                found = lexicon.rows.emplace(key, lexicon.make(span, false)).first;
            return found->second;
        };

        // 已有候选：词库命中（键等于输入或不等）、Unigram 兜底、别的来源。词里偶尔复用词格里会出现的整句。
        std::vector<WordItem> candidates;
        const std::uint32_t existing = rng.below(7);
        for (std::uint32_t i = 0; i < existing; ++i)
        {
            const CandidateSource source = candidate_sources[rng.below(6)];
            std::string word;
            const std::size_t chars = 1 + rng.below(4);
            for (std::size_t c = 0; c < chars; ++c)
                word += utf8(0x4E00 + 3000 + rng.below(200));
            WordItem item(typed, word, 1000 - static_cast<std::int64_t>(i), source,
                          rng.below(3) == 0 ? std::string() : (rng.below(2) == 0 ? joined : joined + "'x"));
            candidates.push_back(item);
        }
        struct Spec
        {
            CandidateSource source;
            int mode;
        };
        std::vector<Spec> specs;
        const std::uint32_t reranker_count = rng.below(4);
        for (std::uint32_t i = 0; i < reranker_count; ++i)
            specs.push_back({reranker_sources[rng.below(4)], static_cast<int>(rng.below(4))});

        std::vector<quanpin::SourcedLatticeReranker> rerankers;
        for (const auto &spec : specs)
        {
            quanpin::SourcedLatticeReranker entry;
            entry.source = spec.source;
            const int mode = spec.mode;
            entry.rerank = [mode](std::vector<quanpin::LatticePath> &paths) {
                if (mode == 0)
                    return false;
                if (mode == 1)
                    std::reverse(paths.begin(), paths.end());
                else if (mode == 2 && paths.size() > 1)
                    std::rotate(paths.begin(), paths.begin() + 1, paths.end());
                return true;
            };
            rerankers.push_back(std::move(entry));
        }

        auto before = candidates;
        auto result = candidates;
        quanpin::merge_lattice_candidates(result, syllables, lookup, typed, options, rerankers);

        const auto items_json = [](const std::vector<WordItem> &items) {
            std::string text = "[";
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                const auto &item = items[i];
                text += (i ? "," : "") + std::string("{\"pinyin\":") + quote(item.pinyin) + ",\"word\":" + quote(item.word) +
                        ",\"weight\":" + std::to_string(item.weight) + ",\"source\":" + quote(source_name(item.source)) +
                        ",\"canonical\":" + quote(item.canonical_pinyin) +
                        ",\"sentence_association\":" + (item.sentence_association ? "true" : "false") +
                        ",\"domains\":" + strings(item.input_domains) + "}";
            }
            return text + "]";
        };
        std::string specs_json = "[";
        for (std::size_t i = 0; i < specs.size(); ++i)
            specs_json += (i ? "," : "") + std::string("{\"source\":") + quote(source_name(specs[i].source)) +
                          ",\"mode\":" + std::to_string(specs[i].mode) + "}";
        specs_json += "]";
        out << "{\"kind\":\"merge\",\"syllables\":" << strings(syllables) << ",\"typed\":" << quote(typed)
            << ",\"options\":" << options_json(options, collocation_on) << ",\"lexicon\":" << rows_json(lexicon.rows)
            << ",\"candidates\":" << items_json(before) << ",\"rerankers\":" << specs_json
            << ",\"result\":" << items_json(result) << "}\n";
    }

    // 带真实语言模型的解码场景：词条取自词表里的真词，打分走 kenlm。
    if (argc > 4)
    {
        const ngram::LanguageModel &model = ngram::shared_language_model(std::filesystem::path(argv[2]));
        if (!model.valid())
        {
            std::cerr << "语言模型载入失败：" << model.error() << "\n";
            return 1;
        }
        std::vector<std::vector<std::string>> pool(8);
        std::ifstream words(argv[3]);
        std::string line;
        while (std::getline(words, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const std::size_t length = count_codepoints(line);
            if (length >= 1 && length <= 4 && pool[length].size() < 400)
                pool[length].push_back(line);
        }
        std::ofstream model_file(argv[4], std::ios::binary);
        emit_decode_scenarios(model_file, "decode_lm", 0x1a9e0000ULL, 80, &model, &pool);
    }
    return 0;
}
