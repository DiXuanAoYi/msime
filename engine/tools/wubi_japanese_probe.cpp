// 五笔与日语探针：用真实的 WubiScheme / WubiCandidateProvider / ConvertRomaji / JapaneseSentenceDecoder /
// JapaneseMatrixSearch / JapaneseCandidateProvider 跑一批固定种子生成的输入，当 Rust 实现的行为规格。
//
// 用法：msime_wubi_japanese_probe <msime.db> <dict_japanese.dat> [输出文件]
// 输出 JSONL，kind 有：
//   romaji_all    全部 ≤3 位（字母加 ' -）和 4 位纯字母串的 ConvertRomaji / KanaForRomajiPrefix 结果的滚动哈希
//   romaji        随机罗马字串的转换结果
//   kana_romaji   假名串转回罗马字
//   wubi_scheme   按键序列驱动的五笔方案状态
//   ja_scheme     按键序列驱动的日语方案状态；ja_set_raw 是 set_raw_input 的过滤
//   wubi_query    五笔码表查询
//   ja_connection 连接成本抽样
//   ja_prefix     单假名前缀的词条（走/不走短前缀缓存）
//   ja_decode     词条查询、整句解码、矩阵搜索
//   ja_query      日语候选提供者的完整输出
//   ja_dynamic    带云候选缓存的日语查询
// 列表一律写成 {"n":总数,"h":全部内容的哈希,"top":前若干项}，免得基线过大。
#include "japanese/japanese_matrix_search.h"
#include "japanese/japanese_sentence_decoder.h"
#include "japanese/romaji_converter.h"
#include "providers/japanese_candidate_provider.h"
#include "providers/wubi_candidate_provider.h"
#include "schemes/japanese_romaji_scheme.h"
#include "schemes/wubi_scheme.h"

#include <sqlite3.h>

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
};

struct Hash
{
    std::uint64_t value = 0xcbf29ce484222325ULL;
    void byte(unsigned char c)
    {
        value ^= c;
        value *= 0x100000001b3ULL;
    }
    void field(const std::string &text)
    {
        for (const unsigned char c : text)
            byte(c);
        byte(0xFF);
    }
    std::string hex() const
    {
        char buffer[20];
        std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
        return buffer;
    }
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

std::string strings_json(const std::vector<std::string> &values)
{
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i)
        out += (i ? "," : "") + quote(values[i]);
    return out + "]";
}

std::string items_json(const std::vector<WordItem> &items, std::size_t top)
{
    Hash hash;
    std::string rows = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto &item = items[i];
        hash.field(item.pinyin);
        hash.field(item.canonical_pinyin);
        hash.field(item.word);
        hash.field(std::to_string(item.weight));
        hash.field(std::to_string(static_cast<int>(item.source)));
        hash.field(std::to_string(static_cast<int>(item.scheme)));
        if (i < top)
            rows += std::string(i ? "," : "") + "[" + quote(item.pinyin) + "," + quote(item.canonical_pinyin) + "," +
                    quote(item.word) + "," + std::to_string(item.weight) + "," +
                    std::to_string(static_cast<int>(item.source)) + "," + std::to_string(static_cast<int>(item.scheme)) +
                    "]";
    }
    return "{\"n\":" + std::to_string(items.size()) + ",\"h\":\"" + hash.hex() + "\",\"top\":" + rows + "]}";
}

std::string lemmas_json(const std::vector<japanese::JapaneseLemma> &lemmas, std::size_t top)
{
    Hash hash;
    std::string rows = "[";
    for (std::size_t i = 0; i < lemmas.size(); ++i)
    {
        const auto &l = lemmas[i];
        hash.field(l.reading);
        hash.field(l.surface);
        hash.field(std::to_string(l.left_id));
        hash.field(std::to_string(l.right_id));
        hash.field(std::to_string(l.word_cost));
        hash.field(std::to_string(l.token_id));
        if (i < top)
            rows += std::string(i ? "," : "") + "[" + quote(l.reading) + "," + quote(l.surface) + "," +
                    std::to_string(l.left_id) + "," + std::to_string(l.right_id) + "," + std::to_string(l.word_cost) +
                    "," + std::to_string(l.token_id) + "]";
    }
    return "{\"n\":" + std::to_string(lemmas.size()) + ",\"h\":\"" + hash.hex() + "\",\"top\":" + rows + "]}";
}

std::string sentences_json(const std::vector<japanese::SentenceCandidate> &sentences, std::size_t top)
{
    Hash hash;
    std::string rows = "[";
    for (std::size_t i = 0; i < sentences.size(); ++i)
    {
        hash.field(sentences[i].text);
        hash.field(std::to_string(sentences[i].cost));
        if (i < top)
            rows += std::string(i ? "," : "") + "[" + quote(sentences[i].text) + "," +
                    std::to_string(sentences[i].cost) + "]";
    }
    return "{\"n\":" + std::to_string(sentences.size()) + ",\"h\":\"" + hash.hex() + "\",\"top\":" + rows + "]}";
}

std::string to_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// ---------- 罗马字 ----------

// 按“长度从短到长、位置最高位在前”的顺序枚举字母表上 1..max_len 位的全部串；Rust 一侧用同样的次序。
template <typename F> void enumerate(const std::string &alphabet, int max_len, F &&visit)
{
    for (int length = 1; length <= max_len; ++length)
    {
        std::vector<std::size_t> digits(static_cast<std::size_t>(length), 0);
        while (true)
        {
            std::string text;
            for (const auto d : digits)
                text.push_back(alphabet[d]);
            visit(text);
            int position = length - 1;
            while (position >= 0 && ++digits[static_cast<std::size_t>(position)] == alphabet.size())
            {
                digits[static_cast<std::size_t>(position)] = 0;
                --position;
            }
            if (position < 0)
                break;
        }
    }
}

std::string random_romaji(Lcg &rng)
{
    static const char *pieces[] = {"a",   "i",   "u",    "e",    "o",    "ka",  "ki",  "ku",  "ke",   "ko",  "sa",
                                   "shi", "si",  "su",   "se",   "so",   "ta",  "chi", "tsu", "te",   "to",  "na",
                                   "ni",  "nu",  "ne",   "no",   "ha",   "hi",  "fu",  "he",  "ho",   "ma",  "mi",
                                   "mu",  "me",  "mo",   "ya",   "yu",   "yo",  "ra",  "ri",  "ru",   "re",  "ro",
                                   "wa",  "wo",  "nn",   "n",    "kya",  "sha", "ja",  "cha", "nya",  "fa",  "xtsu",
                                   "ltsu", "-",  "'",    "kk",   "tt",   "ss",  "pp",  "k",   "sh",   "ky",  "ch",
                                   "ts",  "x",   "z",    "y",    "w",    "konni", "gya", "pyo", "dhi",  "twu", "va",
                                   "ga",  "zu",  "ji",   "da",   "bu",   "pe",  "xya", "lyo", "tya",  "q",   "m"};
    const std::uint32_t count = 1 + rng.below(6);
    std::string text;
    for (std::uint32_t i = 0; i < count; ++i)
        text += pieces[rng.below(sizeof(pieces) / sizeof(pieces[0]))];
    if (rng.below(8) == 0)
        text[rng.below(static_cast<std::uint32_t>(text.size()))] = static_cast<char>(std::toupper(text[0]));
    return text;
}

void emit_romaji(std::ostream &out)
{
    {
        Hash hash;
        long count = 0;
        const auto visit = [&](const std::string &text) {
            const auto c = japanese::ConvertRomaji(text);
            hash.field(text);
            hash.field(c.hiragana);
            hash.field(c.pending);
            hash.byte(c.complete ? 1 : 0);
            hash.byte(0xFE);
            ++count;
        };
        enumerate("abcdefghijklmnopqrstuvwxyz'-", 3, visit);
        enumerate("abcdefghijklmnopqrstuvwxyz", 4, [&](const std::string &text) {
            if (text.size() == 4)
                visit(text);
        });
        Hash prefix_hash;
        long prefix_count = 0;
        enumerate("abcdefghijklmnopqrstuvwxyz", 4, [&](const std::string &text) {
            prefix_hash.field(text);
            for (const auto &kana : japanese::KanaForRomajiPrefix(text))
                prefix_hash.field(kana);
            prefix_hash.byte(0xFE);
            ++prefix_count;
        });
        out << "{\"kind\":\"romaji_all\",\"convert\":{\"n\":" << count << ",\"h\":\"" << hash.hex()
            << "\"},\"prefix\":{\"n\":" << prefix_count << ",\"h\":\"" << prefix_hash.hex() << "\"}}\n";
    }

    Lcg rng{0x70a15e11ULL};
    for (int i = 0; i < 1500; ++i)
    {
        const std::string input = i < 400 ? [&] {
            std::string text;
            const std::uint32_t length = 1 + rng.below(4);
            for (std::uint32_t k = 0; k < length; ++k)
                text.push_back(static_cast<char>('a' + rng.below(26)));
            return text;
        }() : random_romaji(rng);
        const auto c = japanese::ConvertRomaji(input);
        out << "{\"kind\":\"romaji\",\"input\":" << quote(input) << ",\"hiragana\":" << quote(c.hiragana)
            << ",\"pending\":" << quote(c.pending) << ",\"complete\":" << (c.complete ? "true" : "false")
            << ",\"single\":" << (japanese::IsSingleKanaConversion(c) ? "true" : "false")
            << ",\"katakana\":" << quote(japanese::HiraganaToKatakana(c.hiragana))
            << ",\"back\":" << quote(japanese::HiraganaToRomaji(c.hiragana))
            << ",\"prefix_kana\":" << strings_json(japanese::KanaForRomajiPrefix(c.pending)) << "}\n";
    }
}

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
    else
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

void emit_kana_romaji(std::ostream &out)
{
    std::vector<std::string> pool;
    for (char32_t cp = 0x3041; cp <= 0x3096; ++cp)
        pool.push_back(utf8_of(cp));
    for (char32_t cp = 0x30A1; cp <= 0x30F6; ++cp)
        pool.push_back(utf8_of(cp));
    pool.push_back(utf8_of(0x30FC));
    pool.push_back("a");
    pool.push_back("漢");
    pool.push_back("ー");
    const auto row = [&](const std::string &kana) {
        out << "{\"kind\":\"kana_romaji\",\"kana\":" << quote(kana) << ",\"romaji\":" << quote(japanese::HiraganaToRomaji(kana))
            << "}\n";
    };
    for (const auto &kana : pool)
        row(kana);
    // 拗音和外来音：大假名加小假名的全部两字组合，表里的多假名项都落在这里面。
    const char32_t smalls[] = {0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3083, 0x3085, 0x3087};
    for (const std::string base : {"き", "ぎ", "し", "じ", "ち", "に", "ひ", "び", "ぴ", "み", "り", "ふ", "ゔ", "う", "つ", "て", "で", "と", "ど", "く", "ぐ"})
        for (const char32_t tiny : smalls)
            row(base + utf8_of(tiny));
    Lcg rng{0x4a4ab0b0ULL};
    for (int i = 0; i < 600; ++i)
    {
        std::string text;
        const std::uint32_t length = 1 + rng.below(6);
        for (std::uint32_t k = 0; k < length; ++k)
        {
            const auto roll = rng.below(10);
            if (roll == 0)
                text += "っ";
            else if (roll == 1)
                text += "ん";
            else if (roll == 2)
                text += "ッ";
            else
                text += pool[rng.below(static_cast<std::uint32_t>(pool.size()))];
        }
        row(text);
    }
}

// ---------- 五笔 ----------

void emit_wubi_scheme(std::ostream &out)
{
    Lcg rng{0x57b15c3eULL};
    for (int scenario = 0; scenario < 350; ++scenario)
    {
        const bool mixed = rng.below(3) == 0;
        const bool wild = rng.below(3) == 0;
        WubiScheme scheme;
        scheme.set_mixed_pinyin_allowed(mixed);
        scheme.set_z_wildcard(wild);
        std::string ops = "[";
        std::string snapshots = "[";
        const std::uint32_t count = rng.below(16);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::string op;
            const auto roll = rng.below(100);
            if (roll < 70)
            {
                const ImeKeyCode vk = 'A' + rng.below(26);
                const ImeModifierMask mods = rng.below(4) == 0 ? rng.below(4) : 0;
                const ImeCharacter wch = rng.below(5) == 0 ? 0 : static_cast<ImeCharacter>((rng.below(2) ? 'a' : 'A') + (vk - 'A'));
                scheme.handle_key(vk, mods, wch);
                op = "[\"k\"," + std::to_string(vk) + "," + std::to_string(mods) + "," + std::to_string(wch) + "]";
            }
            else if (roll < 78)
            {
                scheme.handle_key(ImeKey::Backspace, 0, 0);
                op = "[\"k\",8,0,0]";
            }
            else if (roll < 81)
            {
                const ImeKeyCode vk = rng.below(2) ? ImeKey::Escape : ImeKey::Return;
                scheme.handle_key(vk, 0, 0);
                op = "[\"k\"," + std::to_string(vk) + ",0,0]";
            }
            else if (roll < 88)
            {
                static const ImeKeyCode others[] = {'0', '1', ImeKey::Space, ImeKey::Apostrophe, ImeKey::Minus, ImeKey::Tab};
                const ImeKeyCode vk = others[rng.below(6)];
                scheme.handle_key(vk, 0, 0x20);
                op = "[\"k\"," + std::to_string(vk) + ",0,32]";
            }
            else if (roll < 93)
            {
                const bool allow = rng.below(2) == 0;
                scheme.set_extended_length_allowed(allow);
                op = std::string("[\"x\",") + (allow ? "1" : "0") + "]";
            }
            else if (roll < 99)
            {
                std::string raw, cases;
                const std::uint32_t length = rng.below(9);
                for (std::uint32_t k = 0; k < length; ++k)
                {
                    const auto kind = rng.below(10);
                    raw.push_back(kind == 0 ? static_cast<char>('0' + rng.below(10)) : static_cast<char>('a' + rng.below(26)));
                }
                if (rng.below(2))
                {
                    cases = raw;
                    for (auto &ch : cases)
                        if (rng.below(2))
                            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
                scheme.set_raw_input(raw, cases);
                op = "[\"r\"," + quote(raw) + "," + quote(cases) + "]";
            }
            else
            {
                scheme.reset();
                op = "[\"z\"]";
            }
            ops += std::string(i ? "," : "") + op;
            snapshots += std::string(i ? "," : "") + "[" + quote(scheme.get_preedit()) + "," +
                         (scheme.has_complete_code() ? "true" : "false") + "]";
        }
        const auto request = scheme.build_request();
        std::string strokes = "[";
        for (std::size_t i = 0; i < request.key_strokes.size(); ++i)
            strokes += std::string(i ? "," : "") + "[" + std::to_string(request.key_strokes[i].vk) + "," +
                       std::to_string(request.key_strokes[i].modifiers_down) + "," +
                       std::to_string(request.key_strokes[i].wch) + "]";
        out << "{\"kind\":\"wubi_scheme\",\"mixed\":" << (mixed ? "true" : "false") << ",\"wild\":" << (wild ? "true" : "false")
            << ",\"ops\":" << ops << "],\"snapshots\":" << snapshots << "],\"strings\":"
            << strings_json({request.raw_input, request.raw_input_with_cases, request.normalized_input,
                             request.raw_segmentation, request.normalized_segmentation, request.segmentation})
            << ",\"strokes\":" << strokes << "],\"wildcard\":" << (request.wubi_z_wildcard ? "true" : "false")
            << ",\"valid\":" << (request.valid ? "true" : "false") << ",\"scheme\":" << static_cast<int>(request.scheme)
            << "}\n";
    }
}

// 日语方案的按键处理：和五笔一样用操作序列驱动，每步一个预编辑快照。
void emit_japanese_scheme(std::ostream &out)
{
    Lcg rng{0x6a9a2b17ULL};
    for (int scenario = 0; scenario < 300; ++scenario)
    {
        JapaneseRomajiScheme scheme;
        std::string ops = "[";
        std::string snapshots = "[";
        const std::uint32_t count = rng.below(14);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ImeKeyCode vk;
            ImeCharacter wch;
            const auto roll = rng.below(100);
            if (roll < 62)
            {
                vk = 'A' + rng.below(26);
                const auto style = rng.below(6);
                wch = style == 0 ? 0 : static_cast<ImeCharacter>((style < 4 ? 'a' : 'A') + (vk - 'A'));
                if (style == 5)
                    wch = static_cast<ImeCharacter>('0' + rng.below(10));
            }
            else if (roll < 72)
            {
                vk = ImeKey::Backspace;
                wch = 0;
            }
            else if (roll < 76)
            {
                vk = rng.below(2) ? ImeKey::Escape : ImeKey::Return;
                wch = 0;
            }
            else if (roll < 86)
            {
                vk = ImeKey::Apostrophe;
                wch = rng.below(3) ? u'\'' : u'"';
            }
            else if (roll < 96)
            {
                vk = ImeKey::Minus;
                wch = rng.below(3) ? u'-' : u'_';
            }
            else
            {
                static const ImeKeyCode others[] = {'0', '1', ImeKey::Space, ImeKey::Tab, ImeKey::Slash};
                vk = others[rng.below(5)];
                wch = u' ';
            }
            const ImeModifierMask mods = rng.below(5) == 0 ? rng.below(4) : 0;
            scheme.handle_key(vk, mods, wch);
            ops += std::string(i ? "," : "") + "[" + std::to_string(vk) + "," + std::to_string(mods) + "," +
                   std::to_string(wch) + "]";
            snapshots += std::string(i ? "," : "") + quote(scheme.get_preedit());
        }
        const auto request = scheme.build_request();
        std::string strokes = "[";
        for (std::size_t k = 0; k < request.key_strokes.size(); ++k)
            strokes += std::string(k ? "," : "") + "[" + std::to_string(request.key_strokes[k].vk) + "," +
                       std::to_string(request.key_strokes[k].modifiers_down) + "," +
                       std::to_string(request.key_strokes[k].wch) + "]";
        out << "{\"kind\":\"ja_scheme\",\"ops\":" << ops << "],\"snapshots\":" << snapshots << "],\"strings\":"
            << strings_json({request.raw_input, request.raw_input_with_cases, request.normalized_input,
                             request.raw_segmentation, request.normalized_segmentation, request.segmentation})
            << ",\"strokes\":" << strokes << "],\"valid\":" << (request.valid ? "true" : "false") << "}\n";
    }
    // set_raw_input 的过滤。
    for (const std::string raw : {"Nihon-go", "a b'c", "KYOU123", "ni'hon", "---", "", "ÀBC", "a\tb"})
    {
        JapaneseRomajiScheme scheme;
        scheme.set_raw_input(raw, rng.below(2) ? raw : "");
        const auto request = scheme.build_request();
        out << "{\"kind\":\"ja_set_raw\",\"raw\":" << quote(raw) << ",\"strings\":"
            << strings_json({request.raw_input, request.raw_input_with_cases, request.normalized_input,
                             request.raw_segmentation, request.normalized_segmentation, request.segmentation})
            << ",\"valid\":" << (request.valid ? "true" : "false") << "}\n";
    }
}

std::vector<std::string> column_sample(sqlite3 *db, const char *sql, std::size_t wanted)
{
    std::vector<std::string> all;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
        return all;
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        if (text)
            all.emplace_back(text);
    }
    sqlite3_finalize(statement);
    std::vector<std::string> picked;
    if (all.empty())
        return picked;
    const std::size_t stride = std::max<std::size_t>(1, all.size() / wanted);
    for (std::size_t i = 0; i < all.size() && picked.size() < wanted; i += stride)
        picked.push_back(all[i]);
    return picked;
}

void emit_wubi_query(std::ostream &out, const char *db_path, sqlite3 *db)
{
    WubiCandidateProvider provider(db_path);
    std::vector<std::pair<std::string, bool>> cases;
    for (char a = 'a'; a <= 'y'; ++a)
        cases.push_back({std::string(1, a), false});
    Lcg rng{0x31e0b1e1ULL};
    for (int i = 0; i < 200; ++i)
        cases.push_back({std::string{static_cast<char>('a' + rng.below(25)), static_cast<char>('a' + rng.below(25))}, false});
    for (const auto &key : column_sample(db, "SELECT DISTINCT \"key\" FROM wubi86 WHERE length(\"key\") >= 3 ORDER BY \"key\"", 400))
        cases.push_back({key, false});
    for (int i = 0; i < 150; ++i)
    {
        std::string code;
        const std::uint32_t length = 3 + rng.below(2);
        for (std::uint32_t k = 0; k < length; ++k)
            code.push_back(static_cast<char>('a' + rng.below(25)));
        cases.push_back({code, false});
    }
    for (int i = 0; i < 250; ++i)
    {
        std::string code;
        const std::uint32_t length = 2 + rng.below(3);
        for (std::uint32_t k = 0; k < length; ++k)
            code.push_back(static_cast<char>('a' + rng.below(25)));
        code[rng.below(length)] = 'z';
        if (rng.below(3) == 0)
            code[rng.below(length)] = 'z';
        cases.push_back({code, rng.below(8) != 0});
    }
    cases.push_back({"z", true});
    cases.push_back({"zz", true});
    cases.push_back({"zzzz", true});
    cases.push_back({"a", true});
    for (const auto &[code, wild] : cases)
    {
        WubiScheme scheme;
        scheme.set_z_wildcard(wild);
        scheme.set_raw_input(code, "");
        const auto request = scheme.build_request();
        const auto items = provider.query(request);
        out << "{\"kind\":\"wubi_query\",\"code\":" << quote(code) << ",\"wild\":" << (wild ? "true" : "false")
            << ",\"valid\":" << (request.valid ? "true" : "false") << ",\"rows\":" << items_json(items, 6) << "}\n";
    }
}

// ---------- 日语 ----------

japanese::RomajiConversion convert_lower(const std::string &input)
{
    return japanese::ConvertRomaji(to_lower(input));
}

QueryRequest japanese_request(const std::string &input)
{
    JapaneseRomajiScheme scheme;
    scheme.set_raw_input(input, input);
    return scheme.build_request();
}

void emit_japanese(std::ostream &out, const char *db_path, const char *model_path, sqlite3 *db)
{
    const japanese::JapaneseSentenceDecoder decoder{std::string(model_path)};
    if (!decoder.ready())
    {
        std::cerr << "日语模型载入失败：" << model_path << "\n";
        std::exit(1);
    }
    const japanese::JapaneseMatrixSearch search(decoder);

    Lcg rng{0x1a9a2e5eULL};
    for (int i = 0; i < 1500; ++i)
    {
        const std::uint32_t range = rng.below(8) == 0 ? 65536 : 3000;
        const auto right = static_cast<std::uint16_t>(rng.below(range));
        const auto left = static_cast<std::uint16_t>(rng.below(range));
        out << "{\"kind\":\"ja_connection\",\"right\":" << right << ",\"left\":" << left
            << ",\"cost\":" << decoder.ConnectionCost(right, left) << "}\n";
    }

    std::vector<std::string> kana;
    for (char32_t cp = 0x3041; cp <= 0x3093; ++cp)
        kana.push_back(utf8_of(cp));
    kana.push_back("ー");
    for (int i = 0; i < 40; ++i)
        kana.push_back(kana[rng.below(83)] + kana[rng.below(83)]);
    for (const auto &prefix : kana)
        out << "{\"kind\":\"ja_prefix\",\"prefix\":" << quote(prefix) << ",\"p24\":" << lemmas_json(decoder.PrefixLemmas(prefix, 24), 6)
            << ",\"p100\":" << lemmas_json(decoder.PrefixLemmas(prefix, 100), 4) << "}\n";

    // 输入清单：词库里的罗马字写法、它们被截断后的写法（出现没打完的罗马字）、随机罗马字串。
    std::vector<std::string> inputs{"-", "a", "n", "k", "ky", "kyou", "nihon", "nihongo", "konnichiha", "konnichiwa", "kitte", "xtsu",
                                    "nn", "n'a", "rennai", "gakkou", "toukyou", "wotashi", "watashiha", "sh", "tsu", "ltsu"};
    const auto codes = column_sample(db, "SELECT code FROM japanese_lexicon ORDER BY rowid", 260);
    for (const auto &code : codes)
        inputs.push_back(code);
    for (std::size_t i = 0; i < codes.size() && i < 110; ++i)
        if (codes[i].size() > 1)
            inputs.push_back(codes[i].substr(0, 1 + rng.below(static_cast<std::uint32_t>(codes[i].size() - 1))));
    for (int i = 0; i < 90; ++i)
        inputs.push_back(random_romaji(rng));
    // 长一些的整句：把几个词库写法接起来，才会走到 hiragana 长度 ≥ 6 的分支。
    for (int i = 0; i < 60 && !codes.empty(); ++i)
        inputs.push_back(codes[rng.below(static_cast<std::uint32_t>(codes.size()))] + codes[rng.below(static_cast<std::uint32_t>(codes.size()))] +
                         (rng.below(3) == 0 ? codes[rng.below(static_cast<std::uint32_t>(codes.size()))] : ""));

    // 自然的句子和短语：词库里的写法大多是片假名变体和 q 前缀，走不到“几个词连成一句”的路径，这里补上。
    for (const char *sentence :
         {"watashihagakkouheikimasu", "kyouhatenkigaii", "nihongowobenkyoushiteimasu", "toukyoutawaadeshokujiwosuru",
          "arigatougozaimasu", "ohayougozaimasu", "konbanwa", "sayounara", "tabemonowotabetai", "ashitahaamedesu",
          "kareharyokouwosuru", "anatanonamaehananidesuka", "sensehatodomotachini", "kinoukaishawoyasunda",
          "nihonnoshuutoha", "daigakuniikitai", "eigowohanasemasu", "densyadeshigotoni", "kuruma-wounten", "ra-men"})
        inputs.insert(inputs.begin() + 4, sentence);
    static const char *words[] = {"watashi", "anata", "kare",   "gakkou", "sensei", "nihon",     "nihongo", "eigo",   "benkyou", "shigoto",
                                  "tabemono", "kyou", "ashita", "tenki",  "densha", "kuruma",    "hon",     "tomodachi", "kaisha", "ie"};
    static const char *particles[] = {"wa", "ga", "wo", "ni", "de", "to", "no", "mo"};
    static const char *endings[] = {"desu", "masu", "shita", "shimasu", "imasu", "nai", "suru", "itai", "ta", "te"};
    for (int i = 0; i < 160; ++i)
    {
        std::string sentence;
        const std::uint32_t parts = 1 + rng.below(3);
        for (std::uint32_t k = 0; k < parts; ++k)
        {
            sentence += words[rng.below(20)];
            sentence += particles[rng.below(8)];
        }
        sentence += endings[rng.below(10)];
        inputs.insert(inputs.begin() + 24, sentence);
    }

    JapaneseCandidateProvider provider(db_path, model_path);
    std::size_t complete_rows = 0, pending_rows = 0;
    for (const auto &input : inputs)
    {
        const auto c = convert_lower(input);
        const auto request = japanese_request(input);
        out << "{\"kind\":\"ja_query\",\"input\":" << quote(input) << ",\"raw\":" << quote(request.raw_input)
            << ",\"rows\":" << items_json(provider.query(request), 24) << "}\n";
        const bool is_complete = !c.hiragana.empty() && c.pending.empty();
        const bool is_pending = !c.pending.empty();
        if ((is_complete && complete_rows < 230) || (is_pending && pending_rows < 60))
        {
            (is_complete ? complete_rows : pending_rows) += 1;
            const auto &h = c.hiragana;
            const auto pending_kana = japanese::KanaForRomajiPrefix(c.pending);
            out << "{\"kind\":\"ja_decode\",\"input\":" << quote(input) << ",\"hiragana\":" << quote(h)
                << ",\"pending\":" << quote(c.pending) << ",\"exact\":" << lemmas_json(decoder.ExactLemmas(h, 16), 5)
                << ",\"prefix\":" << lemmas_json(decoder.PrefixLemmas(h, 24), 5)
                << ",\"longer\":" << lemmas_json(decoder.LongerPrefixLemmas(h, 16), 5) << ",\"continuing\":"
                << (c.pending.empty() ? std::string("null") : lemmas_json(decoder.PrefixLemmasContinuing(h, pending_kana, 32), 5))
                << ",\"decode\":" << sentences_json(decoder.Decode(h, 8), 8)
                << ",\"search\":" << sentences_json(search.SearchConverted(c, 12), 12) << "}\n";
        }
    }

    // 云候选缓存：同一个来源再缓存会替换上一条，所以只有最后一条留着。
    for (const std::string &input : {"kyou", "nihon", "a", "kitte", "konnichiha", "wotashi", "xtsu", "gakkou", "ka", "toukyou", "k", "nn"})
    {
        JapaneseCandidateProvider fresh(db_path, model_path);
        const auto request = japanese_request(input);
        std::vector<std::string> cached{"云" + input};
        fresh.cache_dynamic_candidate(SchemeType::JapaneseRomaji, request.raw_input, cached.back(), CandidateSource::CloudSuggestion);
        if (input.size() % 2 == 0)
        {
            cached.push_back("再" + input);
            fresh.cache_dynamic_candidate(SchemeType::JapaneseRomaji, request.raw_input, cached.back(), CandidateSource::CloudSuggestion);
        }
        out << "{\"kind\":\"ja_dynamic\",\"input\":" << quote(input) << ",\"cached\":" << strings_json(cached)
            << ",\"rows\":" << items_json(fresh.query(request), 12) << "}\n";
    }
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "用法：msime_wubi_japanese_probe <msime.db> <dict_japanese.dat> [输出文件]\n";
        return 2;
    }
    std::ofstream file;
    if (argc > 3)
        file.open(argv[3], std::ios::binary);
    std::ostream &out = argc > 3 ? static_cast<std::ostream &>(file) : std::cout;

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        std::cerr << "打不开词库：" << argv[1] << "\n";
        return 1;
    }
    emit_romaji(out);
    emit_kana_romaji(out);
    emit_wubi_scheme(out);
    emit_japanese_scheme(out);
    emit_wubi_query(out, argv[1], db);
    emit_japanese(out, argv[1], argv[2], db);
    sqlite3_close(db);
    return 0;
}
