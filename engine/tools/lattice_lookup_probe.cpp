// 词格词库查询探针：用真实的 quanpin::make_lattice_db_lookup 在随包的 msime.db 上查一批跨度，
// 当 Rust 实现的行为规格。
//
// 跨度取自词库里真有的键（随机抽表、随机抽行），再混入 ü 的另一种写法（jv/lue 这类）、没有的键和不完整的音节，
// 每个跨度配随机的模糊音规则和跨度上限。列表写成 {"n","h","top"}：条数、全部内容（键、词、权重、扣分）的哈希、前若干项。
//
// 用法：msime_lattice_lookup_probe <msime.db> [输出文件]
#include "quanpin/quanpin_query.h"
#include "quanpin/quanpin_utils.h"

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
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
        byte(0xff);
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

std::string number(double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

struct Table
{
    std::string name;
    int rows = 0;
};

std::vector<Table> list_tables(sqlite3 *db)
{
    std::vector<Table> tables;
    sqlite3_stmt *statement = nullptr;
    sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl\\_%' ESCAPE '\\' ORDER BY name", -1, &statement, nullptr);
    std::vector<std::string> names;
    while (sqlite3_step(statement) == SQLITE_ROW)
        names.push_back(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
    sqlite3_finalize(statement);
    for (const auto &name : names)
    {
        sqlite3_prepare_v2(db, ("SELECT count(*) FROM \"" + name + "\"").c_str(), -1, &statement, nullptr);
        if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) > 0)
            tables.push_back({name, sqlite3_column_int(statement, 0)});
        sqlite3_finalize(statement);
    }
    return tables;
}

std::string key_at(sqlite3 *db, const Table &table, int offset)
{
    sqlite3_stmt *statement = nullptr;
    sqlite3_prepare_v2(db, ("SELECT key FROM \"" + table.name + "\" LIMIT 1 OFFSET " + std::to_string(offset)).c_str(), -1, &statement, nullptr);
    std::string key;
    if (sqlite3_step(statement) == SQLITE_ROW)
        key = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    return key;
}

// ü 的另一种写法，以及几个故意查不到的变形。
std::string respell(const std::string &syllable, Lcg &rng)
{
    static const char *pairs[][2] = {{"ju", "jv"}, {"qu", "qv"}, {"xu", "xv"}, {"yu", "yv"}, {"jue", "jve"}, {"que", "qve"},
                                     {"xue", "xve"}, {"yue", "yve"}, {"lve", "lue"}, {"nve", "nue"}};
    for (const auto &pair : pairs)
        if (syllable == pair[0] && rng.below(2))
            return pair[1];
    return syllable;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "用法：msime_lattice_lookup_probe <msime.db> [输出文件]\n";
        return 2;
    }
    std::ofstream file;
    if (argc > 2)
        file.open(argv[2], std::ios::binary);
    std::ostream &out = argc > 2 ? static_cast<std::ostream &>(file) : std::cout;

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        std::cerr << "打不开词库：" << argv[1] << "\n";
        return 1;
    }
    const auto tables = list_tables(db);
    Lcg rng{0x1a771ce5ULL};
    std::unordered_map<std::string, sqlite3_stmt *> statement_cache;
    int nonempty = 0, fuzzy_penalized = 0, truncated = 0;
    const auto emit = [&](const quanpin::Segments &span, std::uint32_t rules, int span_limit) {
        metasequoia::FuzzyPinyinOptions fuzzy;
        fuzzy.rules = rules;
        const auto lexemes = quanpin::make_lattice_db_lookup(db, statement_cache, span_limit, fuzzy)(span);

        Hash hash;
        std::string top = "[";
        for (std::size_t i = 0; i < lexemes.size(); ++i)
        {
            const auto &lexeme = lexemes[i];
            hash.field(lexeme.key);
            hash.field(lexeme.value);
            hash.field(std::to_string(lexeme.weight));
            hash.field(number(lexeme.penalty));
            if (i < 12)
                top += std::string(i ? "," : "") + "[" + quote(lexeme.key) + "," + quote(lexeme.value) + "," + std::to_string(lexeme.weight) + "," +
                       number(lexeme.penalty) + "]";
            fuzzy_penalized += lexeme.penalty > 0;
        }
        top += "]";
        nonempty += !lexemes.empty();
        truncated += static_cast<int>(lexemes.size()) >= span_limit;
        char digest[24];
        std::snprintf(digest, sizeof(digest), "%016llx", static_cast<unsigned long long>(hash.value));
        out << "{\"kind\":\"lookup\",\"span\":[";
        for (std::size_t i = 0; i < span.size(); ++i)
            out << (i ? "," : "") << quote(span[i]);
        out << "],\"limit\":" << span_limit << ",\"rules\":" << rules << ",\"n\":" << lexemes.size() << ",\"h\":\"" << digest << "\",\"top\":" << top << "}\n";
    };
    for (int scenario = 0; scenario < 700; ++scenario)
    {
        quanpin::Segments span;
        if (rng.below(10) == 0)
        {
            // 随机音节（多半查不到）和不完整音节。
            const char *syllables[] = {"zh", "n", "ni", "hao", "lue", "jv", "xyz", "shei", "nue"};
            for (std::uint32_t i = 0, count = 1 + rng.below(3); i < count; ++i)
                span.push_back(syllables[rng.below(9)]);
        }
        else
        {
            const Table &table = tables[rng.below(static_cast<std::uint32_t>(tables.size()))];
            const std::string key = key_at(db, table, static_cast<int>(rng.below(static_cast<std::uint32_t>(table.rows))));
            span = quanpin::split_segments(key);
            for (auto &syllable : span)
                syllable = respell(syllable, rng);
        }
        if (span.empty())
            continue;
        emit(span, rng.below(3) == 0 ? 0 : rng.below(1u << 11), rng.below(8) == 0 ? 1 + static_cast<int>(rng.below(3)) : 4 + static_cast<int>(rng.below(60)));
    }
    // 全部单音节键各查一遍：多音字在近音读音下会重复出现，覆盖“同字只留排在前面的那行”。
    for (const auto &table : tables)
    {
        if (table.name.rfind("tbl_1_", 0) != 0)
            continue;
        sqlite3_stmt *statement = nullptr;
        sqlite3_prepare_v2(db, ("SELECT DISTINCT key FROM \"" + table.name + "\" ORDER BY key").c_str(), -1, &statement, nullptr);
        std::vector<std::string> keys;
        while (sqlite3_step(statement) == SQLITE_ROW)
            keys.push_back(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
        sqlite3_finalize(statement);
        for (const auto &key : keys)
            emit({key}, 1 + rng.below((1u << 11) - 1), 8 + static_cast<int>(rng.below(60)));
    }
    for (auto &[sql, statement] : statement_cache)
        sqlite3_finalize(statement);
    sqlite3_close(db);
    std::cerr << "非空 " << nonempty << "，带模糊扣分的词条 " << fuzzy_penalized << "，吃满上限 " << truncated << "\n";
    return 0;
}
