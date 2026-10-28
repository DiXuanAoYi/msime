// 全拼词库探针：用真实的 QuanpinDictionary 跑两类场景，当 Rust 实现的行为规格。
//
//  read   —— 在随包 msime.db 上跑 query（不带纠错）/ fuzzy_candidates / expand_initial_candidates / find_candidate。
//            输入串取自词库里真有的键（整串、前缀、简拼、混拼、没打完的尾音节、ü 的别名写法、手动分隔符），
//            另混入最少音节数打平的歧义串（yi'ba'nan'zhao / yi'ban'an'zhao）；随机整句开关（词格、显示下一条）、
//            模糊音、显式切分，以及“用户在这个读音上调过频”的日志状态。
//  write  —— 在一份合成小词库上跑 create_word / create_word_from_canonical_pinyin / update_weight_by_pinyin_and_word /
//            update_weight_by_word / delete_by_pinyin_and_word，夹着查询，每步记返回值和词库、日志的逐表转储哈希（不含 updated_at）。
//
//  sources —— 整句来源：Google 解码器（兜底整句、独立整句行）、快速档/高精度档神经重排、万象语法搭配的随机组合，
//            夹着纠错、模糊音、随机前文。神经重排在后台线程里算：第一次查询按词格的静态顺序出，等线程算完
//            （收到“算完了”的回调，最多等 0.4 秒）再重查一次，两次的结果都记（没有后台任务时 second 为 null）。
//            高精度档用的是同一份模型文件（随包只有快速档），或者一个不存在的路径（模型缺失，来源静默退回）。
//
// 前四类场景用的 SentenceAssociationOptions 只开词格；领域词库见各自场景。
//
// 用法：msime_quanpin_dictionary_probe <资源目录> <工作目录> <输出文件>
//   资源目录要有 msime.db、sc.lm、dict_pinyin.dat（即 installer/app_data）。
//   工作目录里会写 quanpin_dictionary_initial.db（合成小词库，随基线一并提交）。
#include "core/runtime_paths.h"
#include "neural/rescore_worker.h"
#include "quanpin/quanpin_dictionary.h"
#include "quanpin/quanpin_query.h"
#include "quanpin/quanpin_utils.h"
#include "user_dictionary/user_dictionary_journal.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
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
           source_name(item.source) + "\"," + (item.sentence_association ? "1" : "0") + "," + (item.fuzzy ? "1" : "0") + "," + quote(item.corrected_from) + domains_json(item) + "]";
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

std::string join(const std::vector<std::string> &parts, const char *glue = "'")
{
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i)
        joined += (i ? glue : "") + parts[i];
    return joined;
}

std::vector<std::string> split_key(const std::string &key)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        const auto pos = key.find('\'', start);
        if (pos == std::string::npos)
        {
            parts.push_back(key.substr(start));
            return parts;
        }
        parts.push_back(key.substr(start, pos - start));
        start = pos + 1;
    }
}

// ---------------------------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------------------------

// 从词库里每张表取几个键，输入串都由真键变来，保证大多数输入真有词可查。
std::vector<std::string> sample_keys(const fs::path &db_path, Lcg &rng)
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
        if (rows == 0)
            continue;
        const int picks = table.rfind("tbl_1_", 0) == 0 ? 5 : 9;
        for (int i = 0; i < picks; ++i)
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

// 减成简拼的一个音节：声母（zh/ch/sh 只留第一个字母，和用户敲的一致）。
std::string initial_of(const std::string &syllable)
{
    return syllable.substr(0, 1);
}

std::string alias_of(const std::string &syllable)
{
    if (syllable == "nve" || syllable == "lve")
        return syllable.substr(0, 1) + "ue";
    if (syllable.size() >= 2 && syllable[1] == 'u' && std::string("jqxy").find(syllable[0]) != std::string::npos)
        return syllable.substr(0, 1) + "v" + syllable.substr(2);
    return syllable;
}

struct ReadInput
{
    std::string raw;
    std::string segmentation; // 空表示让引擎自己切
};

ReadInput make_input(Lcg &rng, const std::vector<std::string> &keys, const std::vector<std::string> &ambiguous)
{
    ReadInput input;
    if (rng.below(7) == 0)
    {
        // 歧义串：最少音节数打平的几种切分。
        const std::uint32_t count = 2 + rng.below(6);
        for (std::uint32_t i = 0; i < count; ++i)
            input.raw += rng.pick(ambiguous);
        return input;
    }
    if (rng.below(14) == 0)
    {
        const std::uint32_t length = 1 + rng.below(7);
        for (std::uint32_t i = 0; i < length; ++i)
            input.raw += static_cast<char>((rng.below(4) == 0 ? 'A' : 'a') + rng.below(26));
        return input;
    }
    auto syllables = split_key(rng.pick(keys));
    const std::uint32_t shape = rng.below(9);
    std::string raw;
    switch (shape)
    {
    case 0: // 整串
        break;
    case 1: // 前缀
        syllables.resize(1 + rng.below(static_cast<std::uint32_t>(syllables.size())));
        break;
    case 2: // 每个音节随机减成简拼
        for (auto &syllable : syllables)
            if (rng.below(2))
                syllable = initial_of(syllable);
        break;
    case 3: // 纯简拼
        for (auto &syllable : syllables)
            syllable = initial_of(syllable);
        break;
    case 4: // 最后一个音节没打完
    {
        auto &last = syllables.back();
        last = last.substr(0, 1 + rng.below(static_cast<std::uint32_t>(last.size())));
        break;
    }
    case 5: // ü 的别名写法
        for (auto &syllable : syllables)
            syllable = alias_of(syllable);
        break;
    case 6: // 前面混一个声母
        syllables.front() = initial_of(syllables.front());
        break;
    case 7: // 加一个多余的字母
        syllables.back() += static_cast<char>('a' + rng.below(26));
        break;
    default:
        break;
    }
    const bool keep_delimiters = rng.below(5) == 0;
    input.raw = join(syllables, keep_delimiters ? "'" : "");
    // 显式切分：把音节用分隔符接起来原样交给引擎。
    if (!keep_delimiters && rng.below(6) == 0)
        input.segmentation = join(syllables);
    return input;
}

void read_cases(std::ostream &out, const fs::path &resources)
{
    const fs::path user_dir = fs::temp_directory_path() / "msime_quanpin_probe_user";
    fs::remove_all(user_dir);
    fs::create_directories(user_dir);
    const metasequoia::RuntimePaths paths{resources, user_dir, fs::temp_directory_path() / "msime_quanpin_probe_cache", resources};
    const std::string journal = (user_dir / "msime_user.db").string();
    Lcg rng{0x9a117b3dULL};
    const auto keys = sample_keys(resources / "msime.db", rng);
    if (keys.empty())
    {
        std::cerr << "读不到词库键\n";
        std::exit(1);
    }
    const std::vector<std::string> ambiguous = {"yi", "ba", "nan", "zhao", "ban", "an", "xi", "an", "fang", "gan", "fan", "ni", "hao", "ge", "e", "ang", "ai", "ao", "er", "ou", "ji", "ni", "ta", "shi", "xian", "lu", "e", "jin", "tian", "biang", "gei", "yo", "dia", "shuang", "zhuang", "chuang", "guang", "qiong", "xiang", "nv", "lve"};

    QuanpinDictionary dictionary((resources / "msime.db").string(), paths);
    int cases = 0, nonempty = 0;
    for (int scenario = 0; scenario < 1300; ++scenario)
    {
        const ReadInput input = make_input(rng, keys, ambiguous);
        const std::uint32_t association = rng.below(3); // 0 无，1 词格，2 词格 + 显示下一条
        std::uint32_t fuzzy = rng.below(3) == 0 ? 1 + rng.below((1u << 11) - 1) : 0;
        if (fuzzy != 0 && rng.below(2))
            fuzzy = ((1u << 11) - 1) & ~(1u << rng.below(11)); // 几乎全开：近音路径很多，撞路径预算
        SentenceAssociationOptions options;
        options.word_lattice = association >= 1;
        options.show_next_on_duplicate = association == 2;
        dictionary.set_sentence_association(options);
        dictionary.reset_cache();

        // 用户在这个读音上调过频：日志里按“主切读音”记一条 upsert，之后的场景都能读到。
        std::string prefer;
        if (rng.below(7) == 0)
        {
            const auto cuts = quanpin::cut_pinyin_by_mode(input.raw, "correction");
            if (!cuts.empty())
            {
                prefer = quanpin::join_segments(cuts.front());
                user_dictionary::record_upsert(journal, user_dictionary::DictionaryKind::Pinyin, prefer, "测", 1);
            }
        }

        const std::string head = "{\"kind\":\"read\",\"case\":" + std::to_string(cases++) + ",\"raw\":" + quote(input.raw) + ",\"seg\":" + quote(input.segmentation) +
                                 ",\"assoc\":" + std::to_string(association) + ",\"fuzzy\":" + std::to_string(fuzzy) + ",\"prefer\":" + quote(prefer);

        const auto result = dictionary.query(input.raw, input.segmentation, 0, metasequoia::FuzzyPinyinOptions{fuzzy});
        nonempty += !result.empty();
        out << head << ",\"op\":\"query\",\"ret\":" << list_json(result) << ",\"seqs\":" << quote(dictionary.get_pinyin_sequence()) << ",\"segmentation\":" << quote(dictionary.get_pinyin_segmentation())
            << "}\n";

        if (fuzzy != 0)
        {
            const auto cuts = quanpin::cut_pinyin_by_mode(input.raw, "correction");
            const std::string typed = input.segmentation.empty() ? (cuts.empty() ? std::string() : quanpin::join_segments(cuts.front())) : input.segmentation;
            out << head << ",\"op\":\"fuzzy\",\"typed\":" << quote(typed) << ",\"ret\":" << list_json(dictionary.fuzzy_candidates(typed, metasequoia::FuzzyPinyinOptions{fuzzy})) << "}\n";
        }

        if (input.raw.size() == 1)
        {
            auto candidates = result;
            const bool expanded = dictionary.expand_initial_candidates(input.raw, candidates);
            out << head << ",\"op\":\"expand\",\"expanded\":" << (expanded ? "true" : "false") << ",\"ret\":" << list_json(candidates) << "}\n";
        }

        if (!result.empty() && rng.below(3) == 0)
        {
            const WordItem &chosen = result[rng.below(static_cast<std::uint32_t>(result.size()))];
            const std::string key = rng.below(6) == 0 ? chosen.canonical_pinyin + "'x" : (chosen.canonical_pinyin.empty() ? chosen.pinyin : chosen.canonical_pinyin);
            const auto found = dictionary.find_candidate(key, chosen.word);
            out << head << ",\"op\":\"find\",\"key\":" << quote(key) << ",\"value\":" << quote(chosen.word) << ",\"ret\":" << (found ? item_json(*found) : "null") << "}\n";
        }
    }
    // ---- 带纠错的读场景：手误输入（拼写错误的词库键、常见手误样例）× 纠错类型掩码 ----
    const std::vector<std::string> typo_pool = {"ziazheliya", "jioa", "jiuzheeyang", "nia", "niihao", "sahng", "shiideya", "uanli", "hauzh", "xiee", "bqng", "nbn", "sshang", "zhowei", "sholi",
                                                "banzheng", "gau", "yongan", "xianng", "buuhui", "zhngguo", "zaijinuta", "nihoa", "woaini", "shangg", "sjhang", "tinaqi", "mingtain", "zhonggou", "lia", "dia"};
    const std::vector<unsigned> masks = {0xd, 0xe, 0xf, 0xf, 0xf, 0x1, 0x2, 0x4, 0x8, 0x5, 0xb, 0x6, 0xc, 0x3, 0x7, 0x9, 0xa};
    for (int scenario = 0; scenario < 1100; ++scenario)
    {
        ReadInput input;
        if (rng.below(4) == 0)
            input.raw = rng.pick(typo_pool);
        else
        {
            input = make_input(rng, keys, ambiguous);
            if (input.raw.empty())
                continue;
            // 手误：漏字、多字、换位、替换，各来一两处。
            const std::uint32_t damages = 1 + rng.below(2);
            for (std::uint32_t d = 0; d < damages && input.raw.size() > 1; ++d)
            {
                const std::uint32_t at = rng.below(static_cast<std::uint32_t>(input.raw.size() - 1));
                switch (rng.below(4))
                {
                case 0: input.raw.erase(at, 1); break;
                case 1: input.raw.insert(at, 1, rng.below(2) ? input.raw[at] : static_cast<char>('a' + rng.below(26))); break;
                case 2: std::swap(input.raw[at], input.raw[at + 1]); break;
                default: input.raw[at] = static_cast<char>('a' + rng.below(26)); break;
                }
            }
            input.segmentation.clear();
        }
        const unsigned autocorrect = rng.pick(masks);
        const std::uint32_t association = rng.below(5) == 0 ? 0 : (rng.below(4) == 0 ? 2 : 1);
        std::uint32_t fuzzy = rng.below(8) == 0 ? 1 + rng.below((1u << 11) - 1) : 0;
        SentenceAssociationOptions options;
        options.word_lattice = association >= 1;
        options.show_next_on_duplicate = association == 2;
        dictionary.set_sentence_association(options);
        dictionary.reset_cache();

        std::string prefer;
        if (rng.below(8) == 0)
        {
            const auto cuts = quanpin::cut_pinyin_by_mode(input.raw, "correction");
            if (!cuts.empty())
            {
                prefer = quanpin::join_segments(cuts.front());
                user_dictionary::record_upsert(journal, user_dictionary::DictionaryKind::Pinyin, prefer, "测", 1);
            }
        }
        const std::string head = "{\"kind\":\"read\",\"case\":" + std::to_string(cases++) + ",\"raw\":" + quote(input.raw) + ",\"seg\":" + quote(input.segmentation) +
                                 ",\"assoc\":" + std::to_string(association) + ",\"fuzzy\":" + std::to_string(fuzzy) + ",\"prefer\":" + quote(prefer) + ",\"ac\":" + std::to_string(autocorrect);
        const auto result = dictionary.query(input.raw, input.segmentation, autocorrect, metasequoia::FuzzyPinyinOptions{fuzzy});
        nonempty += !result.empty();
        out << head << ",\"op\":\"query\",\"ret\":" << list_json(result) << ",\"seqs\":" << quote(dictionary.get_pinyin_sequence()) << ",\"segmentation\":" << quote(dictionary.get_pinyin_segmentation())
            << "}\n";
    }
    // ---- 开了领域词库的读场景：键取自领域库（加上原来的词库键，好撞上两边都有的读音），启用集合隔几个场景换一次 ----
    {
        auto domain_keys = sample_keys(resources / "domain-input.db", rng);
        if (domain_keys.empty())
        {
            std::cerr << "读不到领域库键\n";
            std::exit(1);
        }
        domain_keys.insert(domain_keys.end(), keys.begin(), keys.end());
        static const std::vector<std::string> packs = {"animals", "automotive", "finance", "food", "historical", "idioms", "it", "law", "medicine", "places", "poetry"};
        std::string enabled;
        int domain_nonempty = 0, domain_tagged = 0;
        for (int scenario = 0; scenario < 700; ++scenario)
        {
            if (scenario % 7 == 0)
            {
                enabled.clear();
                const std::uint32_t count = rng.below(5) == 0 ? packs.size() : 1 + rng.below(4);
                for (std::uint32_t i = 0; i < count; ++i)
                    enabled += (i ? "," : "") + rng.pick(packs);
                dictionary.set_enabled_domain_dictionaries(enabled);
            }
            const ReadInput input = make_input(rng, domain_keys, ambiguous);
            if (input.raw.empty())
                continue;
            const std::uint32_t association = rng.below(3);
            std::uint32_t fuzzy = rng.below(3) == 0 ? 1 + rng.below((1u << 11) - 1) : 0;
            const unsigned autocorrect = rng.below(4) == 0 ? 0xf : 0;
            SentenceAssociationOptions options;
            options.word_lattice = association >= 1;
            options.show_next_on_duplicate = association == 2;
            dictionary.set_sentence_association(options);
            dictionary.reset_cache();
            const std::string head = "{\"kind\":\"read\",\"case\":" + std::to_string(cases++) + ",\"raw\":" + quote(input.raw) + ",\"seg\":" + quote(input.segmentation) +
                                     ",\"assoc\":" + std::to_string(association) + ",\"fuzzy\":" + std::to_string(fuzzy) + ",\"prefer\":\"\",\"ac\":" + std::to_string(autocorrect) + ",\"dom\":" + quote(enabled);
            const auto result = dictionary.query(input.raw, input.segmentation, autocorrect, metasequoia::FuzzyPinyinOptions{fuzzy});
            nonempty += !result.empty();
            domain_nonempty += !result.empty();
            for (const auto &item : result)
                domain_tagged += !item.input_domains.empty();
            out << head << ",\"op\":\"query\",\"ret\":" << list_json(result) << ",\"seqs\":" << quote(dictionary.get_pinyin_sequence()) << ",\"segmentation\":" << quote(dictionary.get_pinyin_segmentation())
                << "}\n";
            if (fuzzy != 0)
            {
                const auto cuts = quanpin::cut_pinyin_by_mode(input.raw, "correction");
                const std::string typed = input.segmentation.empty() ? (cuts.empty() ? std::string() : quanpin::join_segments(cuts.front())) : input.segmentation;
                out << head << ",\"op\":\"fuzzy\",\"typed\":" << quote(typed) << ",\"ret\":" << list_json(dictionary.fuzzy_candidates(typed, metasequoia::FuzzyPinyinOptions{fuzzy})) << "}\n";
            }
            if (input.raw.size() == 1)
            {
                auto candidates = result;
                const bool expanded = dictionary.expand_initial_candidates(input.raw, candidates);
                out << head << ",\"op\":\"expand\",\"expanded\":" << (expanded ? "true" : "false") << ",\"ret\":" << list_json(candidates) << "}\n";
            }
        }
        dictionary.set_enabled_domain_dictionaries("");
        std::cerr << "领域读场景非空 " << domain_nonempty << "，带输入域的候选 " << domain_tagged << "\n";
    }
    std::cerr << "读场景 " << cases << " 个，非空 " << nonempty << "\n";
}

// ---------------------------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------------------------

const std::vector<std::string> kSyllables = {"ni", "hao", "ma", "shi", "jie", "wo", "men", "zhong", "guo", "ren", "da", "xue", "sheng", "ai", "de", "you", "yi", "bu", "ke", "ji", "xiang", "ming", "dian", "chang", "hang", "nve", "lve", "ju", "xu"};
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
    const std::string key = join(syllables);
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

void write_cases(std::ostream &out, const fs::path &resources, const fs::path &work)
{
    build_initial(work / "quanpin_dictionary_initial.db");
    Lcg rng{0x77a1b2c3ULL};
    int ops_total = 0, ops_ok = 0;
    for (int scenario = 0; scenario < 60; ++scenario)
    {
        const fs::path dir = work / "scenario";
        fs::remove_all(dir);
        fs::create_directories(dir / "user");
        fs::create_directories(dir / "dictionaries");
        fs::copy_file(work / "quanpin_dictionary_initial.db", dir / "dictionaries" / "msime.db");
        const metasequoia::RuntimePaths paths{resources, dir / "user", dir / "cache", dir / "dictionaries"};
        auto dictionary = std::make_unique<QuanpinDictionary>((dir / "dictionaries" / "msime.db").string(), paths);
        SentenceAssociationOptions options;
        options.word_lattice = scenario % 2 == 0;
        dictionary->set_sentence_association(options);
        out << "{\"kind\":\"scenario\",\"id\":" << scenario << ",\"assoc\":" << (options.word_lattice ? 1 : 0) << "}\n";

        const auto random_pinyin = [&](std::size_t syllables) {
            std::vector<std::string> picked;
            for (std::size_t i = 0; i < syllables; ++i)
                picked.push_back(rng.pick(kSyllables));
            return picked;
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
            const std::uint32_t kind = rng.below(11);
            if (kind >= 5 && kind < 9 && rng.below(3) != 0)
            {
                const std::string known = existing_word(dir / "dictionaries" / "msime.db", picked, rng);
                if (!known.empty())
                    word = known;
            }
            std::string spelled;
            const std::uint32_t form = rng.below(5); // 0 带分隔符，1/2 不带，3 带手误，4 ü 的别名写法
            if (form == 0)
                spelled = join(picked);
            else if (form == 4)
            {
                auto aliased = picked;
                for (auto &syllable : aliased)
                    syllable = alias_of(syllable);
                spelled = join(aliased, rng.below(2) ? "'" : "");
            }
            else
            {
                spelled = join(picked, "");
                if (form == 3)
                {
                    // 纠错切分认的几种手误：漏 g、颠倒 ng/h、ian 敲成 ain。
                    static const std::pair<const char *, const char *> typos[] = {{"ong", "on"}, {"eng", "egn"}, {"ang", "agn"}, {"ing", "ihng"}, {"ian", "ain"}, {"iang", "aing"}, {"ang", "ahng"}};
                    const auto &typo = typos[rng.below(7)];
                    const auto pos = spelled.find(typo.first);
                    if (pos != std::string::npos)
                        spelled.replace(pos, std::string(typo.first).size(), typo.second);
                }
            }
            std::string op, ret = "null";
            if (kind < 3)
            {
                op = "\"op\":\"create_word\",\"pinyin\":" + quote(spelled) + ",\"word\":" + quote(word);
                ret = std::to_string(dictionary->create_word(spelled, word));
            }
            else if (kind < 5)
            {
                const std::string canonical = rng.below(5) == 0 ? join(picked) + "'zz" : join(picked);
                op = "\"op\":\"create_word_from_canonical_pinyin\",\"pinyin\":" + quote(canonical) + ",\"word\":" + quote(word);
                ret = std::to_string(dictionary->create_word_from_canonical_pinyin(canonical, word));
            }
            else if (kind < 7)
            {
                op = "\"op\":\"update_weight\",\"pinyin\":" + quote(spelled) + ",\"word\":" + quote(word);
                ret = std::to_string(dictionary->update_weight_by_pinyin_and_word(spelled, word));
            }
            else if (kind < 8)
            {
                op = "\"op\":\"delete\",\"pinyin\":" + quote(spelled) + ",\"word\":" + quote(word);
                ret = std::to_string(dictionary->delete_by_pinyin_and_word(spelled, word));
            }
            else if (kind < 9)
            {
                // 先查一次让引擎记住切分，再按“当前切分”调频。
                const std::string raw = join(picked, "");
                const auto listed = dictionary->query(raw);
                op = "\"op\":\"update_by_word\",\"raw\":" + quote(raw) + ",\"word\":" + quote(word) + ",\"listed\":" + list_json(listed);
                ret = std::to_string(dictionary->update_weight_by_word(word));
            }
            else if (kind < 10)
            {
                const std::string raw = join(picked, "");
                op = "\"op\":\"query\",\"raw\":" + quote(raw);
                ret = list_json(dictionary->query(raw));
            }
            else
            {
                const std::string key = join(picked);
                op = "\"op\":\"find\",\"key\":" + quote(key) + ",\"value\":" + quote(word);
                const auto found = dictionary->find_candidate(key, word);
                ret = found ? item_json(*found) : "null";
            }
            ++ops_total;
            ops_ok += ret == "0";
            const std::string state = dump_main(dir / "dictionaries" / "msime.db") + "==\n" + dump_journal(dir / "user" / "msime_user.db");
            out << "{" << op << ",\"ret\":" << ret << ",\"state\":\"" << hash_text(state) << "\"}\n";
        }
        dictionary.reset();
    }
    fs::remove_all(work / "scenario");
    std::cerr << "写库场景 " << ops_total << " 步，成功 " << ops_ok << "\n";
}

// ---------------------------------------------------------------------------------------------
// sources
// ---------------------------------------------------------------------------------------------

void source_cases(std::ostream &out, const fs::path &resources, const fs::path &golden)
{
    const fs::path user_dir = fs::temp_directory_path() / "msime_quanpin_probe_sources";
    fs::remove_all(user_dir);
    fs::create_directories(user_dir);
    const metasequoia::RuntimePaths paths{resources, user_dir, fs::temp_directory_path() / "msime_quanpin_probe_sources_cache", resources};
    Lcg rng{0x50c3e5a1ULL};
    const auto keys = sample_keys(resources / "msime.db", rng);
    const std::vector<std::string> ambiguous = {"yi", "ba", "nan", "zhao", "ban", "an", "xi", "an", "fang", "gan", "fan", "ni", "hao", "ge", "e", "ang", "ai", "ao", "er", "ou", "ji", "ni", "ta", "shi", "xian", "lu", "e", "jin", "tian", "biang", "gei", "yo", "dia", "shuang", "zhuang", "chuang", "guang", "qiong", "xiang", "nv", "lve"};
    const std::vector<std::string> contexts = {"", "", "今天天气很好", "我们明天一起去", "他说", "请问", "公司的", "我想", "这本书讲的是"};
    const std::vector<unsigned> masks = {0xf, 0xf, 0x1, 0x2, 0x4, 0x8, 0x5, 0xb};
    const std::string keyboard_model = metasequoia::path_to_utf8(paths.resource(metasequoia::assets::neural_model_keyboard));
    const std::string gram_path = metasequoia::path_to_utf8(golden / "synthetic.gram");

    QuanpinDictionary dictionary((resources / "msime.db").string(), paths);
    auto &worker = neural::RescoreWorker::instance();
    int cases = 0, nonempty = 0, with_google = 0, with_neural = 0, reordered = 0, timed_out = 0, with_collocation = 0, fallback_rows = 0;
    for (int scenario = 0; scenario < 900; ++scenario)
    {
        ReadInput input = make_input(rng, keys, ambiguous);
        if (input.raw.empty())
            continue;
        if (rng.below(4) == 0 && input.raw.size() > 2)
        {
            const std::uint32_t at = rng.below(static_cast<std::uint32_t>(input.raw.size() - 1));
            if (rng.below(2))
                std::swap(input.raw[at], input.raw[at + 1]);
            else
                input.raw.erase(at, 1);
            input.segmentation.clear();
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
                options.neural_desktop_model = ""; // 资源目录里没有高精度档：来源静默退回
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
        const unsigned autocorrect = rng.below(4) == 0 ? masks[rng.below(static_cast<std::uint32_t>(masks.size()))] : 0;
        const std::uint32_t fuzzy = rng.below(8) == 0 ? 1 + rng.below((1u << 11) - 1) : 0;
        const std::string &context = contexts[rng.below(static_cast<std::uint32_t>(contexts.size()))];

        dictionary.set_sentence_association(options);
        dictionary.set_rescoring_context(context);
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

        const auto first = dictionary.query(input.raw, input.segmentation, autocorrect, metasequoia::FuzzyPinyinOptions{fuzzy});
        std::string second = "null";
        const bool neural = options.neural_keyboard || options.neural_desktop;
        if (neural)
        {
            // 每收到一次“算完了”就重查一次（真实的会话就是这么做的）。一次查询可能排了不止一批候选（主切分和字面切分各一批，
            // 每个模型只有一个待办位，后一批要等前一批算完），所以重查到再没有回调为止，取最后一次的结果。
            std::unique_lock<std::mutex> lock(signal->mutex);
            int seen = 0;
            std::vector<WordItem> latest;
            while (signal->wake.wait_for(lock, std::chrono::milliseconds(400), [&] { return signal->ready > seen; }))
            {
                seen = signal->ready;
                lock.unlock();
                dictionary.reset_cache();
                latest = dictionary.query(input.raw, input.segmentation, autocorrect, metasequoia::FuzzyPinyinOptions{fuzzy});
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
        with_collocation += !options.collocation_model.empty() && weight != 0.0;
        for (const auto &item : first)
            fallback_rows += item.source == CandidateSource::Fallback && item.sentence_association;
        out << "{\"kind\":\"sources\",\"case\":" << cases << ",\"raw\":" << quote(input.raw) << ",\"seg\":" << quote(input.segmentation) << ",\"ac\":" << autocorrect << ",\"fuzzy\":" << fuzzy
            << ",\"context\":" << quote(context) << ",\"lat\":" << (options.word_lattice ? 1 : 0) << ",\"google\":" << (options.google ? 1 : 0) << ",\"nk\":" << (options.neural_keyboard ? 1 : 0)
            << ",\"nd\":" << (options.neural_desktop ? 1 : 0) << ",\"ndm\":\"" << desktop_tag << "\",\"next\":" << (options.show_next_on_duplicate ? 1 : 0) << ",\"col\":" << (options.collocation_model.empty() ? 0 : 1)
            << ",\"colw\":" << weight << ",\"colr\":" << (options.collocation_rerank ? 1 : 0) << ",\"first\":" << list_json(first) << ",\"second\":" << second << "}\n";
    }
    dictionary.set_sentence_association(SentenceAssociationOptions{});
    std::cerr << "整句来源场景 " << cases << " 个，非空 " << nonempty << "，开 Google " << with_google << "，神经重排重查 " << with_neural << "（改变次序 " << reordered << "，超时 " << timed_out
              << "），万象 " << with_collocation << "，兜底/Google 整句行 " << fallback_rows << "\n";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "用法：msime_quanpin_dictionary_probe <资源目录> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path resources = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    fs::create_directories(work);
    std::ofstream out(argv[3], std::ios::binary);
    read_cases(out, resources);
    write_cases(out, resources, work);
    source_cases(out, resources, fs::absolute(argv[3]).parent_path());
    return 0;
}
