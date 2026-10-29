// 双拼词库探针：用真实的 ShuangpinDictionary 跑三类场景，当 Rust 实现的行为规格。
//
//  sort   —— std::sort 对相等权重的最终次序（Rust 的 msvc_sort::sort_by 要和它逐个下标相同）。
//  read   —— 在随包 msime.db 上跑 generate / generateSeries / generate_with_helpcodes / expand_initial_candidates /
//            find_candidate / generate_for_creating_word / filter_by_syllable_helpcodes，九套内置方案，
//            随机整句开关（词格、显示下一条）、模糊音和句中辅助码约束。
//  write  —— 在一份合成小词库上跑 create_word / create_word_from_quanpin / update_weight_by_pinyin_and_word /
//            delete_by_pinyin_and_word，每步记返回值和词库、日志的逐表转储哈希（不含 updated_at）。
//
//  sources —— 整句来源：Google 解码器（兜底整句、独立整句行，句中辅助码随解码约束）、快速档/高精度档神经重排、
//            万象语法搭配的随机组合。神经重排在后台线程里算：第一次查询按词格的静态顺序出，每收到一次“算完了”
//            的回调就重查一次，直到不再有回调（最多等 0.4 秒），两次的结果都记（没有后台任务时 second 为 null）。
//
// 用法：msime_shuangpin_dictionary_probe <资源目录> <工作目录> <输出文件>
//   资源目录要有 msime.db、sc.lm、dict_pinyin.dat、helpcodes/（即 installer/app_data）。
//   工作目录里会写 shuangpin_dictionary_initial.db（合成小词库，随基线一并提交）。
#include "core/runtime_paths.h"
#include "core/syllable_helpcode.h"
#include "neural/rescore_worker.h"
#include "shuangpin/shuangpin_dictionary.h"
#include "shuangpin/shuangpin_profile.h"
#include "quanpin/quanpin_query.h"
#include "quanpin/quanpin_utils.h"
#include "shuangpin/shuangpin_utils.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

std::string hash_text(const std::string &text)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char c : text)
    {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

void run(sqlite3 *db, const std::string &sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
    {
        std::cerr << "SQL 失败：" << sql << " —— " << (error ? error : "") << "\n";
        std::exit(1);
    }
}

const char *source_name(CandidateSource source)
{
    switch (source)
    {
    case CandidateSource::Database: return "database";
    case CandidateSource::UserDatabase: return "user";
    case CandidateSource::CloudSuggestion: return "cloud";
    case CandidateSource::AiSuggestion: return "ai";
    case CandidateSource::EnglishDictionary: return "english";
    case CandidateSource::QuickPhrase: return "quick";
    case CandidateSource::Emoji: return "emoji";
    case CandidateSource::Kaomoji: return "kaomoji";
    case CandidateSource::Generated: return "generated";
    case CandidateSource::Fallback: return "fallback";
    case CandidateSource::NeuralDesktop: return "neural_desktop";
    case CandidateSource::NeuralKeyboard: return "neural_keyboard";
    case CandidateSource::DateTime: return "datetime";
    case CandidateSource::Collocation: return "collocation";
    default: return "other";
    }
}

// 输入域非空才多一项：没开领域词库的老场景，序列化和哈希与加这一项之前逐字一致。
std::string domains_json(const WordItem &item)
{
    if (item.input_domains.empty())
        return "";
    std::string json = ",[";
    for (std::size_t i = 0; i < item.input_domains.size(); ++i)
        json += (i ? "," : "") + quote(item.input_domains[i]);
    return json + "]";
}

std::string item_json(const WordItem &item)
{
    return "[" + quote(item.pinyin) + "," + quote(item.canonical_pinyin) + "," + quote(item.word) + "," + std::to_string(item.weight) + ",\"" +
           source_name(item.source) + "\"," + (item.sentence_association ? "1" : "0") + domains_json(item) + "]";
}

// {"n":条数,"h":全部内容的哈希,"top":前 8 项}
std::string list_json(const std::vector<WordItem> &items)
{
    std::string all;
    for (const auto &item : items)
        all += item_json(item) + "\n";
    std::string top = "[";
    for (std::size_t i = 0; i < items.size() && i < 8; ++i)
        top += (i ? "," : "") + item_json(items[i]);
    return "{\"n\":" + std::to_string(items.size()) + ",\"h\":\"" + hash_text(all) + "\",\"top\":" + top + "]}";
}

// ---------------------------------------------------------------------------------------------
// sort
// ---------------------------------------------------------------------------------------------

void sort_cases(std::ostream &out)
{
    Lcg rng{0x50c7a11ULL};
    const std::vector<int> sizes = {0, 1, 2, 5, 31, 32, 33, 34, 40, 41, 42, 64, 100, 257, 600, 2000, 5000};
    int id = 0;
    for (const int size : sizes)
        for (const int distinct : {2, 5, 50, 100000})
            for (int repeat = 0; repeat < 3; ++repeat)
            {
                struct Row
                {
                    long long weight;
                    int index;
                };
                std::vector<Row> rows;
                std::string weights = "[";
                for (int i = 0; i < size; ++i)
                {
                    const long long weight = static_cast<long long>(rng.below(static_cast<std::uint32_t>(distinct))) * 1000;
                    rows.push_back({weight, i});
                    weights += (i ? "," : "") + std::to_string(weight);
                }
                weights += "]";
                // 一半输入先按权重降序排好，模拟 SQL 的 ORDER BY weight DESC 之后的列表。
                if (repeat == 1)
                    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.weight > b.weight; });
                if (repeat == 1)
                    for (std::size_t i = 0; i < rows.size(); ++i)
                        rows[i].index = static_cast<int>(i);
                std::string input = "[";
                for (std::size_t i = 0; i < rows.size(); ++i)
                    input += (i ? "," : "") + std::to_string(rows[i].weight);
                input += "]";
                std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.weight > b.weight; });
                std::string order = "[";
                for (std::size_t i = 0; i < rows.size(); ++i)
                    order += (i ? "," : "") + std::to_string(rows[i].index);
                order += "]";
                out << "{\"kind\":\"sort\",\"id\":" << id++ << ",\"weights\":" << input << ",\"order\":" << order << "}\n";
            }
}

// ---------------------------------------------------------------------------------------------
// cut —— 全拼切分（贪心 / 带纠错）和“是不是完整拼音”的判断
// ---------------------------------------------------------------------------------------------

void cut_cases(std::ostream &out)
{
    Lcg rng{0xc07c0deULL};
    const auto &syllables = quanpin::intact_pinyin_list();
    static const std::pair<const char *, const char *> typos[] = {{"ong", "on"},     {"eng", "egn"},   {"ang", "agn"},  {"ing", "ihng"},
                                                                   {"ian", "ain"},    {"iang", "aing"}, {"uang", "aung"}, {"uan", "aun"},
                                                                   {"iao", "aio"},    {"ang", "ahng"},  {"eng", "ehng"}, {"jv", "ju"},
                                                                   {"hang", "ahng"},  {"heng", "ehng"}};
    // 每个音节按每条手误规则敲错后的写法（只收真能改写的），专门用来撞别名表。
    std::vector<std::string> typed_forms;
    for (const auto &syllable : syllables)
        for (const auto &typo : typos)
        {
            const std::string canonical_suffix = typo.first;
            if (syllable.size() >= canonical_suffix.size() && syllable.compare(syllable.size() - canonical_suffix.size(), canonical_suffix.size(), canonical_suffix) == 0)
                typed_forms.push_back(syllable.substr(0, syllable.size() - canonical_suffix.size()) + typo.second);
        }
    int id = 0;
    for (int i = 0; i < 4500; ++i)
    {
        std::string input;
        if (i >= 3000)
        {
            // 手误写法加普通音节混排；每 8 条里有一条是同一个有歧义的写法重复很多遍（路径数超过上限）。
            const std::uint32_t count = 1 + rng.below(4);
            for (std::uint32_t k = 0; k < count; ++k)
            {
                if (k && rng.below(5) == 0)
                    input += "'";
                input += rng.below(3) ? rng.pick(typed_forms) : rng.pick(syllables);
            }
            if (i % 8 == 0)
            {
                const std::string unit = rng.below(2) ? "ahng" : (rng.below(2) ? "cehng" : "ahn");
                input.clear();
                for (std::uint32_t k = 0; k < 2 + rng.below(7); ++k)
                    input += unit;
            }
        }
        else
        {
        const std::uint32_t count = 1 + rng.below(5);
        for (std::uint32_t k = 0; k < count; ++k)
        {
            if (k && rng.below(4) == 0)
                input += "'";
            input += rng.pick(syllables);
        }
        }
        const std::uint32_t damage = i >= 3000 ? 5 : rng.below(6);
        if (damage == 0 || damage == 1)
        {
            const auto &typo = typos[rng.below(14)];
            const auto pos = input.find(typo.first);
            if (pos != std::string::npos)
                input.replace(pos, std::string(typo.first).size(), typo.second);
        }
        else if (damage == 2 && input.size() > 1)
            input.erase(rng.below(static_cast<std::uint32_t>(input.size())), 1);
        else if (damage == 3)
            input.insert(rng.below(static_cast<std::uint32_t>(input.size() + 1)), 1, static_cast<char>('a' + rng.below(26)));
        else if (damage == 4 && input.size() > 2)
        {
            const auto at = rng.below(static_cast<std::uint32_t>(input.size() - 1));
            std::swap(input[at], input[at + 1]);
        }
        const auto paths_json = [](const std::vector<quanpin::Segments> &paths) {
            std::string json = "[";
            for (std::size_t p = 0; p < paths.size(); ++p)
            {
                json += std::string(p ? "," : "") + "[";
                for (std::size_t s = 0; s < paths[p].size(); ++s)
                    json += (s ? "," : "") + quote(paths[p][s]);
                json += "]";
            }
            return json + "]";
        };
        out << "{\"kind\":\"cut\",\"id\":" << id++ << ",\"input\":" << quote(input) << ",\"greedy\":" << paths_json(quanpin::cut_pinyin_by_mode(input, "greedy", true))
            << ",\"correction\":" << paths_json(quanpin::cut_pinyin_by_mode(input, "correction", true)) << ",\"complete\":" << (quanpin::is_complete_pinyin_input(input) ? "true" : "false")
            << ",\"jianpin\":" << quote(quanpin::segments_to_jianpin(quanpin::split_segments(input))) << "}\n";
    }
}

// ---------------------------------------------------------------------------------------------
// 方案、输入串
// ---------------------------------------------------------------------------------------------

struct ProfileInfo
{
    const char *name;
    const ShuangpinProfile *profile;
    std::vector<std::string> codes;              // 方案里真能解出音节的两键码
    std::map<std::string, std::string> code_for; // 全拼音节 -> 一个码
};

std::vector<ProfileInfo> make_profiles()
{
    static const char *names[] = {"xiaohe", "ziranma", "shoudao", "microsoft", "sogou", "ziguang", "zhinengabc", "guobiao", "pinyinjiajia"};
    std::vector<ProfileInfo> result;
    for (const char *name : names)
    {
        ProfileInfo info{name, &GetShuangpinProfile(name), {}, {}};
        const std::string keys = "abcdefghijklmnopqrstuvwxyz;";
        for (const char a : keys)
            for (const char b : keys)
            {
                const std::string code{a, b};
                if (a != ';' && ShuangpinUtil::is_accepted_syllable_code(code, *info.profile))
                {
                    info.codes.push_back(code);
                    const std::string pinyin = ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(code, *info.profile);
                    info.code_for.emplace(pinyin, code);
                }
            }
        result.push_back(std::move(info));
    }
    return result;
}

std::string random_sequence(Lcg &rng, const ProfileInfo &profile)
{
    std::string input;
    const std::uint32_t pieces = 1 + rng.below(6);
    for (std::uint32_t i = 0; i < pieces; ++i)
    {
        if (rng.below(9) == 0)
            input += static_cast<char>('a' + rng.below(26));
        else
            input += profile.codes[rng.below(static_cast<std::uint32_t>(profile.codes.size()))];
    }
    return input;
}

// ---------------------------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------------------------

std::string constraints_json(const SyllableHelpcodes &helpcodes)
{
    std::string out = "[";
    for (std::size_t i = 0; i < helpcodes.size(); ++i)
        out += std::string(i ? "," : "") + "[" + std::to_string(helpcodes[i].syllable) + "," + std::to_string(static_cast<int>(helpcodes[i].first)) + "," +
               std::to_string(static_cast<int>(helpcodes[i].second)) + "]";
    return out + "]";
}

// 领域库每张 tbl_* 表抽几个键，场景的输入由它们按方案折成双拼。
std::vector<std::string> sample_domain_keys(const fs::path &db_path, Lcg &rng)
{
    std::vector<std::string> keys;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(db_path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return keys;
    std::vector<std::string> tables;
    sqlite3_stmt *list = nullptr;
    sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_%' ORDER BY name", -1, &list, nullptr);
    while (list && sqlite3_step(list) == SQLITE_ROW)
        tables.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(list, 0)));
    sqlite3_finalize(list);
    for (const auto &table : tables)
    {
        sqlite3_stmt *count = nullptr;
        sqlite3_prepare_v2(db, ("SELECT count(*) FROM \"" + table + "\"").c_str(), -1, &count, nullptr);
        long long rows = 0;
        if (count && sqlite3_step(count) == SQLITE_ROW)
            rows = sqlite3_column_int64(count, 0);
        sqlite3_finalize(count);
        for (int i = 0; i < 12 && rows > 0; ++i)
        {
            sqlite3_stmt *row = nullptr;
            sqlite3_prepare_v2(db, ("SELECT key FROM \"" + table + "\" LIMIT 1 OFFSET " + std::to_string(rng.below(static_cast<std::uint32_t>(rows)))).c_str(), -1, &row, nullptr);
            if (row && sqlite3_step(row) == SQLITE_ROW)
                keys.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(row, 0)));
            sqlite3_finalize(row);
        }
    }
    sqlite3_close(db);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

// 领域词的全拼键折成这个方案的双拼串；有的整串、有的只留前几个音节、有的音节换成声母，音节在方案里没有码就放弃。
std::string shuangpin_for_key(Lcg &rng, const ProfileInfo &profile, const std::string &key)
{
    std::vector<std::string> syllables;
    std::size_t start = 0;
    while (true)
    {
        const auto pos = key.find('\'', start);
        syllables.push_back(key.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos)
            break;
        start = pos + 1;
    }
    if (rng.below(4) == 0 && syllables.size() > 1)
        syllables.resize(1 + rng.below(static_cast<std::uint32_t>(syllables.size())));
    std::string seq;
    for (std::size_t i = 0; i < syllables.size(); ++i)
    {
        if (rng.below(6) == 0 || (i + 1 == syllables.size() && rng.below(5) == 0))
        {
            seq += syllables[i].substr(0, 1);
            continue;
        }
        const auto code = profile.code_for.find(syllables[i]);
        if (code == profile.code_for.end())
            return "";
        seq += code->second;
    }
    return seq;
}

void read_cases(std::ostream &out, const fs::path &resources, const std::vector<ProfileInfo> &profiles)
{
    const metasequoia::RuntimePaths paths{resources, fs::temp_directory_path() / "msime_shuangpin_probe_user", fs::temp_directory_path() / "msime_shuangpin_probe_cache",
                                          resources};
    fs::create_directories(paths.user_data);
    Lcg rng{0x5b11d1c7ULL};
    int cases = 0;
    for (const auto &info : profiles)
    {
        ShuangpinDictionary dictionary(*info.profile, paths);
        dictionary.set_helpcode_keymap(HelpcodeUtils::load_helpcode_keymap(resources, "xiaohe"));
        for (int scenario = 0; scenario < 70; ++scenario)
        {
            const std::string seq = random_sequence(rng, info);
            const std::string seg = ShuangpinUtil::pinyin_segmentation(seq, *info.profile);
            const std::uint32_t association = rng.below(3); // 0 无，1 词格，2 词格 + 显示下一条
            const std::uint32_t fuzzy = rng.below(3) == 0 ? rng.below(1u << 11) : 0;
            SyllableHelpcodes constraints;
            if (rng.below(4) == 0)
            {
                const std::uint32_t count = 1 + rng.below(2);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    SyllableHelpcode helpcode;
                    helpcode.syllable = rng.below(4);
                    helpcode.first = static_cast<char>('a' + rng.below(26));
                    helpcode.second = rng.below(2) ? static_cast<char>('a' + rng.below(26)) : 0;
                    constraints.push_back(helpcode);
                }
            }
            SentenceAssociationOptions options;
            options.word_lattice = association >= 1;
            options.show_next_on_duplicate = association == 2;
            dictionary.set_sentence_association(options);
            dictionary.set_fuzzy_pinyin(metasequoia::FuzzyPinyinOptions{fuzzy});
            dictionary.set_syllable_helpcodes(constraints);
            dictionary.reset_cache();

            const std::string head = "{\"kind\":\"read\",\"profile\":\"" + std::string(info.name) + "\",\"case\":" + std::to_string(cases++) + ",\"seq\":" + quote(seq) +
                                     ",\"seg\":" + quote(seg) + ",\"assoc\":" + std::to_string(association) + ",\"fuzzy\":" + std::to_string(fuzzy) +
                                     ",\"constraints\":" + constraints_json(constraints);

            out << head << ",\"op\":\"generate\",\"ret\":" << list_json(dictionary.generate(seq, seg)) << "}\n";

            const auto series = dictionary.generateSeries(seq, seg);
            out << head << ",\"op\":\"series\",\"ret\":" << list_json(series) << "}\n";

            auto filtered = series;
            dictionary.filter_by_syllable_helpcodes(filtered);
            out << head << ",\"op\":\"filter\",\"ret\":" << list_json(filtered) << "}\n";

            if (seq.size() >= 2 && seq.size() % 2 == 0 && rng.below(2))
            {
                std::string codes;
                const std::uint32_t kind = rng.below(4);
                const char first = static_cast<char>('a' + rng.below(26)), second = static_cast<char>('a' + rng.below(26));
                if (kind == 0)
                    codes = std::string(1, first);
                else if (kind == 1)
                    codes = std::string(1, static_cast<char>(std::toupper(first)));
                else if (kind == 2)
                    codes = std::string{first, second};
                else
                    codes = std::string{static_cast<char>(std::toupper(first)), second};
                std::string typed = seq;
                for (const char c : codes)
                    typed += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                out << head << ",\"op\":\"helpcodes\",\"typed\":" << quote(typed) << ",\"codes\":" << quote(codes)
                    << ",\"ret\":" << list_json(dictionary.generate_with_helpcodes(seq, seg, typed, codes)) << "}\n";
            }

            if (seq.size() == 1)
            {
                auto candidates = series;
                const bool expanded = dictionary.expand_initial_candidates(seq, candidates);
                out << head << ",\"op\":\"expand\",\"expanded\":" << (expanded ? "true" : "false") << ",\"ret\":" << list_json(candidates) << "}\n";
            }

            if (!series.empty() && rng.below(2))
            {
                const WordItem &chosen = series[rng.below(static_cast<std::uint32_t>(series.size()))];
                const std::string key = rng.below(6) == 0 ? chosen.canonical_pinyin + "'x" : chosen.canonical_pinyin;
                const auto found = dictionary.find_candidate(key, chosen.word);
                out << head << ",\"op\":\"find\",\"key\":" << quote(key) << ",\"value\":" << quote(chosen.word) << ",\"ret\":" << (found ? item_json(*found) : "null") << "}\n";
            }

            if (rng.below(3) == 0)
                out << head << ",\"op\":\"creating\",\"ret\":" << list_json(dictionary.generate_for_creating_word(seq)) << "}\n";
        }

        // ---- 开了领域词库的读场景 ----
        static const std::vector<std::string> packs = {"animals", "automotive", "finance", "food", "historical", "idioms", "it", "law", "medicine", "places", "poetry"};
        std::uint64_t domain_seed = 0xd0a1d1c7ULL;
        for (const char c : std::string(info.name))
            domain_seed = domain_seed * 131 + static_cast<unsigned char>(c);
        Lcg drng{domain_seed}; // 独立的随机流：老场景的输入不因加了这一块而变
        Lcg key_rng{0x4b455953ULL};
        static std::vector<std::string> domain_keys;
        if (domain_keys.empty())
            domain_keys = sample_domain_keys(resources / "domain-input.db", key_rng);
        if (domain_keys.empty())
        {
            std::cerr << "读不到领域库键\n";
            std::exit(1);
        }
        std::string enabled;
        for (int scenario = 0; scenario < 120; ++scenario)
        {
            if (scenario % 6 == 0)
            {
                enabled.clear();
                const std::uint32_t count = drng.below(5) == 0 ? packs.size() : 1 + drng.below(4);
                for (std::uint32_t i = 0; i < count; ++i)
                    enabled += (i ? "," : "") + drng.pick(packs);
                dictionary.set_enabled_domain_dictionaries(enabled);
            }
            const std::string seq = shuangpin_for_key(drng, info, drng.pick(domain_keys));
            if (seq.empty())
                continue;
            const std::string seg = ShuangpinUtil::pinyin_segmentation(seq, *info.profile);
            const std::uint32_t association = drng.below(3);
            const std::uint32_t fuzzy = drng.below(3) == 0 ? drng.below(1u << 11) : 0;
            SentenceAssociationOptions options;
            options.word_lattice = association >= 1;
            options.show_next_on_duplicate = association == 2;
            dictionary.set_sentence_association(options);
            dictionary.set_fuzzy_pinyin(metasequoia::FuzzyPinyinOptions{fuzzy});
            dictionary.set_syllable_helpcodes({});
            dictionary.reset_cache();
            const std::string head = "{\"kind\":\"read\",\"profile\":\"" + std::string(info.name) + "\",\"case\":" + std::to_string(cases++) + ",\"seq\":" + quote(seq) +
                                     ",\"seg\":" + quote(seg) + ",\"assoc\":" + std::to_string(association) + ",\"fuzzy\":" + std::to_string(fuzzy) + ",\"constraints\":[],\"dom\":" + quote(enabled);
            out << head << ",\"op\":\"generate\",\"ret\":" << list_json(dictionary.generate(seq, seg)) << "}\n";
            const auto series = dictionary.generateSeries(seq, seg);
            out << head << ",\"op\":\"series\",\"ret\":" << list_json(series) << "}\n";
            if (seq.size() == 1)
            {
                auto candidates = series;
                const bool expanded = dictionary.expand_initial_candidates(seq, candidates);
                out << head << ",\"op\":\"expand\",\"expanded\":" << (expanded ? "true" : "false") << ",\"ret\":" << list_json(candidates) << "}\n";
            }
        }
        dictionary.set_enabled_domain_dictionaries("");
    }
}

// ---------------------------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------------------------

const std::vector<std::string> kSyllables = {"ni", "hao", "ma", "shi", "jie", "wo", "men", "zhong", "guo", "ren", "da", "xue", "sheng", "ai", "de", "you", "yi", "bu", "ke", "ji", "xiang", "ming", "dian", "chang", "hang"};
const std::string kHanzi = "你好吗是界我们中国人大学生爱的有一不可及他她它这那里什么为时间";

std::string hanzi_at(std::size_t index)
{
    return kHanzi.substr((index % (kHanzi.size() / 3)) * 3, 3);
}

void build_initial(const fs::path &file)
{
    fs::remove(file);
    sqlite3 *db = nullptr;
    sqlite3_open(file.string().c_str(), &db);
    for (const char letter : std::string("abcdefghjklmnopqrstwxyz"))
    {
        for (int count = 1; count <= 7; ++count)
            run(db, "CREATE TABLE tbl_" + std::to_string(count) + "_" + letter + "(key TEXT NOT NULL,jp TEXT NOT NULL,value TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0)");
        run(db, std::string("CREATE TABLE tbl_others_") + letter + "(key TEXT NOT NULL,jp TEXT NOT NULL,value TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0)");
    }
    Lcg rng{0x1d17aULL};
    std::set<std::string> seen;
    const auto add_key = [&](const std::vector<std::string> &syllables) {
        std::string key, jp;
        for (std::size_t i = 0; i < syllables.size(); ++i)
        {
            key += (i ? "'" : "") + syllables[i];
            jp += syllables[i][0];
        }
        if (!seen.insert(key).second)
            return;
        const std::string table = "tbl_" + std::to_string(syllables.size()) + "_" + syllables[0][0];
        const std::uint32_t words = 2 + rng.below(5);
        std::set<std::string> used;
        for (std::uint32_t w = 0; w < words; ++w)
        {
            std::string value;
            for (std::size_t i = 0; i < syllables.size(); ++i)
                value += hanzi_at(rng.below(29));
            if (!used.insert(value).second)
                continue;
            const long long weight = rng.below(4) == 0 ? 5000 : static_cast<long long>(1000 + rng.below(90000));
            std::string escaped_key;
            for (const char c : key)
                escaped_key += c == '\'' ? std::string("''") : std::string(1, c);
            run(db, "INSERT INTO " + table + " VALUES('" + escaped_key + "','" + jp + "','" + value + "'," + std::to_string(weight) + ")");
        }
    };
    for (const auto &syllable : kSyllables)
        add_key({syllable});
    for (int i = 0; i < 90; ++i)
        add_key({rng.pick(kSyllables), rng.pick(kSyllables)});
    for (int i = 0; i < 40; ++i)
        add_key({rng.pick(kSyllables), rng.pick(kSyllables), rng.pick(kSyllables)});
    for (int i = 0; i < 12; ++i)
        add_key({rng.pick(kSyllables), rng.pick(kSyllables), rng.pick(kSyllables), rng.pick(kSyllables)});
    sqlite3_close(db);
}

std::string dump_main(const fs::path &file)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return "(打不开)\n";
    std::vector<std::string> tables;
    sqlite3_stmt *list = nullptr;
    sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_%' ORDER BY name", -1, &list, nullptr);
    while (list && sqlite3_step(list) == SQLITE_ROW)
        tables.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(list, 0)));
    sqlite3_finalize(list);
    std::string text;
    for (const auto &table : tables)
    {
        sqlite3_stmt *rows = nullptr;
        sqlite3_prepare_v2(db, ("SELECT key,jp,value,weight FROM \"" + table + "\" ORDER BY 1,3,4,2").c_str(), -1, &rows, nullptr);
        std::string body;
        while (rows && sqlite3_step(rows) == SQLITE_ROW)
            for (int i = 0; i < 4; ++i)
                body += std::string(i ? "|" : "") + reinterpret_cast<const char *>(sqlite3_column_text(rows, i)) + (i == 3 ? "\n" : "");
        sqlite3_finalize(rows);
        if (!body.empty())
            text += "T:" + table + "\n" + body;
    }
    sqlite3_close(db);
    return text;
}

std::string dump_journal(const fs::path &file)
{
    sqlite3 *db = nullptr;
    if (!fs::exists(file) || sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return "(无日志)\n";
    std::string text;
    sqlite3_stmt *rows = nullptr;
    sqlite3_prepare_v2(db, "SELECT dictionary,key,value,operation,weight,display,user_inserted FROM user_dictionary_operations ORDER BY 1,2,3", -1, &rows, nullptr);
    while (rows && sqlite3_step(rows) == SQLITE_ROW)
        for (int i = 0; i < 7; ++i)
            text += std::string(i ? "|" : "") + reinterpret_cast<const char *>(sqlite3_column_text(rows, i)) + (i == 6 ? "\n" : "");
    sqlite3_finalize(rows);
    sqlite3_close(db);
    return text;
}

// 库里这个键下已有的某个词（没有就返回空），让调频和删词有机会打中真实的行。
std::string existing_word(const fs::path &file, const std::vector<std::string> &syllables, Lcg &rng)
{
    std::string key;
    for (std::size_t i = 0; i < syllables.size(); ++i)
        key += (i ? "'" : "") + syllables[i];
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return "";
    std::vector<std::string> words;
    sqlite3_stmt *rows = nullptr;
    const std::string sql = "SELECT value FROM tbl_" + std::to_string(syllables.size()) + "_" + syllables[0][0] + " WHERE key=?1 ORDER BY weight DESC";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &rows, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text(rows, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(rows) == SQLITE_ROW)
            words.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(rows, 0)));
    }
    sqlite3_finalize(rows);
    sqlite3_close(db);
    return words.empty() ? "" : words[rng.below(static_cast<std::uint32_t>(words.size()))];
}

void write_cases(std::ostream &out, const fs::path &resources, const fs::path &work, const std::vector<ProfileInfo> &profiles)
{
    build_initial(work / "shuangpin_dictionary_initial.db");
    Lcg rng{0x77a1b2c3ULL};
    int ops_total = 0, ops_ok = 0;
    for (int scenario = 0; scenario < 54; ++scenario)
    {
        const ProfileInfo &info = profiles[scenario % profiles.size()];
        const fs::path dir = work / "scenario";
        fs::remove_all(dir);
        fs::create_directories(dir / "user");
        fs::create_directories(dir / "dictionaries");
        fs::copy_file(work / "shuangpin_dictionary_initial.db", dir / "dictionaries" / "msime.db");
        const metasequoia::RuntimePaths paths{resources, dir / "user", dir / "cache", dir / "dictionaries"};
        ShuangpinDictionary dictionary(*info.profile, paths);
        dictionary.set_helpcode_keymap(HelpcodeUtils::load_helpcode_keymap(resources, "xiaohe"));
        SentenceAssociationOptions options;
        options.word_lattice = true;
        dictionary.set_sentence_association(options);
        out << "{\"kind\":\"scenario\",\"id\":" << scenario << ",\"profile\":\"" << info.name << "\"}\n";

        const auto random_pinyin = [&](std::size_t syllables) {
            std::vector<std::string> picked;
            for (std::size_t i = 0; i < syllables; ++i)
                picked.push_back(rng.pick(kSyllables));
            return picked;
        };
        const auto shuangpin_of = [&](const std::vector<std::string> &picked) {
            std::string raw;
            for (const auto &syllable : picked)
            {
                const auto found = info.code_for.find(syllable);
                raw += found == info.code_for.end() ? syllable : found->second;
            }
            return raw;
        };
        const auto join = [](const std::vector<std::string> &picked) {
            std::string joined;
            for (std::size_t i = 0; i < picked.size(); ++i)
                joined += (i ? "'" : "") + picked[i];
            return joined;
        };

        const std::uint32_t op_count = 8 + rng.below(10);
        for (std::uint32_t step = 0; step < op_count; ++step)
        {
            const auto picked = random_pinyin(1 + rng.below(4));
            std::size_t word_length = picked.size();
            if (rng.below(8) == 0)
                word_length += 1;
            std::string word;
            for (std::size_t i = 0; i < word_length; ++i)
                word += hanzi_at(rng.below(29));
            if (rng.below(10) == 0)
                word += "a"; // 非汉字
            const std::uint32_t kind = rng.below(10);
            if (kind >= 5 && kind < 8 && rng.below(3) != 0)
            {
                const std::string known = existing_word(dir / "dictionaries" / "msime.db", picked, rng);
                if (!known.empty())
                    word = known;
            }
            std::string spelled;
            const std::uint32_t form = rng.below(4); // 0 双拼原串，1 带分隔符的全拼，2 不带分隔符的全拼，3 带手误的全拼
            spelled = form == 0 ? shuangpin_of(picked) : form == 1 ? join(picked) : [&] {
                std::string plain;
                for (const auto &syllable : picked)
                    plain += syllable;
                if (form == 3)
                {
                    // 纠错切分认的几种手误：漏 g、颠倒 ng/h、ian 敲成 ain。
                    static const std::pair<const char *, const char *> typos[] = {{"ong", "on"}, {"eng", "egn"}, {"ang", "agn"}, {"ing", "ihng"}, {"ian", "ain"}, {"iang", "aing"}, {"ang", "ahng"}};
                    const auto &typo = typos[rng.below(7)];
                    const auto pos = plain.find(typo.first);
                    if (pos != std::string::npos)
                        plain.replace(pos, std::string(typo.first).size(), typo.second);
                }
                return plain;
            }();
            std::string op, ret = "null";
            if (kind < 3)
            {
                op = "\"op\":\"create_word\",\"pinyin\":" + quote(shuangpin_of(picked)) + ",\"word\":" + quote(word);
                ret = std::to_string(dictionary.create_word(shuangpin_of(picked), word));
            }
            else if (kind < 5)
            {
                const std::string quanpin = rng.below(5) == 0 ? join(picked) + "'zz" : join(picked);
                op = "\"op\":\"create_word_from_quanpin\",\"pinyin\":" + quote(quanpin) + ",\"word\":" + quote(word);
                ret = std::to_string(dictionary.create_word_from_quanpin(quanpin, word));
            }
            else if (kind < 7)
            {
                op = "\"op\":\"update_weight\",\"pinyin\":" + quote(spelled) + ",\"word\":" + quote(word);
                ret = std::to_string(dictionary.update_weight_by_pinyin_and_word(spelled, word));
            }
            else if (kind < 8)
            {
                op = "\"op\":\"delete\",\"pinyin\":" + quote(spelled) + ",\"word\":" + quote(word);
                ret = std::to_string(dictionary.delete_by_pinyin_and_word(spelled, word));
            }
            else if (kind < 9)
            {
                const std::string raw = shuangpin_of(picked);
                const std::string seg = ShuangpinUtil::pinyin_segmentation(raw, *info.profile);
                op = "\"op\":\"series\",\"seq\":" + quote(raw) + ",\"seg\":" + quote(seg);
                ret = list_json(dictionary.generateSeries(raw, seg));
            }
            else
            {
                const std::string raw = shuangpin_of(picked);
                op = "\"op\":\"creating\",\"seq\":" + quote(raw);
                ret = list_json(dictionary.generate_for_creating_word(raw));
            }
            ++ops_total;
            ops_ok += ret == "0";
            const std::string state = dump_main(dir / "dictionaries" / "msime.db") + "==\n" + dump_journal(dir / "user" / "msime_user.db");
            out << "{" << op << ",\"ret\":" << ret << ",\"state\":\"" << hash_text(state) << "\"}\n";
        }
    }
    fs::remove_all(work / "scenario");
    std::cerr << "写库场景 " << ops_total << " 步，成功 " << ops_ok << "\n";
}

// ---------------------------------------------------------------------------------------------
// sources
// ---------------------------------------------------------------------------------------------

void source_cases(std::ostream &out, const fs::path &resources, const std::vector<ProfileInfo> &profiles, const fs::path &golden)
{
    const metasequoia::RuntimePaths paths{resources, fs::temp_directory_path() / "msime_shuangpin_probe_sources_user", fs::temp_directory_path() / "msime_shuangpin_probe_sources_cache",
                                          resources};
    fs::create_directories(paths.user_data);
    Lcg rng{0x6e3f21a9ULL};
    const std::vector<std::string> contexts = {"", "", "今天天气很好", "我们明天一起去", "他说", "请问", "公司的", "我想", "这本书讲的是"};
    const std::string keyboard_model = metasequoia::path_to_utf8(paths.resource(metasequoia::assets::neural_model_keyboard));
    const std::string gram_path = metasequoia::path_to_utf8(golden / "synthetic.gram");
    // 合成语法模型取自输出文件所在目录；输出写到别处时万象来源静默失效，所以缺了就停。
    if (!fs::exists(golden / "synthetic.gram"))
    {
        std::cerr << "找不到 " << (golden / "synthetic.gram").string() << "：输出文件要写在 engine/contracts/golden/ 里\n";
        std::exit(2);
    }
    auto &worker = neural::RescoreWorker::instance();
    int cases = 0, nonempty = 0, with_google = 0, with_neural = 0, reordered = 0, timed_out = 0, with_constraints = 0, google_rows = 0;
    for (const auto &info : profiles)
    {
        ShuangpinDictionary dictionary(*info.profile, paths);
        dictionary.set_helpcode_keymap(HelpcodeUtils::load_helpcode_keymap(resources, "xiaohe"));
        for (int scenario = 0; scenario < 50; ++scenario)
        {
            const std::string seq = random_sequence(rng, info);
            if (seq.size() < 2)
                continue;
            const std::string seg = ShuangpinUtil::pinyin_segmentation(seq, *info.profile);
            const std::uint32_t fuzzy = rng.below(8) == 0 ? rng.below(1u << 11) : 0;
            SyllableHelpcodes constraints;
            if (rng.below(4) == 0)
            {
                const std::uint32_t count = 1 + rng.below(2);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    SyllableHelpcode helpcode;
                    helpcode.syllable = rng.below(4);
                    helpcode.first = static_cast<char>('a' + rng.below(26));
                    helpcode.second = rng.below(2) ? static_cast<char>('a' + rng.below(26)) : 0;
                    constraints.push_back(helpcode);
                }
            }
            SentenceAssociationOptions options;
            options.word_lattice = rng.below(3) != 0;
            options.google = rng.below(2) == 0;
            options.neural_keyboard = rng.below(3) == 0;
            options.neural_desktop = rng.below(5) == 0;
            options.show_next_on_duplicate = rng.below(3) == 0;
            std::string desktop_tag = "none";
            if (options.neural_desktop)
            {
                if (rng.below(4) == 0)
                {
                    options.neural_desktop_model = "";
                    desktop_tag = "default";
                }
                else
                {
                    options.neural_desktop_model = keyboard_model;
                    desktop_tag = "same";
                }
            }
            const double weights[] = {0.0, 0.3, 1.0, 2.0};
            double weight = 0.0;
            if (rng.below(3) == 0)
            {
                options.collocation_model = gram_path;
                weight = weights[rng.below(4)];
                options.collocation_weight = weight;
                options.collocation_rerank = rng.below(2) == 0;
            }
            const std::string &context = contexts[rng.below(static_cast<std::uint32_t>(contexts.size()))];

            dictionary.set_sentence_association(options);
            dictionary.set_rescoring_context(context);
            dictionary.set_fuzzy_pinyin(metasequoia::FuzzyPinyinOptions{fuzzy});
            dictionary.set_syllable_helpcodes(constraints);
            dictionary.reset_cache();
            worker.clear();
            struct Signal
            {
                std::mutex mutex;
                std::condition_variable wake;
                int ready = 0;
            };
            const auto signal = std::make_shared<Signal>();
            worker.set_ready_callback([signal] {
                {
                    std::lock_guard<std::mutex> lock(signal->mutex);
                    ++signal->ready;
                }
                signal->wake.notify_all();
            });

            const auto first = dictionary.generateSeries(seq, seg);
            std::string second = "null";
            if (options.neural_keyboard || options.neural_desktop)
            {
                std::unique_lock<std::mutex> lock(signal->mutex);
                int seen = 0;
                std::vector<WordItem> latest;
                while (signal->wake.wait_for(lock, std::chrono::milliseconds(400), [&] { return signal->ready > seen; }))
                {
                    seen = signal->ready;
                    lock.unlock();
                    dictionary.reset_cache();
                    latest = dictionary.generateSeries(seq, seg);
                    lock.lock();
                }
                if (seen > 0)
                {
                    second = list_json(latest);
                    ++with_neural;
                    reordered += list_json(latest) != list_json(first);
                }
                else
                    ++timed_out;
            }
            worker.set_ready_callback(nullptr);

            ++cases;
            nonempty += !first.empty();
            with_google += options.google;
            with_constraints += !constraints.empty();
            for (const auto &item : first)
                google_rows += item.source == CandidateSource::Fallback && item.sentence_association;
            out << "{\"kind\":\"sources\",\"profile\":\"" << info.name << "\",\"case\":" << cases << ",\"seq\":" << quote(seq) << ",\"seg\":" << quote(seg) << ",\"fuzzy\":" << fuzzy
                << ",\"constraints\":" << constraints_json(constraints) << ",\"context\":" << quote(context) << ",\"lat\":" << (options.word_lattice ? 1 : 0) << ",\"google\":" << (options.google ? 1 : 0)
                << ",\"nk\":" << (options.neural_keyboard ? 1 : 0) << ",\"nd\":" << (options.neural_desktop ? 1 : 0) << ",\"ndm\":\"" << desktop_tag << "\",\"next\":" << (options.show_next_on_duplicate ? 1 : 0)
                << ",\"col\":" << (options.collocation_model.empty() ? 0 : 1) << ",\"colw\":" << weight << ",\"colr\":" << (options.collocation_rerank ? 1 : 0) << ",\"first\":" << list_json(first)
                << ",\"second\":" << second << "}\n";
        }
    }
    std::cerr << "整句来源场景 " << cases << " 个，非空 " << nonempty << "，开 Google " << with_google << "，带句中辅助码 " << with_constraints << "，神经重排重查 " << with_neural << "（改变次序 " << reordered
              << "，超时 " << timed_out << "），Google 整句行 " << google_rows << "\n";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "用法：msime_shuangpin_dictionary_probe <资源目录> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path resources = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    fs::create_directories(work);
    std::ofstream out(argv[3], std::ios::binary);
    const auto profiles = make_profiles();
    sort_cases(out);
    cut_cases(out);
    read_cases(out, resources, profiles);
    write_cases(out, resources, work, profiles);
    source_cases(out, resources, profiles, fs::absolute(argv[3]).parent_path());
    return 0;
}
