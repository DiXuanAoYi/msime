// 词库状态暂存探针：用真实的 stage_dictionary_state / prepare_runtime_paths 跑随机场景，当 Rust 实现的行为规格。
//
//  stage —— 一串随机记录（词条、固定位置、选择计数，夹着越界/重复/空键/超长等坏记录）、随机记录上限、
//           三种目标目录（新目录、已存在、落在资源目录里）、合法或非法的内容 ID；记成功与否、目录是否留下、
//           成功时暂存出的日志与词库转储哈希，以及从暂存结果再导出的状态。
//  prepare —— prepare_runtime_paths：新建、重复准备（日志里多了改动要重放进去）、三个根目录重叠、缺文件的旧代、
//             已有 .incoming 暂存目录、非法 ID。
//
// 初始库就是用户词库探针导出的 user_dictionary_initial_main.db / user_dictionary_initial_english.db。
// 用法：msime_dictionary_state_probe <initial_main.db> <initial_english.db> <工作目录> <输出文件>
#include "core/runtime_paths.h"
#include "user_dictionary/user_dictionary_journal.h"

#include <metasequoia/dictionary_state.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
using metasequoia::DictionaryStateEntry;
using metasequoia::DictionaryStatePosition;
using metasequoia::DictionaryStateRecord;
using metasequoia::DictionaryStateSelection;
using metasequoia::PersonalDictionaryKind;

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

std::string dump_table(sqlite3 *db, const std::string &table, const std::string &columns, const std::string &order)
{
    std::string text = "T:" + table + "\n";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, ("SELECT " + columns + " FROM \"" + table + "\" ORDER BY " + order).c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return text + "(无)\n";
    const int count = sqlite3_column_count(statement);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        for (int i = 0; i < count; ++i)
        {
            const auto *value = sqlite3_column_text(statement, i);
            text += std::string(i ? "|" : "") + (value ? reinterpret_cast<const char *>(value) : "<null>");
        }
        text += "\n";
    }
    sqlite3_finalize(statement);
    return text;
}

std::string dump_file(const fs::path &file, const std::vector<std::tuple<std::string, std::string, std::string>> &tables)
{
    sqlite3 *db = nullptr;
    if (!fs::exists(file) || sqlite3_open_v2(file.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        if (db)
            sqlite3_close(db);
        return "(无)\n";
    }
    std::string text;
    for (const auto &[table, columns, order] : tables)
        text += dump_table(db, table, columns, order);
    sqlite3_close(db);
    return text;
}

// updated_at 不进转储。
std::string dump_journal(const fs::path &file)
{
    return dump_file(file, {{"user_dictionary_operations", "dictionary,key,value,operation,weight,display,user_inserted", "1,2,3"},
                            {"candidate_selection_state", "context_key,entry_key,value,selection_count", "1,2,3"},
                            {"fixed_candidate_positions", "context_key,entry_key,value,position", "1,2,3"}});
}

std::string dump_dictionaries(const fs::path &dir)
{
    std::string text = "== main\n";
    text += dump_file(dir / "msime.db", {{"tbl_1_n", "*", "1,3"}, {"tbl_2_n", "*", "1,3"}, {"tbl_2_j", "*", "1,3"}, {"tbl_3_n", "*", "1,3"}, {"wubi86", "*", "1,2"}, {"quick_parases", "*", "1,2"}});
    text += "== english\n";
    text += dump_file(dir / "english.db", {{"english_words", "*", "1,2"}});
    return text;
}

std::string records_json(const std::vector<DictionaryStateRecord> &records)
{
    const auto kind_name = [](PersonalDictionaryKind kind) {
        switch (kind)
        {
        case PersonalDictionaryKind::Pinyin: return "pinyin";
        case PersonalDictionaryKind::Wubi: return "wubi";
        case PersonalDictionaryKind::English: return "english";
        default: return "quick";
        }
    };
    std::string json = "[";
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        json += i ? "," : "";
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DictionaryStateEntry>)
                    json += "[\"entry\",\"" + std::string(kind_name(value.kind)) + "\"," + quote(value.key) + "," + quote(value.value) + "," + std::to_string(value.weight) + "," + quote(value.display) + "," +
                            (value.deleted ? "1" : "0") + "," + (value.user_inserted ? "1" : "0") + "]";
                else if constexpr (std::is_same_v<T, DictionaryStatePosition>)
                    json += "[\"position\"," + quote(value.context) + "," + quote(value.key) + "," + quote(value.value) + "," + std::to_string(value.position) + "]";
                else
                    json += "[\"selection\"," + quote(value.context) + "," + quote(value.key) + "," + quote(value.value) + "," + std::to_string(value.count) + "]";
            },
            records[i]);
    }
    return json + "]";
}

// 流出来的状态，按流出的次序拼成串再哈希。
std::string streamed_hash(const metasequoia::RuntimePaths &paths)
{
    std::vector<DictionaryStateRecord> records;
    try
    {
        metasequoia::stream_dictionary_state(paths, [&](const DictionaryStateRecord &record) {
            records.push_back(record);
            return true;
        });
    }
    catch (const std::exception &)
    {
        return "error";
    }
    return hash_text(records_json(records));
}

void copy_fixture(const fs::path &main_db, const fs::path &english_db, const fs::path &resources)
{
    fs::remove_all(resources);
    fs::create_directories(resources);
    fs::copy_file(main_db, resources / "msime.db");
    fs::copy_file(english_db, resources / "english.db");
}

DictionaryStateRecord random_record(Lcg &rng)
{
    // 值带个随机编号，省得随手造的记录撞上同一个身份（撞上才是“重复”的场景，那个单独造）。
    const auto pick_value = [&](const std::vector<std::string> &pool) { return rng.pick(pool) + (rng.below(8) == 0 ? "" : std::to_string(rng.below(1000))); };
    static const std::vector<std::string> pinyin_keys = {"ni'hao", "n", "hao", "ni", "ke", "fl", "ni'hao'ma"};
    static const std::vector<std::string> values = {"你好", "拟好", "拟蒿", "测", "可", "发", "你们", "好啊", "x"};
    switch (rng.below(5))
    {
    case 0:
        return DictionaryStateEntry{PersonalDictionaryKind::Pinyin, rng.pick(pinyin_keys), pick_value(values), static_cast<std::int64_t>(rng.below(100000)), "", rng.below(4) == 0, rng.below(2) == 0};
    case 1:
        return DictionaryStateEntry{PersonalDictionaryKind::Wubi, rng.below(2) ? "wq" : "gg", pick_value(values), static_cast<std::int64_t>(rng.below(100000)), "", rng.below(4) == 0, rng.below(2) == 0};
    case 2:
        return DictionaryStateEntry{rng.below(2) ? PersonalDictionaryKind::English : PersonalDictionaryKind::QuickPhrase, rng.below(2) ? "hello" : "dz", pick_value(values), static_cast<std::int64_t>(rng.below(1000)),
                                    rng.below(3) == 0 ? "显示" : "", rng.below(4) == 0, rng.below(2) == 0};
    case 3:
        return DictionaryStatePosition{"c" + std::to_string(rng.below(60)), rng.pick(pinyin_keys), pick_value(values), static_cast<int>(1 + rng.below(5))};
    default:
        return DictionaryStateSelection{"c" + std::to_string(rng.below(60)), rng.pick(pinyin_keys), pick_value(values), static_cast<int>(rng.below(11))};
    }
}

// 把一条记录弄坏：越界数值、空键/空值、超长。
DictionaryStateRecord corrupt(Lcg &rng, DictionaryStateRecord record)
{
    std::visit(
        [&](auto &value) {
            using T = std::decay_t<decltype(value)>;
            switch (rng.below(5))
            {
            case 0: value.key.clear(); break;
            case 1: value.value.clear(); break;
            case 2: value.key.assign(513, 'k'); break;
            case 3: value.value.assign(4097, 'v'); break;
            default:
                if constexpr (std::is_same_v<T, DictionaryStateEntry>)
                    value.weight = rng.below(2) ? -1 : 100000001;
                else if constexpr (std::is_same_v<T, DictionaryStatePosition>)
                    value.position = rng.below(2) ? 0 : 6;
                else
                    value.count = rng.below(2) ? -1 : 11;
                break;
            }
        },
        record);
    return record;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::cerr << "用法：msime_dictionary_state_probe <initial_main.db> <initial_english.db> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path main_db = fs::absolute(argv[1]), english_db = fs::absolute(argv[2]), work = fs::absolute(argv[3]);
    fs::create_directories(work);
    std::ofstream out(argv[4], std::ios::binary);
    int ok_stage = 0, bad_stage = 0, id = 0;

    static const std::vector<std::string> good_ids = {"fixture", "gen-1", "A_b-9", "x"};
    static const std::vector<std::string> bad_ids = {"", "bad id", "a/b", "中文", std::string(129, 'z'), "a.b"};

    for (int scenario = 0; scenario < 260; ++scenario)
    {
        Lcg rng{0x57a7e5ULL + static_cast<std::uint64_t>(scenario) * 104729};
        const fs::path root = work / "scenario";
        fs::remove_all(root);
        const fs::path resources = root / "resources";
        copy_fixture(main_db, english_db, resources);

        std::vector<DictionaryStateRecord> records;
        const std::uint32_t count = rng.below(5) == 0 ? 0 : 1 + rng.below(24);
        for (std::uint32_t i = 0; i < count; ++i)
            records.push_back(random_record(rng));
        if (!records.empty() && rng.below(6) == 0)
            records[rng.below(static_cast<std::uint32_t>(records.size()))] = corrupt(rng, records[rng.below(static_cast<std::uint32_t>(records.size()))]);
        if (!records.empty() && rng.below(12) == 0)
            records.push_back(records[rng.below(static_cast<std::uint32_t>(records.size()))]); // 重复的身份
        std::size_t maximum = 500000;
        if (rng.below(6) == 0)
            maximum = records.empty() ? 0 : records.size() - (rng.below(2) ? 1 : 0);

        const std::uint32_t where = rng.below(8);
        fs::path generation = root / "gen";
        std::string place = "new";
        if (where == 0)
        {
            generation = resources / "gen";
            place = "inside";
        }
        else if (where == 1)
        {
            fs::create_directories(generation);
            place = "exists";
        }
        const std::string content = rng.below(7) == 0 ? rng.pick(bad_ids) : rng.pick(good_ids);

        bool ok = true;
        std::string state = "-";
        std::string streamed = "-";
        try
        {
            std::size_t next_index = 0;
            const auto staged = metasequoia::stage_dictionary_state(resources, generation, content, [&](DictionaryStateRecord &record) {
                if (next_index == records.size())
                    return false;
                record = records[next_index++];
                return true;
            }, maximum);
            state = hash_text(dump_journal(staged.user("msime_user.db")) + dump_dictionaries(staged.dictionaries) + (fs::exists(staged.dictionaries / ".ready") ? "ready\n" : "noready\n"));
            streamed = streamed_hash(staged);
        }
        catch (const std::exception &)
        {
            ok = false;
        }
        (ok ? ok_stage : bad_stage)++;
        out << "{\"kind\":\"stage\",\"id\":" << id++ << ",\"records\":" << records_json(records) << ",\"max\":" << maximum << ",\"place\":\"" << place << "\",\"content\":" << quote(content) << ",\"ok\":" << (ok ? "true" : "false")
            << ",\"gen_exists\":" << (fs::exists(generation) ? "true" : "false") << ",\"state\":\"" << state << "\",\"streamed\":\"" << streamed << "\"}\n";
    }

    int ok_prepare = 0;
    for (int scenario = 0; scenario < 80; ++scenario)
    {
        Lcg rng{0x9e9a5eULL + static_cast<std::uint64_t>(scenario) * 130363};
        const fs::path root = work / "scenario";
        fs::remove_all(root);
        const fs::path resources = root / "resources";
        copy_fixture(main_db, english_db, resources);
        fs::path user = root / "user", cache = root / "cache";
        const std::uint32_t mode = scenario % 8;
        std::string label;
        if (mode == 3)
        {
            user = resources / "user"; // 用户目录落在资源目录里
            label = "overlap_user";
        }
        else if (mode == 4)
        {
            cache = user / "cache"; // 缓存落在用户目录里
            label = "overlap_cache";
        }
        else
            label = std::vector<std::string>{"fresh", "again", "after_upsert", "", "", "incomplete", "incoming", "bad_id"}[mode];
        const std::string content = mode == 7 ? rng.pick(bad_ids) : rng.pick(good_ids);

        const std::string third = rng.below(2) ? "other-id" : content; // after_upsert 的第三次准备：换一个 ID 或原 ID
        std::string log;
        const auto attempt = [&](const std::string &id_value) {
            try
            {
                const auto paths = metasequoia::prepare_runtime_paths(resources, user, cache, id_value);
                log += "ok:" + hash_text(dump_journal(paths.user("msime_user.db")) + dump_dictionaries(paths.dictionaries) + (fs::exists(paths.dictionaries / ".ready") ? "ready\n" : "noready\n")) + ";";
                return true;
            }
            catch (const std::exception &)
            {
                log += "error;";
                return false;
            }
        };
        if (mode == 6)
            fs::create_directories(user / "dictionaries" / (content + ".incoming"));
        const bool first = attempt(content);
        if (first && mode >= 1 && mode != 3 && mode != 4 && mode != 6 && mode != 7)
        {
            if (mode == 2)
            {
                user_dictionary::record_upsert((user / "msime_user.db").string(), user_dictionary::DictionaryKind::Pinyin, "ni'hao", "拟好", 777);
                user_dictionary::record_user_insert((user / "msime_user.db").string(), user_dictionary::DictionaryKind::Wubi, "wq", "测", 123);
            }
            if (mode == 5)
                fs::remove(user / "dictionaries" / content / "english.db");
            attempt(content);
            if (mode == 2)
                attempt(third);
        }
        ok_prepare += first;
        out << "{\"kind\":\"prepare\",\"id\":" << id++ << ",\"case\":\"" << label << "\",\"content\":" << quote(content) << ",\"third\":" << quote(third) << ",\"log\":\"" << log << "\"}\n";
    }
    fs::remove_all(work / "scenario");
    std::cerr << "stage 成功 " << ok_stage << " 失败 " << bad_stage << "，prepare 首次成功 " << ok_prepare << "\n";
    return 0;
}
