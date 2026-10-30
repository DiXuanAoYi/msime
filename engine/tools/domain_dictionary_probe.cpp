// 领域词库探针：用真实的 domain_dictionary::Dictionary 在随包 domain-input.db 上导出，当 Rust 实现的行为规格。
//
//  normalize —— normalize_enabled：随机 csv（含不认识的名字、重复、空串、乱序）。
//  query / exact / lattice —— 随机启用的词包集合下，输入取自领域库真键的整串、前缀、简拼、混拼、没打完的尾音节：
//            query（全拼/双拼两种来源、随机 limit）、exact（一批切分）、lattice（随机模糊音规则、跨度上限），
//            记条数、内容哈希、前 8 项（含输入域）。
//  merge —— Dictionary::merge：两份真实查询结果互相并，记并后列表和每项的输入域。
//
// 用法：msime_domain_dictionary_probe <领域库路径> <输出文件>
#include "domain_dictionary/domain_dictionary.h"
#include "quanpin/quanpin_query.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
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

std::string strings_json(const std::vector<std::string> &items)
{
    std::string json = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        json += (i ? "," : "") + quote(items[i]);
    return json + "]";
}

std::string item_json(const WordItem &item)
{
    return "[" + quote(item.pinyin) + "," + quote(item.canonical_pinyin) + "," + quote(item.word) + "," + std::to_string(item.weight) + "," +
           std::to_string(static_cast<int>(item.source)) + "," + strings_json(item.input_domains) + "]";
}

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

std::string array_json(const std::vector<WordItem> &items)
{
    std::string json = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        json += (i ? "," : "") + item_json(items[i]);
    return json + "]";
}

std::string lexemes_json(const std::vector<quanpin::LatticeLexeme> &rows)
{
    std::string all;
    std::string top = "[";
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto &row = rows[i];
        const std::string one = "[" + quote(row.key) + "," + quote(row.value) + "," + std::to_string(row.weight) + "," + std::to_string(row.penalty) + "," + strings_json(row.input_domains) + "]";
        all += one + "\n";
        if (i < 8)
            top += (i ? "," : "") + one;
    }
    return "{\"n\":" + std::to_string(rows.size()) + ",\"h\":\"" + hash_text(all) + "\",\"top\":" + top + "]}";
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
        for (int i = 0; i < 5 && rows > 0; ++i)
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

quanpin::Segments shape_of(Lcg &rng, const std::vector<std::string> &keys)
{
    auto syllables = split_key(rng.pick(keys));
    switch (rng.below(7))
    {
    case 1:
        syllables.resize(1 + rng.below(static_cast<std::uint32_t>(syllables.size())));
        break;
    case 2:
        for (auto &syllable : syllables)
            if (rng.below(2))
                syllable = syllable.substr(0, 1);
        break;
    case 3:
        for (auto &syllable : syllables)
            syllable = syllable.substr(0, 1);
        break;
    case 4:
    {
        auto &last = syllables.back();
        last = last.substr(0, 1 + rng.below(static_cast<std::uint32_t>(last.size())));
        break;
    }
    case 5:
        syllables.front() = syllables.front().substr(0, 1);
        break;
    default:
        break;
    }
    return syllables;
}

std::string enabled_csv(Lcg &rng)
{
    static const std::vector<std::string> names = {"animals", "automotive", "finance", "food", "historical", "idioms", "it", "law", "medicine", "places", "poetry", "nonsense", "", "Animals"};
    std::string csv;
    const std::uint32_t count = 2 + rng.below(6);
    for (std::uint32_t i = 0; i < count; ++i)
        csv += (i ? "," : "") + rng.pick(names);
    return csv;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "用法：msime_domain_dictionary_probe <领域库路径> <输出文件>\n";
        return 2;
    }
    const fs::path db_path = fs::absolute(argv[1]);
    std::ofstream out(argv[2], std::ios::binary);
    Lcg rng{0xd0a1d1c7ULL};
    int id = 0;

    out << "{\"kind\":\"ids\",\"ret\":" << strings_json(domain_dictionary::ids()) << "}\n";
    for (int i = 0; i < 300; ++i)
    {
        std::string csv;
        const std::uint32_t pieces = rng.below(8);
        static const std::vector<std::string> names = {"animals", "automotive", "finance", "food", "historical", "idioms", "it", "law", "medicine", "places", "poetry", "x", "", " it", "it "};
        for (std::uint32_t k = 0; k < pieces; ++k)
            csv += (k ? "," : "") + rng.pick(names);
        out << "{\"kind\":\"normalize\",\"csv\":" << quote(csv) << ",\"ret\":" << quote(domain_dictionary::normalize_enabled(csv)) << "}\n";
    }

    const auto keys = sample_keys(db_path, rng);
    if (keys.empty())
    {
        std::cerr << "读不到领域库\n";
        return 1;
    }
    domain_dictionary::Dictionary dictionary(db_path);
    int nonempty = 0;
    for (int scenario = 0; scenario < 200; ++scenario)
    {
        const std::string csv = rng.below(10) == 0 ? "" : enabled_csv(rng);
        const bool changed = dictionary.set_enabled(csv);
        out << "{\"kind\":\"enable\",\"id\":" << id++ << ",\"csv\":" << quote(csv) << ",\"changed\":" << (changed ? "true" : "false") << ",\"enabled\":" << quote(dictionary.enabled()) << "}\n";
        for (int step = 0; step < 14; ++step)
        {
            const std::uint32_t op = rng.below(5);
            const auto segments = op >= 2 && rng.below(4) != 0 ? split_key(rng.pick(keys)) : shape_of(rng, keys);
            const std::string code = rng.below(3) == 0 ? "" : quanpin::join_segments(segments);
            if (op <= 1)
            {
                const int limit = rng.below(3) == 0 ? 24 : 128;
                const auto source = rng.below(3) == 0 ? quanpin::QuerySource::Shuangpin : quanpin::QuerySource::Quanpin;
                const auto rows = dictionary.query(segments, code, limit, source);
                nonempty += !rows.empty();
                out << "{\"kind\":\"query\",\"id\":" << id++ << ",\"segments\":" << strings_json(segments) << ",\"code\":" << quote(code) << ",\"limit\":" << limit << ",\"source\":"
                    << (source == quanpin::QuerySource::Shuangpin ? 1 : 0) << ",\"ret\":" << list_json(rows) << "}\n";
            }
            else if (op == 2)
            {
                std::vector<quanpin::Segments> cuts;
                const std::uint32_t count = 1 + rng.below(4);
                for (std::uint32_t k = 0; k < count; ++k)
                    cuts.push_back(rng.below(2) ? segments : shape_of(rng, keys));
                const int limit = rng.below(3) == 0 ? 1 : 128;
                const auto rows = dictionary.exact(cuts, limit);
                nonempty += !rows.empty();
                std::string cuts_json = "[";
                for (std::size_t k = 0; k < cuts.size(); ++k)
                    cuts_json += (k ? "," : "") + strings_json(cuts[k]);
                out << "{\"kind\":\"exact\",\"id\":" << id++ << ",\"cuts\":" << cuts_json << "],\"limit\":" << limit << ",\"ret\":" << list_json(rows) << "}\n";
            }
            else if (op == 3)
            {
                const int limit = rng.below(2) ? 32 : 4 + static_cast<int>(rng.below(60));
                const std::uint32_t rules = rng.below(2) ? 0 : rng.below(1u << 11);
                const auto rows = dictionary.lattice(limit, metasequoia::FuzzyPinyinOptions{rules})(segments);
                nonempty += !rows.empty();
                out << "{\"kind\":\"lattice\",\"id\":" << id++ << ",\"span\":" << strings_json(segments) << ",\"limit\":" << limit << ",\"rules\":" << rules << ",\"ret\":" << lexemes_json(rows) << "}\n";
            }
            else
            {
                auto base = dictionary.query(segments, code, 24);
                auto extra = dictionary.query(shape_of(rng, keys), "", 24);
                // 让一部分额外项和 base 撞上（同词同读音，只输入域不同）。
                for (std::size_t k = 0; k < extra.size() && k < base.size(); k += 3)
                {
                    extra[k].word = base[k].word;
                    extra[k].canonical_pinyin = base[k].canonical_pinyin;
                    extra[k].input_domains = {rng.pick(domain_dictionary::ids())};
                }
                const auto before = array_json(base);
                const auto extra_json = array_json(extra);
                domain_dictionary::Dictionary::merge(base, extra);
                out << "{\"kind\":\"merge\",\"id\":" << id++ << ",\"base\":" << before << ",\"extra\":" << extra_json << ",\"ret\":" << array_json(base) << "}\n";
            }
        }
    }
    std::cerr << "完成，非空查询 " << nonempty << "\n";
    return 0;
}
