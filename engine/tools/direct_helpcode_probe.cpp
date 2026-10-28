// 直接辅助码整句解码与解析器探针：用真实的 direct_helpcode::decode_best_path 和 Resolver::resolve 跑一批随机输入串，
// 当 Rust 实现的行为规格。
//
// 词库、码表和辅码谓词都用确定的哈希函数合成（Rust 侧写同一套），词条权重取哈希，彼此几乎不会相同，
// 免得束截断的平局次序（C++ 的 partial_sort 对平局没有规定）影响基线。
// 带语言模型的场景需要 sc.lm：未登录词全按同一个罚分，但状态传递的路径是真的。
//
// 用法：msime_direct_helpcode_probe [输出文件] [sc.lm]
// 输出 kind：decode（只解码）、resolve（Resolver::resolve，含改写出的请求字段）。
#include "direct_helpcode/direct_resolver.h"
#include "ngram/language_model.h"
#include "shuangpin/shuangpin_utils.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
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

std::uint64_t fnv(const std::string &text, std::uint64_t seed = 0xcbf29ce484222325ULL)
{
    std::uint64_t hash = seed;
    for (const unsigned char c : text)
    {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
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

std::string utf8_of(char32_t cp)
{
    std::string out;
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    return out;
}

// 合成汉字：40 个，码点 0x4E00 + idx * 37。
std::string hanzi(std::uint32_t idx)
{
    return utf8_of(0x4E00 + idx * 37);
}

std::string join(const quanpin::Segments &span)
{
    return quanpin::join_segments(span);
}

// 合成词库：每个跨度 2-6 行（约 1/25 的跨度没有行），字数多数等于音节数（偶尔多一个），权重取哈希。
std::vector<quanpin::LatticeLexeme> lexicon(const quanpin::Segments &span)
{
    const std::string key = join(span);
    const std::uint64_t h = fnv(key);
    std::vector<quanpin::LatticeLexeme> rows;
    const std::uint32_t count = h % 25 == 0 ? 0 : 2 + static_cast<std::uint32_t>((h >> 8) % 5);
    for (std::uint32_t r = 0; r < count; ++r)
    {
        const std::uint64_t hr = fnv(key + "#" + std::to_string(r), h);
        const std::size_t chars = span.size() + ((hr >> 8) % 8 == 0 ? 1 : 0);
        quanpin::LatticeLexeme row;
        for (std::size_t i = 0; i < chars; ++i)
            row.value += hanzi(static_cast<std::uint32_t>(fnv(key + "#" + std::to_string(r) + "/" + std::to_string(i)) % 40));
        row.key = hr % 4 == 0 ? "" : key;
        row.weight = static_cast<std::int64_t>(1 + (hr >> 16) % 5000000);
        row.penalty = hr % 11 == 0 ? 1.0 : 0.0;
        rows.push_back(std::move(row));
    }
    return rows;
}

// 合成码表：idx % 9 == 0 的字没有码，idx % 13 == 0 的字只有一码。
std::string aux_code(std::uint32_t idx)
{
    std::string code;
    code.push_back(static_cast<char>('a' + idx % 26));
    if (idx % 13 != 0)
        code.push_back(static_cast<char>('a' + (idx * 7) % 26));
    return code;
}

HelpcodeUtils::Keymap make_keymap()
{
    HelpcodeUtils::Keymap keymap;
    for (std::uint32_t idx = 0; idx < 40; ++idx)
        if (idx % 9 != 0)
            keymap[hanzi(idx)] = aux_code(idx);
    return keymap;
}

bool aux_predicate(const std::string &quanpin, char first, char second)
{
    std::uint64_t hash = fnv(quanpin);
    hash = fnv(std::string{first, second}, hash);
    return hash % 3 != 0;
}

// 字级搭配回调，与 lattice_probe 里的逐字相同。
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

double collocation(std::string_view tail, std::string_view word, bool rear)
{
    const std::string tail_text(tail), word_text(word);
    const std::uint32_t value = last_codepoint(tail_text) * 31u + first_codepoint(word_text) * 17u +
                                static_cast<std::uint32_t>(count_codepoints(tail_text)) * 3u + (rear ? 5u : 0u);
    return (static_cast<int>(value % 13u) - 6) * 0.25;
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

std::string syllables_json(const std::vector<direct_helpcode::SyllableSpelling> &syllables)
{
    std::string out = "[";
    for (std::size_t i = 0; i < syllables.size(); ++i)
        out += std::string(i ? "," : "") + "[" + std::to_string(syllables[i].begin) + "," + std::to_string(syllables[i].end) + "," +
               quote(kind_name(syllables[i].kind)) + "]";
    return out + "]";
}

std::string path_json(const std::optional<direct_helpcode::DecodedPath> &path)
{
    if (!path)
        return "null";
    std::string words = "[";
    for (std::size_t i = 0; i < path->words.size(); ++i)
        words += (i ? "," : "") + quote(path->words[i]);
    return "{\"syllables\":" + syllables_json(path->syllables) + ",\"sentence\":" + quote(path->sentence) + ",\"key\":" + quote(path->key) +
           ",\"words\":" + words + "],\"log_prob\":" + number(path->log_prob) + ",\"complete\":" + (path->complete ? "true" : "false") + "}";
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
            typed += static_cast<char>('a' + rng.below(26));
            break;
        case 3:
            typed += static_cast<char>('A' + rng.below(26));
            break;
        default:
        {
            typed += codes[rng.below(static_cast<std::uint32_t>(codes.size()))];
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

std::string options_json(const direct_helpcode::DecodeOptions &o, bool lm)
{
    return "{\"beam\":" + std::to_string(o.beam) + ",\"span_limit\":" + std::to_string(o.span_limit) + ",\"max_phrase\":" +
           std::to_string(o.max_phrase_syllables) + ",\"max_sequences\":" + std::to_string(o.max_sequences_per_start) + ",\"initial_penalty\":" +
           number(o.initial_penalty) + ",\"collocation_weight\":" + number(o.collocation_weight) + ",\"collocation\":" +
           (o.collocation_scorer ? "true" : "false") + ",\"lm\":" + (lm ? "true" : "false") + ",\"unigram_z\":" + number(o.unigram_z) +
           ",\"phrase_length_bonus\":" + number(o.phrase_length_bonus) + "}";
}
} // namespace

int main(int argc, char **argv)
{
    std::ofstream file;
    if (argc > 1)
        file.open(argv[1], std::ios::binary);
    std::ostream &out = argc > 1 ? static_cast<std::ostream &>(file) : std::cout;
    const ngram::LanguageModel *model = nullptr;
    if (argc > 2)
    {
        model = &ngram::shared_language_model(std::filesystem::path(argv[2]));
        if (!model->valid())
        {
            std::cerr << "语言模型载入失败：" << model->error() << "\n";
            return 1;
        }
    }

    static const char *names[] = {"xiaohe", "ziranma", "shoudao", "microsoft", "sogou", "ziguang", "zhinengabc", "guobiao", "pinyinjiajia"};
    const HelpcodeUtils::Keymap keymap = make_keymap();
    Lcg rng{0xd12ec7e1ULL};
    int decodes = 0, complete = 0, incomplete = 0, rewrites = 0, sentences = 0, with_model = 0;
    for (const char *name : names)
    {
        const ShuangpinProfile &profile = GetShuangpinProfile(name);
        std::vector<std::string> codes;
        const std::string keys = "abcdefghijklmnopqrstuvwxyz;";
        std::vector<std::string> quanpins;
        for (const char a : keys)
            for (const char b : keys)
                if (a != ';' && ShuangpinUtil::is_accepted_syllable_code(std::string{a, b}, profile))
                {
                    codes.push_back(std::string{a, b});
                    quanpins.push_back(ShuangpinUtil::cvt_single_sp_to_pinyin(std::string{a, b}, profile));
                }
        for (int scenario = 0; scenario < 220; ++scenario)
        {
            std::string typed;
            if (rng.below(2) == 0)
            {
                // 只含完整双拼码（有时跟一两位辅码），多数能整句走通。
                for (std::uint32_t i = 0, count = 1 + rng.below(4); i < count; ++i)
                {
                    typed += codes[rng.below(static_cast<std::uint32_t>(codes.size()))];
                    if (rng.below(3) == 0)
                        typed += static_cast<char>('a' + rng.below(26));
                }
            }
            else
                typed = random_typed(rng, codes);
            direct_helpcode::SpellingOptions spelling_options;
            spelling_options.slash_marker = rng.below(4) != 0;
            spelling_options.uppercase_marker = rng.below(2) == 0;
            direct_helpcode::DecodeOptions options;
            static const int beams[] = {2, 4, 32};
            static const int limits[] = {1, 3, 32};
            static const int phrases[] = {1, 2, 7};
            static const int sequences[] = {4, 16, 256};
            options.beam = beams[rng.below(3)];
            options.span_limit = limits[rng.below(3)];
            options.max_phrase_syllables = phrases[rng.below(3)];
            options.max_sequences_per_start = sequences[rng.below(3)];
            options.initial_penalty = rng.below(3) == 0 ? -3.0 : -12.0;
            options.unigram_z = rng.below(5) == 0 ? 0.5 : 1e6 * (1 + rng.below(9));
            options.phrase_length_bonus = 1.0 + static_cast<double>(rng.below(60)) / 10.0;
            if (rng.below(2) == 0)
            {
                options.collocation_scorer = [](std::string_view tail, std::string_view word, bool rear) { return collocation(tail, word, rear); };
                options.collocation_weight = rng.below(6) == 0 ? 0.0 : 0.1 + static_cast<double>(rng.below(30)) / 10.0;
            }
            const bool use_model = model && rng.below(2) == 0;
            if (use_model)
                options.language_model = model;
            with_model += use_model;

            // 只解码：哈希谓词建图。
            {
                const auto graph = direct_helpcode::build_spelling_graph(typed, profile, aux_predicate, spelling_options);
                const direct_helpcode::SpanLookup lookup = [](const quanpin::Segments &span, bool) { return lexicon(span); };
                const direct_helpcode::CharAccept accept = [&](const std::string &hz, char first, char second) {
                    const auto found = keymap.find(hz);
                    if (found == keymap.end())
                        return false;
                    SyllableHelpcode helpcode;
                    helpcode.first = first;
                    helpcode.second = second;
                    return syllable_helpcode_matches(helpcode, found->second);
                };
                const auto path = direct_helpcode::decode_best_path(graph, lookup, accept, options);
                ++decodes;
                if (path)
                    (path->complete ? complete : incomplete)++;
                out << "{\"kind\":\"decode\",\"profile\":" << quote(name) << ",\"typed\":" << quote(typed) << ",\"slash\":" << (spelling_options.slash_marker ? "true" : "false")
                    << ",\"upper\":" << (spelling_options.uppercase_marker ? "true" : "false") << ",\"options\":" << options_json(options, use_model)
                    << ",\"path\":" << path_json(path) << "}\n";
            }

            // 解析器：用码表建索引，一半场景给全量单字行、一半走按音节回查。
            {
                direct_helpcode::Resolver resolver(profile);
                direct_helpcode::ResolveContext context;
                context.lookup = [](const quanpin::Segments &span, bool) { return lexicon(span); };
                const bool bulk = rng.below(2) == 0;
                if (bulk)
                    context.single_char_rows = [&]() {
                        direct_helpcode::SingleCharRows rows;
                        for (const auto &q : quanpins)
                            for (const auto &row : lexicon({q}))
                                if (count_codepoints(row.value) == 1)
                                    rows.emplace_back(q, row.value);
                        return rows;
                    };
                const bool no_keymap = rng.below(8) == 0;
                context.keymap = no_keymap ? nullptr : &keymap;
                context.options = options;
                QueryRequest request;
                request.raw_input_with_cases = typed;
                request.direct_helpcode_slash_marker = spelling_options.slash_marker;
                request.direct_helpcode_uppercase_marker = spelling_options.uppercase_marker;
                const bool rewritten = resolver.resolve(request, context);
                const auto *sentence = resolver.resolved_sentence();
                rewrites += rewritten;
                sentences += sentence != nullptr;
                std::string helpcodes = "[", decorations = "[", source = "[";
                for (std::size_t i = 0; i < request.syllable_helpcodes.size(); ++i)
                    helpcodes += std::string(i ? "," : "") + "[" + std::to_string(request.syllable_helpcodes[i].syllable) + "," +
                                 std::to_string(static_cast<int>(request.syllable_helpcodes[i].first)) + "," +
                                 std::to_string(static_cast<int>(request.syllable_helpcodes[i].second)) + "]";
                for (std::size_t i = 0; i < request.syllable_helpcode_decorations.size(); ++i)
                    decorations += std::string(i ? "," : "") + "[" + std::to_string(request.syllable_helpcode_decorations[i].first) + "," +
                                   quote(request.syllable_helpcode_decorations[i].second) + "]";
                for (std::size_t i = 0; i < request.syllable_helpcode_source_index.size(); ++i)
                    source += (i ? "," : "") + std::to_string(request.syllable_helpcode_source_index[i]);
                std::optional<direct_helpcode::DecodedPath> sentence_copy;
                if (sentence)
                    sentence_copy = *sentence;
                out << "{\"kind\":\"resolve\",\"profile\":" << quote(name) << ",\"typed\":" << quote(typed) << ",\"slash\":" << (spelling_options.slash_marker ? "true" : "false")
                    << ",\"upper\":" << (spelling_options.uppercase_marker ? "true" : "false") << ",\"bulk\":" << (bulk ? "true" : "false")
                    << ",\"no_keymap\":" << (no_keymap ? "true" : "false") << ",\"options\":" << options_json(options, use_model)
                    << ",\"rewritten\":" << (rewritten ? "true" : "false") << ",\"sentence\":" << path_json(sentence_copy);
                if (rewritten)
                    out << ",\"request\":{\"raw_input_with_syllable_helpcodes\":" << quote(request.raw_input_with_syllable_helpcodes)
                        << ",\"raw_input\":" << quote(request.raw_input) << ",\"raw_input_with_cases\":" << quote(request.raw_input_with_cases)
                        << ",\"helpcodes\":" << helpcodes << "],\"source_index\":" << source << "],\"decorations\":" << decorations << "]"
                        << ",\"valid\":" << (request.valid ? "true" : "false") << ",\"raw_segmentation\":" << quote(request.raw_segmentation)
                        << ",\"normalized_segmentation\":" << quote(request.normalized_segmentation) << ",\"segmentation\":" << quote(request.segmentation)
                        << ",\"normalized_input\":" << quote(request.normalized_input)
                        << ",\"flags\":[" << request.enable_mid_sentence_helpcode << "," << request.enable_shuangpin_helpcode << "," << request.has_syllable_helpcode_layout << "]}";
                out << "}\n";
            }
        }
    }
    std::cerr << decodes << " 次解码（完整 " << complete << "，含占位 " << incomplete << "，带语言模型 " << with_model << "）；改写请求 " << rewrites << "，解出整句 " << sentences << "\n";
    return 0;
}
