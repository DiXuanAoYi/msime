#include "english/vocabulary_store.h"
#include <sqlite3.h>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>

namespace Vocabulary
{
namespace
{
void Check(int rc, sqlite3 *db)
{
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}
void Exec(sqlite3 *db, const char *sql)
{
    Check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), db);
}
struct Statement
{
    sqlite3_stmt *p = nullptr;
    Statement(sqlite3 *db, const char *sql)
    {
        Check(sqlite3_prepare_v2(db, sql, -1, &p, nullptr), db);
    }
    ~Statement()
    {
        sqlite3_finalize(p);
    }
};
bool ValidWord(const std::string &word)
{
    return !word.empty() && word.size() <= 1024 && word.find_first_of("\t\r\n") == std::string::npos &&
           word.find('\0') == std::string::npos;
}
} // namespace
int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::unique_ptr<Store> Store::Open(const std::filesystem::path &path)
{
    sqlite3 *db = nullptr;
    const int rc = sqlite3_open_v2(path.u8string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    auto result = std::unique_ptr<Store>(new Store(db));
    Check(rc, db);
    sqlite3_busy_timeout(db, 1000);
    Exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
             "CREATE TABLE IF NOT EXISTS vocabulary(language TEXT NOT NULL,word TEXT NOT NULL,"
             "seen INTEGER NOT NULL,selected INTEGER NOT NULL,used INTEGER NOT NULL,"
             "first_ms INTEGER NOT NULL,last_ms INTEGER NOT NULL,PRIMARY KEY(language,word));"
             "CREATE TABLE IF NOT EXISTS vocabulary_meta(id INTEGER PRIMARY KEY,cleared_ms INTEGER NOT NULL);"
             "INSERT OR IGNORE INTO vocabulary_meta VALUES(1,0);");
    return result;
}
Store::~Store()
{
    sqlite3_close(db_);
}
void Store::Prune(int days, int64_t now)
{
    if (days != 30 && days != 90 && days != 365)
        days = 90;
    Statement s(db_, "DELETE FROM vocabulary WHERE last_ms < ?");
    sqlite3_bind_int64(s.p, 1, now - int64_t(days) * 86400000);
    Check(sqlite3_step(s.p), db_);
    // Bound disk growth even if an application generates many distinct candidate words.
    Exec(db_, "DELETE FROM vocabulary WHERE rowid IN (SELECT rowid FROM vocabulary ORDER BY last_ms DESC "
              "LIMIT -1 OFFSET 100000)");
}
void Store::Record(const Batch &batch, int retention_days)
{
    static const std::set<std::string> languages{"en", "ja", "es", "fr", "de", "ru", "ko"};
    if (!languages.count(batch.language) || batch.timestamp_ms <= 0)
        return;
    std::map<std::string, Entry> counts;
    const auto add = [&](const std::vector<std::string> &words, int kind) {
        std::set<std::string> unique;
        for (const auto &word : words)
            if (ValidWord(word) && unique.size() < 64)
                unique.insert(word);
        for (const auto &word : unique)
        {
            auto &entry = counts[word];
            if (kind == 0)
                entry.seen = 1;
            if (kind == 1)
                entry.selected = 1;
            if (kind == 2)
            {
                entry.selected = 1;
                entry.used = 1;
            }
        }
    };
    add(batch.seen, 0);
    add(batch.selected, 1);
    add(batch.used, 2);
    Exec(db_, "BEGIN IMMEDIATE");
    try
    {
        Statement cleared(db_, "SELECT cleared_ms FROM vocabulary_meta WHERE id=1");
        Check(sqlite3_step(cleared.p), db_);
        if (batch.timestamp_ms > sqlite3_column_int64(cleared.p, 0))
        {
            Statement s(db_,
                        "INSERT INTO vocabulary VALUES(?,?,?,?,?,?,?) ON CONFLICT(language,word) DO UPDATE SET "
                        "seen=min(2147483647,seen+excluded.seen),selected=min(2147483647,selected+excluded.selected),"
                        "used=min(2147483647,used+excluded.used),last_ms=max(last_ms,excluded.last_ms)");
            for (const auto &[word, entry] : counts)
            {
                sqlite3_reset(s.p);
                sqlite3_bind_text(s.p, 1, batch.language.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s.p, 2, word.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(s.p, 3, entry.seen);
                sqlite3_bind_int64(s.p, 4, entry.selected);
                sqlite3_bind_int64(s.p, 5, entry.used);
                sqlite3_bind_int64(s.p, 6, batch.timestamp_ms);
                sqlite3_bind_int64(s.p, 7, batch.timestamp_ms);
                Check(sqlite3_step(s.p), db_);
            }
        }
        Prune(retention_days, batch.timestamp_ms);
        Exec(db_, "COMMIT");
    }
    catch (...)
    {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
std::vector<Entry> Store::Read(int retention_days, int64_t now_ms)
{
    Prune(retention_days, now_ms);
    Statement s(
        db_,
        "SELECT language,word,seen,selected,used,first_ms,last_ms FROM vocabulary ORDER BY last_ms DESC,language,word");
    std::vector<Entry> rows;
    int rc;
    while ((rc = sqlite3_step(s.p)) == SQLITE_ROW)
        rows.push_back({reinterpret_cast<const char *>(sqlite3_column_text(s.p, 0)),
                        reinterpret_cast<const char *>(sqlite3_column_text(s.p, 1)), sqlite3_column_int64(s.p, 2),
                        sqlite3_column_int64(s.p, 3), sqlite3_column_int64(s.p, 4), sqlite3_column_int64(s.p, 5),
                        sqlite3_column_int64(s.p, 6)});
    Check(rc, db_);
    return rows;
}
void Store::Clear(int64_t now_ms)
{
    Exec(db_, "BEGIN IMMEDIATE");
    try
    {
        Statement s(db_, "UPDATE vocabulary_meta SET cleared_ms=max(cleared_ms,?) WHERE id=1");
        sqlite3_bind_int64(s.p, 1, now_ms);
        Check(sqlite3_step(s.p), db_);
        Exec(db_, "DELETE FROM vocabulary; COMMIT");
    }
    catch (...)
    {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
} // namespace Vocabulary
