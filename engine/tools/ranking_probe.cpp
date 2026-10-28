// 调频探针：把一批“候选权重 + 选中项 + 调频模式”的用例喂给真实的 user_dictionary::adjust_candidate_ranking，
// 输出每个用例调完之后每一行的权重，用来当别的实现（Rust 核心）的行为规格。
//
// 用法：msime_ranking_probe <用例文件> <临时目录> [输出文件]
//
// 用例文件每行一条，列用 TAB 分隔：
//   候选<TAB>选中下标<TAB>模式<TAB>每次上移步数<TAB>触发次数<TAB>置顶<TAB>连选次数
// 候选是显示顺序里的 `权重:键`，逗号分隔，键只能是 a 或 b（a 是被调频的读音 ni，b 是混在同一列表里的另一个读音 wo）。
// 每个用例在全新的合成词库里跑：a 的候选写进 tbl_1_n，b 的写进 tbl_1_w，用户库是空的。
// 连选次数大于 1 时同一个候选用同一份（过时的）候选列表连选几次，用来走触发次数的计数。
// 空行和 # 开头的行跳过。输出每行一个 JSON：
//   {"case":原行,"results":[每次调用的返回值],"changed":[每次是否改了排序],"weights":[每行最终权重]}
#include "core/word_item.h"
#include "user_dictionary/user_dictionary_journal.h"

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct Row
{
    std::int64_t weight = 0;
    char key = 'a';
    std::string value;
};

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, separator))
        parts.push_back(part);
    return parts;
}

std::string json_string(const std::string &text)
{
    std::string out = "\"";
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\')
            out += std::string("\\") + static_cast<char>(c);
        else if (c == '\t')
            out += "\\t";
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}

bool exec(sqlite3 *db, const std::string &sql)
{
    return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::int64_t read_weight(sqlite3 *db, const Row &row)
{
    const std::string table = row.key == 'a' ? "tbl_1_n" : "tbl_1_w";
    const std::string key = row.key == 'a' ? "ni" : "wo";
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT weight FROM " + table + " WHERE key='" + key + "' AND value='" + row.value + "'";
    std::int64_t weight = -1;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        weight = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return weight;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "用法：msime_ranking_probe <用例文件> <临时目录> [输出文件]\n";
        return 2;
    }
    std::ifstream cases(argv[1]);
    if (!cases)
    {
        std::cerr << "打不开用例文件：" << argv[1] << "\n";
        return 2;
    }
    const std::filesystem::path scratch = argv[2];
    std::filesystem::create_directories(scratch);
    std::ofstream file;
    if (argc > 3)
        file.open(argv[3], std::ios::binary);
    std::ostream &out = argc > 3 ? static_cast<std::ostream &>(file) : std::cout;

    std::string line;
    int serial = 0;
    int failures = 0;
    while (std::getline(cases, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        const auto columns = split(line, '\t');
        if (columns.size() != 7)
        {
            std::cerr << "列数不对：" << line << "\n";
            ++failures;
            continue;
        }
        std::vector<Row> rows;
        for (const auto &cell : split(columns[0], ','))
        {
            const auto pair = split(cell, ':');
            Row row;
            row.weight = std::stoll(pair.at(0));
            row.key = pair.at(1).at(0);
            row.value = "w" + std::to_string(rows.size());
            rows.push_back(row);
        }
        const std::size_t selected = static_cast<std::size_t>(std::stoul(columns[1]));
        const std::string mode = columns[2];
        const int linear_step = std::stoi(columns[3]);
        const int trigger_count = std::stoi(columns[4]);
        const bool force_top = columns[5] == "1";
        const int calls = std::stoi(columns[6]);

        const auto main_path = scratch / ("main_" + std::to_string(serial) + ".db");
        const auto user_path = scratch / ("user_" + std::to_string(serial) + ".db");
        ++serial;
        std::filesystem::remove(main_path);
        std::filesystem::remove(user_path);
        sqlite3 *db = nullptr;
        if (sqlite3_open(main_path.string().c_str(), &db) != SQLITE_OK)
            return 2;
        for (const char *table : {"tbl_1_n", "tbl_1_w"})
            exec(db, std::string("CREATE TABLE ") + table + "(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        for (const auto &row : rows)
        {
            const std::string table = row.key == 'a' ? "tbl_1_n" : "tbl_1_w";
            const std::string key = row.key == 'a' ? "ni" : "wo";
            exec(db, "INSERT INTO " + table + " VALUES('" + key + "','" + key.substr(0, 1) + "','" + row.value + "'," +
                         std::to_string(row.weight) + ")");
        }
        sqlite3_close(db);

        std::vector<WordItem> candidates;
        for (const auto &row : rows)
        {
            const std::string key = row.key == 'a' ? "ni" : "wo";
            candidates.emplace_back(key, row.value, row.weight, CandidateSource::Database, key);
        }
        std::string results = "[", changed = "[";
        for (int call = 0; call < calls; ++call)
        {
            bool did_change = false;
            const bool ok = user_dictionary::adjust_candidate_ranking(
                main_path.string(), user_path.string(), "ni", candidates, "ni", rows.at(selected).value, mode,
                linear_step, trigger_count, force_top, &did_change);
            results += std::string(call ? "," : "") + (ok ? "true" : "false");
            changed += std::string(call ? "," : "") + (did_change ? "true" : "false");
        }
        results += "]";
        changed += "]";

        sqlite3_open_v2(main_path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
        std::string weights = "[";
        for (std::size_t i = 0; i < rows.size(); ++i)
            weights += (i ? "," : "") + std::to_string(read_weight(db, rows[i]));
        weights += "]";
        sqlite3_close(db);
        std::filesystem::remove(main_path);
        std::filesystem::remove(user_path);
        std::filesystem::remove(main_path.string() + "-wal");
        std::filesystem::remove(main_path.string() + "-shm");
        std::filesystem::remove(user_path.string() + "-wal");
        std::filesystem::remove(user_path.string() + "-shm");

        out << "{\"case\":" << json_string(line) << ",\"results\":" << results << ",\"changed\":" << changed
            << ",\"weights\":" << weights << "}\n";
    }
    return failures ? 1 : 0;
}
