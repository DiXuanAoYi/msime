#include "domain_dictionary.h"
#include "../core/data_path.h"
#include <algorithm>
#include <sqlite3.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace domain_dictionary
{
const std::vector<std::string> &ids()
{
    static const std::vector<std::string> values = {"animals",    "automotive", "finance", "food",
                                                    "historical", "idioms",     "it",      "law",
                                                    "medicine",   "places",     "poetry"};
    return values;
}
std::string normalize_enabled(const std::string &csv)
{
    std::unordered_set<std::string> selected;
    std::istringstream input(csv);
    std::string id;
    while (std::getline(input, id, ','))
    {
        if (std::find(ids().begin(), ids().end(), id) != ids().end())
            selected.insert(id);
    }
    std::string result;
    for (const auto &value : ids())
        if (selected.count(value))
        {
            if (!result.empty())
                result += ',';
            result += value;
        }
    return result;
}
struct Dictionary::Impl
{
    std::filesystem::path path;
    sqlite3 *db = nullptr;
    std::string selected;
    std::unordered_map<std::string, sqlite3_stmt *> statements;
    std::unordered_map<std::string, std::vector<std::string>> tags;
    void clear()
    {
        for (auto &[sql, stmt] : statements)
            sqlite3_finalize(stmt);
        statements.clear();
        tags.clear();
    }
    ~Impl()
    {
        clear();
        if (db)
            sqlite3_close(db);
    }
    std::vector<std::string> membership(const std::string &key, const std::string &word)
    {
        const std::string cache_key = key + '\x1f' + word;
        const auto found = tags.find(cache_key);
        if (found != tags.end())
            return found->second;
        std::vector<std::string> result;
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT pack FROM entries WHERE key=?1 AND value=?2", -1, &stmt, nullptr) ==
            SQLITE_OK)
        {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, word.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                if (p && normalize_enabled(selected + ',' + p) == selected)
                    result.emplace_back(p);
            }
        }
        sqlite3_finalize(stmt);
        if (tags.size() >= 8192)
            tags.clear();
        tags.emplace(cache_key, result);
        return result;
    }
    std::vector<WordItem> convert(const std::vector<quanpin::KeyedQueryItem> &rows, const std::string &code)
    {
        std::vector<WordItem> result;
        for (const auto &row : rows)
        {
            WordItem item(code.empty() ? row.key : code, row.value, row.weight, CandidateSource::Database, row.key);
            item.input_domains = membership(row.key, row.value);
            result.push_back(std::move(item));
        }
        return result;
    }
};
Dictionary::Dictionary(std::filesystem::path path) : impl_(std::make_unique<Impl>())
{
    impl_->path = std::move(path);
}
Dictionary::~Dictionary() = default;
const std::string &Dictionary::enabled() const
{
    return impl_->selected;
}
bool Dictionary::set_enabled(const std::string &csv)
{
    const auto normalized = normalize_enabled(csv);
    if (normalized == impl_->selected)
        return false;
    impl_->clear();
    impl_->selected = normalized;
    if (!impl_->db && !normalized.empty())
    {
        // Never create or migrate this immutable asset in a user's directory.
        if (sqlite3_open_v2(metasequoia::path_to_utf8(impl_->path).c_str(), &impl_->db,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK)
        {
            sqlite3_close(impl_->db);
            impl_->db = nullptr;
        }
    }
    if (!impl_->db)
        return true;
    std::string filter = "'__none__'";
    std::istringstream input(normalized);
    std::string id;
    while (std::getline(input, id, ','))
        filter += ",'" + id + "'";
    // TEMP views live only in this connection. Existing phonetic query helpers therefore retain
    // their exact/prefix/mixed-jianpin rules, while main is opened read-only and never changed.
    sqlite3_stmt *list = nullptr;
    std::vector<std::string> tables;
    if (sqlite3_prepare_v2(impl_->db, "SELECT name FROM main.sqlite_master WHERE type='table' AND name LIKE 'tbl_%'",
                           -1, &list, nullptr) == SQLITE_OK)
        while (sqlite3_step(list) == SQLITE_ROW)
            tables.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(list, 0)));
    sqlite3_finalize(list);
    for (const auto &table : tables)
    {
        if (table.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
            continue;
        const auto sql = "DROP VIEW IF EXISTS temp.\"" + table + "\"; CREATE TEMP VIEW \"" + table +
                         "\" AS SELECT key,jp,value,weight FROM main.\"" + table + "\" WHERE pack IN (" + filter + ")";
        if (sqlite3_exec(impl_->db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
        {
            impl_->clear();
            sqlite3_close(impl_->db);
            impl_->db = nullptr; // Fail closed: never expose unfiltered main tables.
            break;
        }
    }
    return true;
}
std::vector<WordItem> Dictionary::query(const quanpin::Segments &segments, const std::string &code, int limit,
                                        quanpin::QuerySource source)
{
    if (!impl_->db || impl_->selected.empty() || segments.empty())
        return {};
    const auto rows = segments.size() == 1 && segments[0].size() == 1
                          ? quanpin::query_initial(impl_->db, segments[0], limit)
                          : quanpin::query_segments_keyed_flat(segments, impl_->db, impl_->statements, limit, source);
    return impl_->convert(rows, code);
}
std::vector<WordItem> Dictionary::exact(const std::vector<quanpin::Segments> &cuts, int limit)
{
    if (!impl_->db || impl_->selected.empty())
        return {};
    return impl_->convert(quanpin::query_exact_segmentations_keyed_flat(cuts, impl_->db, impl_->statements, limit), "");
}
quanpin::WordLatticeLookup Dictionary::lattice(int limit, metasequoia::FuzzyPinyinOptions fuzzy)
{
    return [this, limit, fuzzy](const quanpin::Segments &span) {
        if (!impl_->db || impl_->selected.empty())
            return std::vector<quanpin::LatticeLexeme>{};
        auto rows = quanpin::make_lattice_db_lookup(impl_->db, impl_->statements, limit, fuzzy)(span);
        for (auto &row : rows)
            row.input_domains = impl_->membership(row.key, row.value);
        return rows;
    };
}
void Dictionary::merge(std::vector<WordItem> &base, std::vector<WordItem> extra)
{
    for (auto &item : extra)
    {
        auto found = std::find_if(base.begin(), base.end(), [&](const WordItem &row) {
            return row.word == item.word && row.canonical_pinyin == item.canonical_pinyin;
        });
        if (found == base.end())
            base.push_back(std::move(item));
        else
            for (const auto &id : item.input_domains)
                if (std::find(found->input_domains.begin(), found->input_domains.end(), id) ==
                    found->input_domains.end())
                    found->input_domains.push_back(id);
    }
}
} // namespace domain_dictionary
