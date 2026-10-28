// 用户词库探针：随机生成一串操作（造词、删词、调频、固定位置、快捷短语/英文槽位、日期时间顺序、重放、
// 个人词库增删改……），用真实的 user_dictionary::* / metasequoia::* 在一份小的合成词库上顺序执行，
// 每步记录参数、返回值和执行后三个库（日志、msime.db、english.db）的状态哈希，当 Rust 实现的行为规格。
//
// 每个场景从同一份初始库（initial_main.db / initial_english.db，探针现建，也要提交）开始，日志从空开始。
// 日志里的 updated_at 来自 SQLite 的 unixepoch()，不进状态哈希；重放按 (updated_at, rowid) 排序，
// 而不同日志行很少落到词库的同一行上，所以次序基本不影响结果。
//
// 用法：msime_user_dictionary_probe <工作目录> <输出文件> [逐步状态转储文件]
//   工作目录下会写 initial_main.db、initial_english.db 和每个场景的临时目录（跑完删掉）。
#include "core/word_item.h"
#include "english/english_dictionary.h"
#include "user_dictionary/user_dictionary_journal.h"

#include <metasequoia/dictionary_state.h>
#include <metasequoia/personal_dictionary.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using user_dictionary::DictionaryKind;

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

void run(sqlite3 *db, const std::string &sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
    {
        std::cerr << "SQL 失败：" << sql << " —— " << (error ? error : "") << "\n";
        std::exit(1);
    }
}

// ---------------------------------------------------------------------------------------------
// 初始库
// ---------------------------------------------------------------------------------------------

void add_pinyin(sqlite3 *db, const std::string &key, const std::string &jp, const std::vector<std::pair<std::string, long long>> &rows)
{
    std::string escaped;
    for (const char c : key)
        escaped += c == '\'' ? std::string("''") : std::string(1, c);
    for (const auto &[value, weight] : rows)
        run(db, "INSERT INTO tbl_" + std::to_string(std::count(key.begin(), key.end(), '\'') + 1) + "_" + key.substr(0, 1) + "(key,jp,value,weight) VALUES('" + escaped + "','" +
                    jp + "','" + value + "'," + std::to_string(weight) + ")");
}

void build_initial(const fs::path &dir)
{
    fs::remove(dir / "initial_main.db");
    fs::remove(dir / "initial_english.db");
    sqlite3 *db = nullptr;
    sqlite3_open((dir / "initial_main.db").string().c_str(), &db);
    for (const char *table : {"tbl_1_n", "tbl_1_w", "tbl_1_j", "tbl_1_t", "tbl_1_d", "tbl_1_k", "tbl_1_l", "tbl_1_c", "tbl_1_m", "tbl_1_f", "tbl_2_n", "tbl_2_j", "tbl_3_n"})
        run(db, std::string("CREATE TABLE ") + table + "(key TEXT NOT NULL,jp TEXT NOT NULL,value TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0)");
    run(db, "CREATE TABLE wubi86(key TEXT NOT NULL,value TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0)");
    run(db, "CREATE TABLE quick_parases(key TEXT NOT NULL,value TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0)");
    add_pinyin(db, "ni", "n", {{"你", 9000000}, {"泥", 5000}, {"尼", 4000}, {"拟", 3000}, {"逆", 2900}, {"溺", 2900}, {"匿", 2899}, {"腻", 100}});
    add_pinyin(db, "na", "n", {{"那", 8000000}, {"拿", 600000}, {"哪", 500000}, {"纳", 90000}});
    add_pinyin(db, "wo", "w", {{"我", 20000000}, {"握", 3000}, {"卧", 2000}, {"沃", 1}});
    // 24 行紧挨着的权重：覆盖“没有整数中点”“占用权重冲突”和 16 行重排。
    std::vector<std::pair<std::string, long long>> jian = {{"见", 3460998}, {"间", 3000000}, {"建", 2000000}, {"件", 1000000}};
    const char *more[] = {"键", "剑", "渐", "健", "舰", "荐", "践", "监", "兼", "肩", "坚", "尖", "歼", "笺", "检", "减", "简", "碱", "拣", "茧"};
    long long weight = 1000;
    for (const char *value : more)
        jian.emplace_back(value, weight--);
    add_pinyin(db, "jian", "j", jian);
    add_pinyin(db, "ni'hao", "nh", {{"你好", 5000000}, {"泥好", 10}, {"拟好", 9}, {"逆好", 9}, {"尼好", 9}});
    add_pinyin(db, "ji'an", "ja", {{"吉安", 1}, {"积案", 5}, {"几案", 5}, {"急案", 5}, {"即按", 5}, {"激案", 5}});
    add_pinyin(db, "ni'hao'ma", "nhm", {{"你好吗", 100000}, {"你好嘛", 90000}});
    // 局部重排要走到的几种形状：权重超过上限的、成片相同的、紧挨着连成 16 个占用的。
    const auto rows_of = [](const std::string &characters, const auto &weight_at) {
        std::vector<std::pair<std::string, long long>> rows;
        for (std::size_t i = 0; i * 3 < characters.size(); ++i)
            rows.emplace_back(characters.substr(i * 3, 3), weight_at(i));
        return rows;
    };
    add_pinyin(db, "ta", "t", rows_of("他塔踏台太态泰贪摊滩坛谈潭痰谭坦毯叹探碳炭唐堂棠糖", [](std::size_t i) -> long long { return i == 0 ? 120000000 : i == 1 ? 110000000 : i < 4 ? 90000000 : 80000000; }));
    add_pinyin(db, "da", "d", rows_of("大达答搭打待代带袋戴单担丹胆旦但弹蛋当挡党刀倒到道稻盗德得的灯等登", [](std::size_t i) -> long long { return i < 17 ? 1000016 - static_cast<long long>(i) : i == 17 ? 999999 : 900000 - static_cast<long long>(i - 18) * 1000; }));
    add_pinyin(db, "li", "l", rows_of("里理力立历利李黎离礼丽例励粒隶栗莉厉俐", [](std::size_t i) -> long long { return i < 16 ? 18 - static_cast<long long>(i) : i == 16 ? 2 : i == 17 ? 1 : 0; }));
    // 恰好卡在边界上的：连号链撞到上限、权重逼近 int64 上限、阶梯末位恰好等于下限。
    add_pinyin(db, "ce", "c", rows_of("测册侧厕策", [](std::size_t i) -> long long { return 100000000 - static_cast<long long>(i); }));
    add_pinyin(db, "mx", "m", rows_of("满蔓慢漫曼", [](std::size_t i) -> long long { return INT64_MAX - static_cast<long long>(i) * 7; }));
    add_pinyin(db, "fl", "f", rows_of("发法翻反烦繁凡饭泛范贩犯方房防访放芳", [](std::size_t i) -> long long { return i < 2 ? 16001 : 100 - static_cast<long long>(i - 2); }));
    add_pinyin(db, "ke", "k", rows_of("可科颗课客刻克柯渴棵壳咳坷珂轲苛磕蝌瞌恪氪铪锞骒", [](std::size_t) -> long long { return 5000000; }));
    run(db, "INSERT INTO wubi86 VALUES('a','工',1000),('a','戈',500),('a','匚',500),('wq','他',5000),('wq','你',4000),('wq','们',3000),('gg','王',90000),('gg','五',50000)");
    run(db, "INSERT INTO quick_parases VALUES('dz','地址',100),('dz','电子邮件',90),('dz','住址',80),('xm','姓名',10)");
    sqlite3_close(db);

    const std::string english = (dir / "initial_english.db").string();
    if (!EnglishDictionary::ensure_schema(english))
    {
        std::cerr << "建不了 english.db\n";
        std::exit(1);
    }
    sqlite3_open(english.c_str(), &db);
    run(db, "INSERT INTO english_words VALUES('hello','hello',10),('hello','Hello',5),('help','help',20),('held','held',3),('he','he',1000),('hex','hex',7)");
    sqlite3_close(db);
}

// ---------------------------------------------------------------------------------------------
// 状态转储
// ---------------------------------------------------------------------------------------------

std::string dump_table(sqlite3 *db, const std::string &table, const std::string &order)
{
    std::string text = "T:" + table + "\n";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, ("SELECT * FROM \"" + table + "\" ORDER BY " + order).c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return text + "(无)\n";
    const int columns = sqlite3_column_count(statement);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        for (int i = 0; i < columns; ++i)
        {
            const auto *value = sqlite3_column_text(statement, i);
            text += std::string(i ? "|" : "") + (value ? reinterpret_cast<const char *>(value) : "<null>");
        }
        text += "\n";
    }
    sqlite3_finalize(statement);
    return text;
}

std::string dump_database(const fs::path &file, const std::vector<std::pair<std::string, std::string>> &tables)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        return "(打不开)\n";
    }
    std::string text;
    for (const auto &[table, order] : tables)
        text += dump_table(db, table, order);
    sqlite3_close(db);
    return text;
}

std::string dump_state(const fs::path &dir)
{
    // updated_at 不进转储；日志各表按主键排。
    std::string text = "== journal\n";
    {
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2((dir / "user" / "msime_user.db").string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK)
        {
            sqlite3_stmt *statement = nullptr;
            text += "T:user_dictionary_operations\n";
            sqlite3_prepare_v2(db, "SELECT dictionary,key,value,operation,weight,display,user_inserted FROM user_dictionary_operations ORDER BY 1,2,3", -1, &statement, nullptr);
            while (statement && sqlite3_step(statement) == SQLITE_ROW)
            {
                for (int i = 0; i < 7; ++i)
                    text += std::string(i ? "|" : "") + reinterpret_cast<const char *>(sqlite3_column_text(statement, i));
                text += "\n";
            }
            sqlite3_finalize(statement);
            sqlite3_close(db);
            text += dump_database(dir / "user" / "msime_user.db",
                                  {{"personal_dictionary_receipts", "1"},
                                   {"candidate_selection_state", "1,2,3"},
                                   {"fixed_candidate_positions", "1,2,3"},
                                   {"quick_phrase_slots", "1"},
                                   {"english_slots", "1"},
                                   {"date_time_format_order", "1,3"}});
        }
        else
            text += "(无日志)\n";
    }
    text += "== main\n";
    text += dump_database(dir / "dictionaries" / "msime.db",
                          {{"tbl_1_n", "1,3"}, {"tbl_1_w", "1,3"}, {"tbl_1_j", "1,3"}, {"tbl_1_t", "1,3"}, {"tbl_1_d", "1,3"}, {"tbl_1_k", "1,3"}, {"tbl_1_l", "1,3"}, {"tbl_1_c", "1,3"}, {"tbl_1_m", "1,3"}, {"tbl_1_f", "1,3"}, {"tbl_2_n", "1,3"}, {"tbl_2_j", "1,3"}, {"tbl_3_n", "1,3"},
                           {"wubi86", "1,2"}, {"quick_parases", "1,2"}});
    text += "== english\n";
    text += dump_database(dir / "dictionaries" / "english.db", {{"english_words", "1,2"}});
    return text;
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

// ---------------------------------------------------------------------------------------------
// 操作生成
// ---------------------------------------------------------------------------------------------

const char *source_name(CandidateSource source)
{
    switch (source)
    {
    case CandidateSource::Database: return "database";
    case CandidateSource::UserDatabase: return "user";
    case CandidateSource::CloudSuggestion: return "cloud";
    case CandidateSource::AiSuggestion: return "ai";
    case CandidateSource::EnglishDictionary: return "english";
    default: return "other";
    }
}

std::string candidates_json(const std::vector<WordItem> &items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += std::string(i ? "," : "") + "{\"pinyin\":" + quote(items[i].pinyin) + ",\"canonical\":" + quote(items[i].canonical_pinyin) + ",\"word\":" +
               quote(items[i].word) + ",\"weight\":" + std::to_string(items[i].weight) + ",\"source\":\"" + source_name(items[i].source) + "\",\"fixed\":" +
               std::to_string(items[i].fixed_position) + "}";
    return out + "]";
}

std::string strings_json(const std::vector<std::string> &items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? "," : "") + quote(items[i]);
    return out + "]";
}

const char *kind_label(DictionaryKind kind)
{
    switch (kind)
    {
    case DictionaryKind::Pinyin: return "pinyin";
    case DictionaryKind::Wubi: return "wubi";
    case DictionaryKind::QuickPhrase: return "quick";
    case DictionaryKind::English: return "english";
    }
    return "";
}

const char *personal_kind_label(metasequoia::PersonalDictionaryKind kind)
{
    switch (kind)
    {
    case metasequoia::PersonalDictionaryKind::Pinyin: return "pinyin";
    case metasequoia::PersonalDictionaryKind::Wubi: return "wubi";
    case metasequoia::PersonalDictionaryKind::QuickPhrase: return "quick";
    case metasequoia::PersonalDictionaryKind::English: return "english";
    }
    return "";
}

metasequoia::PersonalDictionaryKind personal_kind(const std::string &label)
{
    if (label == "wubi")
        return metasequoia::PersonalDictionaryKind::Wubi;
    if (label == "quick")
        return metasequoia::PersonalDictionaryKind::QuickPhrase;
    if (label == "english")
        return metasequoia::PersonalDictionaryKind::English;
    return metasequoia::PersonalDictionaryKind::Pinyin;
}

std::string personal_entry_json(const std::optional<metasequoia::PersonalDictionaryEntry> &entry)
{
    if (!entry)
        return "null";
    return std::string("{\"kind\":\"") + personal_kind_label(entry->kind) + "\",\"key\":" + quote(entry->key) + ",\"value\":" + quote(entry->value) +
           ",\"weight\":" + std::to_string(entry->weight) + "}";
}

struct World
{
    const std::vector<std::string> pinyin_keys = {"ni", "na", "wo", "jian", "ni'hao", "ji'an", "ni'hao'ma", "xx", "ni'", "Ni"};
    const std::vector<std::string> pinyin_values = {"你", "泥", "尼", "拟", "逆", "溺", "匿", "那", "我", "见", "间", "建", "键", "吉安", "积案", "你好", "你好吗", "新词", "测试"};
    const std::vector<std::string> wubi_keys = {"a", "wq", "gg", "zz", "abcde"};
    const std::vector<std::string> wubi_values = {"工", "戈", "匚", "他", "你", "们", "王", "五", "新"};
    const std::vector<std::string> quick_codes = {"dz", "xm", "yx"};
    const std::vector<std::string> quick_values = {"地址", "电子邮件", "住址", "姓名", "新短语"};
    const std::vector<std::string> english_words = {"hello", "help", "held", "he", "hex", "world", "Hello", "HELLO", "a1"};
    const std::vector<std::string> contexts = {"ni", "jian", "ni'hao", "n", "ctx"};
    const std::vector<std::string> categories = {"rq", "sj"};
    const std::vector<std::string> format_ids = {"d1", "d2", "d3", "d4", "d5"};
    const std::vector<std::string> modes = {"halve", "linear", "promote", "disabled", "other"};
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "用法：msime_user_dictionary_probe <工作目录> <输出文件> [逐步状态转储文件]\n";
        return 2;
    }
    const fs::path work = fs::absolute(argv[1]);
    fs::create_directories(work);
    build_initial(work);
    std::ofstream out(argv[2], std::ios::binary);
    std::ofstream debug;
    if (argc > 3)
        debug.open(argv[3], std::ios::binary);

    World world;
    int ops_total = 0, ops_true = 0;
    for (int scenario = 0; scenario < 160; ++scenario)
    {
        Lcg rng{0x5e12d1c7ULL + static_cast<std::uint64_t>(scenario) * 7919};
        const fs::path dir = work / "scenario";
        fs::remove_all(dir);
        fs::create_directories(dir / "user");
        fs::create_directories(dir / "dictionaries");
        fs::copy_file(work / "initial_main.db", dir / "dictionaries" / "msime.db");
        fs::copy_file(work / "initial_english.db", dir / "dictionaries" / "english.db");
        const std::string main_db = (dir / "dictionaries" / "msime.db").string();
        const std::string english_db = (dir / "dictionaries" / "english.db").string();
        const std::string journal = (dir / "user" / "msime_user.db").string();
        const metasequoia::RuntimePaths paths{dir, dir / "user", dir / "cache", dir / "dictionaries"};

        const auto kind_of = [&](const std::string &label) {
            if (label == "wubi")
                return DictionaryKind::Wubi;
            if (label == "quick")
                return DictionaryKind::QuickPhrase;
            if (label == "english")
                return DictionaryKind::English;
            return DictionaryKind::Pinyin;
        };
        const auto read_candidates = [&](const std::string &sql_key, DictionaryKind kind) {
            // 从词库里读一组候选，打乱次序，偶尔换来源或加噪声。
            std::vector<WordItem> items;
            sqlite3 *db = nullptr;
            sqlite3_open_v2(main_db.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
            std::string table = "wubi86";
            if (kind != DictionaryKind::Wubi)
            {
                const auto syllables = std::count(sql_key.begin(), sql_key.end(), '\'') + 1;
                table = "tbl_" + std::string(syllables <= 7 ? std::to_string(syllables) : "others") + "_" + sql_key.substr(0, 1);
            }
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(db, ("SELECT value,weight FROM \"" + table + "\" WHERE key=?1").c_str(), -1, &statement, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(statement, 1, sql_key.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(statement) == SQLITE_ROW)
                {
                    WordItem item;
                    item.pinyin = sql_key;
                    item.canonical_pinyin = rng.below(3) == 0 ? sql_key : "";
                    item.word = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
                    item.weight = sqlite3_column_int64(statement, 1);
                    item.source = rng.below(8) == 0 ? CandidateSource::UserDatabase : CandidateSource::Database;
                    items.push_back(std::move(item));
                }
            }
            sqlite3_finalize(statement);
            sqlite3_close(db);
            for (std::size_t i = items.size(); i > 1; --i)
                std::swap(items[i - 1], items[rng.below(static_cast<std::uint32_t>(i))]);
            if (rng.below(6) == 0)
            {
                WordItem noise;
                noise.pinyin = "x";
                noise.word = "云";
                noise.weight = 77;
                noise.source = rng.below(2) ? CandidateSource::CloudSuggestion : CandidateSource::Fallback;
                items.insert(items.begin() + rng.below(static_cast<std::uint32_t>(items.size() + 1)), noise);
            }
            if (rng.below(5) == 0 && items.size() > 1)
                items.pop_back();
            return items;
        };

        const std::uint32_t op_count = 8 + rng.below(18);
        out << "{\"kind\":\"scenario\",\"id\":" << scenario << "}\n";
        for (std::uint32_t step = 0; step < op_count; ++step)
        {
            std::ostringstream op;
            std::string ret = "null";
            const auto bool_ret = [&](bool value) {
                ret = value ? "true" : "false";
                ops_true += value;
            };
            const std::uint32_t pick = rng.below(40);
            const auto key_for = [&](const std::string &kind) -> std::string {
                if (kind == "wubi")
                    return rng.pick(world.wubi_keys);
                if (kind == "quick")
                    return rng.pick(world.quick_codes);
                if (kind == "english")
                    return rng.pick(world.english_words);
                return rng.pick(world.pinyin_keys);
            };
            const auto value_for = [&](const std::string &kind) -> std::string {
                if (kind == "wubi")
                    return rng.pick(world.wubi_values);
                if (kind == "quick")
                    return rng.pick(world.quick_values);
                if (kind == "english")
                    return rng.pick(world.english_words);
                return rng.pick(world.pinyin_values);
            };
            const std::vector<std::string> kinds = {"pinyin", "wubi", "quick", "english", "pinyin", "pinyin"};
            if (pick < 3)
            {
                const std::string kind = rng.pick(kinds), key = key_for(kind), value = value_for(kind), display = rng.below(2) ? value : "";
                const long long weight = static_cast<long long>(rng.below(5) == 0 ? 0 : rng.below(300000));
                const bool upsert = pick == 0;
                op << "\"op\":\"" << (upsert ? "record_upsert" : "record_user_insert") << "\",\"kind\":\"" << kind << "\",\"key\":" << quote(key) << ",\"value\":" << quote(value)
                   << ",\"weight\":" << weight << ",\"display\":" << quote(display);
                bool_ret(upsert ? user_dictionary::record_upsert(journal, kind_of(kind), key, value, weight, display)
                                : user_dictionary::record_user_insert(journal, kind_of(kind), key, value, weight, display));
            }
            else if (pick == 3)
            {
                const std::string kind = rng.pick(kinds), key = key_for(kind), value = value_for(kind);
                op << "\"op\":\"record_delete\",\"kind\":\"" << kind << "\",\"key\":" << quote(key) << ",\"value\":" << quote(value);
                bool_ret(user_dictionary::record_delete(journal, kind_of(kind), key, value));
            }
            else if (pick == 4)
            {
                std::string context = rng.pick(world.contexts), key = rng.pick(world.pinyin_keys), value = rng.pick(world.pinyin_values);
                int position = static_cast<int>(rng.below(8)) - 1;
                if (rng.below(3) == 0)
                {
                    // 日期时间的固定位置：上下文由类别决定，键和值都是格式 ID。
                    context = user_dictionary::date_time_fixed_position_context(rng.pick(world.categories));
                    key = value = rng.pick(world.format_ids);
                    position = 1 + static_cast<int>(rng.below(5));
                }
                op << "\"op\":\"set_fixed_position\",\"context\":" << quote(context) << ",\"key\":" << quote(key) << ",\"value\":" << quote(value) << ",\"position\":" << position;
                bool_ret(user_dictionary::set_fixed_position(journal, context, key, value, position));
            }
            else if (pick == 5)
            {
                const std::string context = rng.pick(world.contexts), key = rng.pick(world.pinyin_keys), value = rng.pick(world.pinyin_values);
                op << "\"op\":\"clear_fixed_position\",\"context\":" << quote(context) << ",\"key\":" << quote(key) << ",\"value\":" << quote(value);
                bool_ret(user_dictionary::clear_fixed_position(journal, context, key, value));
            }
            else if (pick == 6)
            {
                const std::string context = rng.pick(world.contexts), key = rng.pick(world.pinyin_keys), value = rng.pick(world.pinyin_values);
                op << "\"op\":\"is_fixed\",\"context\":" << quote(context) << ",\"key\":" << quote(key) << ",\"value\":" << quote(value);
                bool_ret(user_dictionary::is_fixed(journal, context, key, value));
            }
            else if (pick == 7 || pick == 8)
            {
                const std::string context = rng.pick(world.contexts);
                auto items = read_candidates(rng.pick(world.pinyin_keys), DictionaryKind::Pinyin);
                if (rng.below(2))
                {
                    WordItem ai;
                    ai.word = "智能";
                    ai.source = CandidateSource::AiSuggestion;
                    items.insert(items.begin() + rng.below(static_cast<std::uint32_t>(items.size() + 1)), ai);
                }
                const bool include_missing = rng.below(2), keep_dynamic = rng.below(3) == 0;
                op << "\"op\":\"apply_fixed_positions\",\"context\":" << quote(context) << ",\"candidates\":" << candidates_json(items) << ",\"include_missing\":" << include_missing
                   << ",\"keep_dynamic\":" << keep_dynamic;
                // 找不到的候选：字数是偶数就当找到，由回调造一个。
                const auto find = [](const std::string &key, const std::string &value) -> std::optional<WordItem> {
                    if (value.size() % 2 != 0)
                        return std::nullopt;
                    WordItem item;
                    item.pinyin = key;
                    item.word = value;
                    item.weight = static_cast<std::int64_t>(value.size());
                    return item;
                };
                user_dictionary::apply_fixed_positions(journal, context, items, include_missing, find, keep_dynamic);
                ret = candidates_json(items);
            }
            else if (pick >= 9 && pick <= 15)
            {
                const bool wubi = pick == 15;
                const std::string key = wubi ? rng.pick(world.wubi_keys) : rng.pick(std::vector<std::string>{"ni", "ni", "jian", "jian", "jian", "ji'an", "ni'hao", "wo", "na", "ni'hao'ma", "xx", "ta", "ta", "ta", "da", "da", "da", "li", "li", "li", "ce", "ce", "mx", "mx", "fl", "fl", "fl", "ke", "ke", "ke"});
                const auto items = read_candidates(key, wubi ? DictionaryKind::Wubi : DictionaryKind::Pinyin);
                std::string value = !items.empty() && rng.below(10) != 0 ? rng.pick(items).word : rng.pick(world.pinyin_values);
                std::string context = wubi ? key : (rng.below(3) == 0 ? rng.pick(world.contexts) : key);
                std::string mode = rng.below(5) == 0 ? "disabled" : rng.pick(world.modes);
                int linear_step = static_cast<int>(rng.below(6)) - 1, trigger = static_cast<int>(rng.below(5) == 0 ? 12 : rng.below(4));
                bool force_top = rng.below(5) == 0;
                if (!wubi && rng.below(2))
                {
                    // 把选中项摆到能走进各个边界分支的位置：线性上移一位落在下标 17 / 2 / 1，或者强制置顶。
                    struct Aim
                    {
                        const char *key;
                        long long weight;
                        bool top;
                    };
                    static const Aim aims[] = {{"da", 900000, false}, {"li", 0, false}, {"ce", 99999997, false}, {"fl", 100, false}, {"mx", 0, true}};
                    for (const auto &aim : aims)
                    {
                        if (key != aim.key)
                            continue;
                        for (const auto &item : items)
                            if (aim.top ? item.weight != INT64_MAX : item.weight == aim.weight)
                            {
                                value = item.word;
                                context = key;
                                mode = "linear";
                                linear_step = 1;
                                trigger = 1;
                                force_top = aim.top;
                            }
                    }
                }
                bool changed = false;
                op << "\"op\":\"adjust_candidate_ranking\",\"kind\":\"" << (wubi ? "wubi" : "pinyin") << "\",\"context\":" << quote(context) << ",\"candidates\":" << candidates_json(items)
                   << ",\"key\":" << quote(key) << ",\"value\":" << quote(value) << ",\"mode\":" << quote(mode) << ",\"linear_step\":" << linear_step << ",\"trigger\":" << trigger
                   << ",\"force_top\":" << force_top;
                const bool ok = user_dictionary::adjust_candidate_ranking(main_db, journal, context, items, key, value, mode, linear_step, trigger, force_top, &changed,
                                                                           wubi ? DictionaryKind::Wubi : DictionaryKind::Pinyin);
                ret = std::string("[") + (ok ? "true" : "false") + "," + (changed ? "true" : "false") + "]";
                ops_true += ok;
            }
            else if (pick == 16 || pick == 17)
            {
                // 英文候选：从 english.db 读一组。
                std::vector<WordItem> items;
                sqlite3 *db = nullptr;
                sqlite3_open_v2(english_db.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
                sqlite3_stmt *statement = nullptr;
                sqlite3_prepare_v2(db, "SELECT word,display,weight FROM english_words WHERE word LIKE 'he%' ORDER BY weight DESC", -1, &statement, nullptr);
                while (statement && sqlite3_step(statement) == SQLITE_ROW)
                {
                    WordItem item;
                    item.pinyin = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
                    item.word = reinterpret_cast<const char *>(sqlite3_column_text(statement, 1));
                    item.weight = sqlite3_column_int64(statement, 2);
                    item.source = CandidateSource::EnglishDictionary;
                    items.push_back(std::move(item));
                }
                sqlite3_finalize(statement);
                sqlite3_close(db);
                for (std::size_t i = items.size(); i > 1; --i)
                    std::swap(items[i - 1], items[rng.below(static_cast<std::uint32_t>(i))]);
                if (rng.below(3) == 0)
                {
                    WordItem chinese;
                    chinese.pinyin = "he";
                    chinese.word = "和";
                    chinese.weight = 9000000;
                    items.insert(items.begin(), chinese);
                }
                std::sort(items.begin(), items.end(), [](const WordItem &a, const WordItem &b) { return a.weight > b.weight; });
                const WordItem *chosen = items.empty() ? nullptr : &rng.pick(items);
                const std::string key = chosen && rng.below(8) ? chosen->pinyin : "he", value = chosen && rng.below(8) ? chosen->word : "hello";
                const std::string context = rng.pick(world.contexts), mode = rng.below(5) == 0 ? "disabled" : rng.pick(world.modes);
                const int linear_step = static_cast<int>(rng.below(5)) - 1, trigger = static_cast<int>(rng.below(4));
                const bool force_top = rng.below(4) == 0;
                bool changed = false;
                op << "\"op\":\"adjust_english_candidate_ranking\",\"context\":" << quote(context) << ",\"candidates\":" << candidates_json(items) << ",\"key\":" << quote(key)
                   << ",\"value\":" << quote(value) << ",\"mode\":" << quote(mode) << ",\"linear_step\":" << linear_step << ",\"trigger\":" << trigger << ",\"force_top\":" << force_top;
                const bool ok = user_dictionary::adjust_english_candidate_ranking(english_db, journal, context, items, key, value, mode, linear_step, trigger, force_top, &changed);
                ret = std::string("[") + (ok ? "true" : "false") + "," + (changed ? "true" : "false") + "]";
                ops_true += ok;
            }
            else if (pick == 18 || pick == 19)
            {
                const std::string kind = rng.pick(std::vector<std::string>{"pinyin", "wubi", "english", "quick", "pinyin"});
                std::string key = key_for(kind), value = value_for(kind);
                if (kind == "english")
                {
                    key = rng.pick(std::vector<std::string>{"hello", "help", "held", "he", "nope"});
                    value = rng.pick(std::vector<std::string>{"hello", "Hello", "help", "held", "he", "nope"});
                }
                op << "\"op\":\"delete_dictionary_candidate\",\"kind\":\"" << kind << "\",\"key\":" << quote(key) << ",\"value\":" << quote(value);
                bool_ret(user_dictionary::delete_dictionary_candidate(kind == "english" ? english_db : main_db, journal, kind_of(kind), key, value));
            }
            else if (pick == 20)
            {
                const std::string key = rng.pick(world.wubi_keys), value = rng.pick(world.wubi_values);
                op << "\"op\":\"bump_wubi_weight\",\"key\":" << quote(key) << ",\"value\":" << quote(value);
                bool_ret(user_dictionary::bump_wubi_weight(main_db, journal, key, value));
            }
            else if (pick == 21 || pick == 22)
            {
                const std::string code = rng.pick(world.quick_codes), value = rng.pick(world.quick_values);
                const bool first = rng.below(3) == 0;
                const std::string mode = rng.below(6) == 0 ? "disabled" : rng.pick(world.modes);
                const int linear_step = static_cast<int>(rng.below(4)), trigger = static_cast<int>(rng.below(4));
                if (pick == 21)
                {
                    op << "\"op\":\"learn_quick_phrase_selection\",\"code\":" << quote(code) << ",\"value\":" << quote(value) << ",\"first\":" << first << ",\"mode\":" << quote(mode)
                       << ",\"linear_step\":" << linear_step << ",\"trigger\":" << trigger;
                    bool_ret(user_dictionary::learn_quick_phrase_selection(main_db, journal, code, value, first, mode, linear_step, trigger));
                }
                else
                {
                    const int rank = static_cast<int>(rng.below(7)) - 1, max_slot = static_cast<int>(rng.below(7));
                    op << "\"op\":\"learn_quick_phrase_bypass\",\"code\":" << quote(code) << ",\"rank\":" << rank << ",\"mode\":" << quote(mode) << ",\"linear_step\":" << linear_step
                       << ",\"trigger\":" << trigger << ",\"max_slot\":" << max_slot;
                    bool_ret(user_dictionary::learn_quick_phrase_bypass(journal, code, rank, mode, linear_step, trigger, max_slot));
                }
            }
            else if (pick == 23)
            {
                const std::string code = rng.pick(world.quick_codes);
                op << "\"op\":\"quick_phrase_slot\",\"code\":" << quote(code);
                ret = std::to_string(user_dictionary::quick_phrase_slot(journal, code));
            }
            else if (pick >= 24 && pick <= 26)
            {
                const std::string code = rng.pick(std::vector<std::string>{"he", "hel", "hello", ""});
                const std::string mode = rng.below(6) == 0 ? "disabled" : rng.pick(world.modes);
                const int linear_step = static_cast<int>(rng.below(4)), trigger = static_cast<int>(rng.below(4));
                if (pick == 24)
                {
                    const int slot = static_cast<int>(rng.below(7)) - 1;
                    op << "\"op\":\"set_english_slot\",\"code\":" << quote(code) << ",\"slot\":" << slot;
                    bool_ret(user_dictionary::set_english_slot(journal, code, slot));
                }
                else if (pick == 25)
                {
                    const int index = static_cast<int>(rng.below(8)) - 1;
                    op << "\"op\":\"learn_english_slot_selection\",\"code\":" << quote(code) << ",\"index\":" << index << ",\"mode\":" << quote(mode) << ",\"linear_step\":" << linear_step
                       << ",\"trigger\":" << trigger;
                    bool_ret(user_dictionary::learn_english_slot_selection(journal, code, index, mode, linear_step, trigger));
                }
                else
                {
                    const int english_index = static_cast<int>(rng.below(6)) - 1, selected = static_cast<int>(rng.below(9)) - 1, max_slot = static_cast<int>(rng.below(8));
                    op << "\"op\":\"learn_english_slot_bypass\",\"code\":" << quote(code) << ",\"english_index\":" << english_index << ",\"selected\":" << selected << ",\"mode\":" << quote(mode)
                       << ",\"linear_step\":" << linear_step << ",\"trigger\":" << trigger << ",\"max_slot\":" << max_slot;
                    bool_ret(user_dictionary::learn_english_slot_bypass(journal, code, english_index, selected, mode, linear_step, trigger, max_slot));
                }
            }
            else if (pick == 27)
            {
                const std::string code = rng.pick(std::vector<std::string>{"he", "hel", ""});
                op << "\"op\":\"english_slot\",\"code\":" << quote(code);
                const auto slot = user_dictionary::english_slot(journal, code);
                ret = slot ? std::to_string(*slot) : "null";
            }
            else if (pick >= 28 && pick <= 30)
            {
                const std::string category = rng.below(10) == 0 ? "" : rng.pick(world.categories);
                std::vector<std::string> ids = world.format_ids;
                ids.resize(2 + rng.below(4));
                const std::string format_id = rng.below(8) == 0 ? "zz" : rng.pick(world.format_ids);
                const std::string mode = rng.below(6) == 0 ? "disabled" : rng.pick(world.modes);
                const int linear_step = static_cast<int>(rng.below(4)), trigger = static_cast<int>(rng.below(4));
                if (pick == 28)
                {
                    op << "\"op\":\"learn_date_time_selection\",\"category\":" << quote(category) << ",\"ids\":" << strings_json(ids) << ",\"format\":" << quote(format_id)
                       << ",\"mode\":" << quote(mode) << ",\"linear_step\":" << linear_step << ",\"trigger\":" << trigger;
                    bool_ret(user_dictionary::learn_date_time_selection(journal, category, ids, format_id, mode, linear_step, trigger));
                }
                else if (pick == 29)
                {
                    op << "\"op\":\"pin_date_time_format\",\"category\":" << quote(category) << ",\"ids\":" << strings_json(ids) << ",\"format\":" << quote(format_id);
                    bool_ret(user_dictionary::pin_date_time_format(journal, category, ids, format_id));
                }
                else
                {
                    std::vector<WordItem> items;
                    for (const auto &id : ids)
                    {
                        WordItem item;
                        item.pinyin = id;
                        item.word = "2026-" + id;
                        items.push_back(item);
                    }
                    const bool learned = rng.below(4) != 0;
                    op << "\"op\":\"apply_date_time_order\",\"category\":" << quote(category) << ",\"candidates\":" << candidates_json(items) << ",\"learned\":" << learned;
                    user_dictionary::apply_date_time_order(journal, category, items, learned);
                    ret = candidates_json(items);
                }
            }
            else if (pick == 31)
            {
                const std::string display = rng.pick(std::vector<std::string>{"Hello", "world", "Wor1d", "", "ABC", "x", std::string(64, 'q'), std::string(65, 'q')});
                const long long weight = static_cast<long long>(rng.below(60)) - 5;
                op << "\"op\":\"learn_entered_english_word\",\"display\":" << quote(display) << ",\"weight\":" << weight;
                bool_ret(user_dictionary::learn_entered_english_word(english_db, journal, display, weight));
            }
            else if (pick == 32)
            {
                const std::string key = rng.pick(world.pinyin_keys), value = rng.pick(world.pinyin_values);
                op << "\"op\":\"record_pinyin_upsert_from_database\",\"key\":" << quote(key) << ",\"value\":" << quote(value);
                bool_ret(user_dictionary::record_pinyin_upsert_from_database(main_db, key, value, journal));
            }
            else if (pick == 33)
            {
                const std::string kind = rng.pick(kinds), key = key_for(kind), value = value_for(kind);
                const bool inserted = rng.below(2);
                op << "\"op\":\"journal_queries\",\"kind\":\"" << kind << "\",\"key\":" << quote(key) << ",\"value\":" << quote(value) << ",\"which\":" << inserted;
                bool_ret(inserted ? user_dictionary::is_user_inserted(journal, kind_of(kind), key, value) : user_dictionary::has_user_upsert_for_key(journal, kind_of(kind), key));
            }
            else if (pick == 34)
            {
                op << "\"op\":\"replay\"";
                const auto result = user_dictionary::replay(journal, main_db, english_db);
                ret = "[" + std::to_string(result.applied) + "," + std::to_string(result.skipped) + "," + std::to_string(result.failed) + "," + quote(result.error) + "]";
            }
            else if (pick >= 35 && pick <= 37)
            {
                const std::string kind = rng.pick(std::vector<std::string>{"pinyin", "wubi", "quick", "english", "pinyin", "pinyin"});
                const auto make = [&]() -> metasequoia::PersonalDictionaryEntry {
                    metasequoia::PersonalDictionaryEntry entry;
                    entry.kind = personal_kind(kind);
                    if (kind == "pinyin")
                    {
                        entry.key = rng.pick(std::vector<std::string>{"ni hao", "NI'HAO", "ni'hao", "ni", "wo'men", "xx", "ni''hao"});
                        entry.value = rng.pick(std::vector<std::string>{"你好", "你", "我们", "泥好", "x"});
                    }
                    else if (kind == "wubi")
                    {
                        entry.key = rng.pick(std::vector<std::string>{"a", "WQ", "abcde", "1", "gg"});
                        entry.value = rng.pick(world.wubi_values);
                    }
                    else if (kind == "quick")
                    {
                        entry.key = rng.pick(std::vector<std::string>{"dz", "XM", "a1", "a-b"});
                        entry.value = rng.pick(std::vector<std::string>{"地址", "两行\n文本", "tab\t", "bad\x01"});
                    }
                    else
                    {
                        entry.key = rng.pick(std::vector<std::string>{"hello", "Hello", "help", "x1"});
                        entry.value = rng.pick(std::vector<std::string>{"hello", "Hello", "HELP", "nope"});
                    }
                    entry.weight = rng.below(10) == 0 ? 0 : static_cast<std::int64_t>(1 + rng.below(200000));
                    return entry;
                };
                const int mode = static_cast<int>(rng.below(3)); // 0 增 1 删 2 改
                std::optional<metasequoia::PersonalDictionaryEntry> previous, replacement;
                if (mode != 0)
                    previous = make();
                if (mode != 1)
                    replacement = make();
                if (mode == 2 && rng.below(2))
                    previous = replacement; // 同条改权重，旧条目常常真的存在
                const std::string request_id = rng.below(3) == 0 ? "" : rng.pick(std::vector<std::string>{"r1", "r2", "bad id", "r-3", std::string(128, 'a'), std::string(129, 'a')});
                op << "\"op\":\"edit_personal_dictionary\",\"previous\":" << personal_entry_json(previous) << ",\"replacement\":" << personal_entry_json(replacement)
                   << ",\"request_id\":" << quote(request_id);
                const auto result = metasequoia::edit_personal_dictionary(paths, previous, replacement, request_id);
                ret = std::string("[") + (result.success ? "true" : "false") + "," + quote(result.error) + "]";
                ops_true += result.success;
            }
            else if (pick == 38)
            {
                const std::size_t offset = rng.below(12) == 0 ? (rng.below(2) ? 1000001 : 1000000) : rng.below(4), limit = rng.below(10) == 0 ? (rng.below(2) ? 0 : 1001 - rng.below(2)) : 1 + rng.below(4);
                op << "\"op\":\"personal_dictionary_entries\",\"offset\":" << offset << ",\"limit\":" << limit;
                const auto page = metasequoia::personal_dictionary_entries(paths, offset, limit);
                ret = "{\"entries\":[";
                for (std::size_t i = 0; i < page.entries.size(); ++i)
                    ret += std::string(i ? "," : "") + personal_entry_json(page.entries[i]);
                ret += std::string("],\"has_more\":") + (page.has_more ? "true" : "false") + ",\"error\":" + quote(page.error) + "}";
            }
            else
            {
                op << "\"op\":\"dictionary_state\"";
                std::string records = "[";
                bool first = true;
                try
                {
                    metasequoia::stream_dictionary_state(paths, [&](const metasequoia::DictionaryStateRecord &record) {
                        std::string item;
                        if (const auto *entry = std::get_if<metasequoia::DictionaryStateEntry>(&record))
                            item = std::string("[\"entry\",\"") + personal_kind_label(entry->kind) + "\"," + quote(entry->key) + "," + quote(entry->value) + "," + std::to_string(entry->weight) +
                                   "," + quote(entry->display) + "," + (entry->deleted ? "true" : "false") + "," + (entry->user_inserted ? "true" : "false") + "]";
                        else if (const auto *position = std::get_if<metasequoia::DictionaryStatePosition>(&record))
                            item = "[\"position\"," + quote(position->context) + "," + quote(position->key) + "," + quote(position->value) + "," + std::to_string(position->position) + "]";
                        else
                        {
                            const auto &selection = std::get<metasequoia::DictionaryStateSelection>(record);
                            item = "[\"selection\"," + quote(selection.context) + "," + quote(selection.key) + "," + quote(selection.value) + "," + std::to_string(selection.count) + "]";
                        }
                        records += (first ? "" : ",") + item;
                        first = false;
                        return true;
                    });
                    ret = records + "]";
                }
                catch (const std::exception &)
                {
                    ret = "\"error\"";
                }
            }

            const std::string dump = dump_state(dir);
            if (debug)
                debug << "### scenario " << scenario << " step " << step << " " << op.str() << "\n" << dump;
            out << "{" << op.str() << ",\"ret\":" << ret << ",\"state\":\"" << hash_text(dump) << "\"}\n";
            ++ops_total;
        }
    }
    fs::remove_all(work / "scenario");
    std::cerr << ops_total << " 步操作，返回真 " << ops_true << "\n";
    return 0;
}
