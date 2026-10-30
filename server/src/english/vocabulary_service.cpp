#include "english/vocabulary_service.h"
#include "config/ime_config.h"
#include "utils/common_utils.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <set>
#include <sqlite3.h>

namespace Vocabulary
{
namespace
{
std::filesystem::path DatabasePath()
{
    return std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"vocabulary.db";
}
using FamiliarSet = std::set<std::pair<std::string, std::string>>;
std::shared_ptr<const FamiliarSet> familiar_snapshot = std::make_shared<FamiliarSet>();
void RefreshFamiliar()
{
    auto next = std::make_shared<FamiliarSet>();
    sqlite3 *db = nullptr;
    const auto path = DatabasePath().u8string();
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK)
    {
        sqlite3_stmt *s = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT language,word FROM vocabulary WHERE seen>=3 AND last_ms>=?", -1, &s,
                               nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(s, 1, NowMs() - int64_t(GetConfiguredVocabularyRetentionDays()) * 86400000);
            int rc;
            while ((rc = sqlite3_step(s)) == SQLITE_ROW)
                next->emplace(reinterpret_cast<const char *>(sqlite3_column_text(s, 0)),
                              reinterpret_cast<const char *>(sqlite3_column_text(s, 1)));
            if (rc != SQLITE_DONE)
            {
                sqlite3_finalize(s);
                sqlite3_close(db);
                return;
            }
        }
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    std::shared_ptr<const FamiliarSet> snapshot = next;
    std::atomic_store(&familiar_snapshot, std::move(snapshot));
}
class Writer
{
  public:
    Writer() : thread_([this] { Run(); })
    {
    }
    ~Writer()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }
    void Push(Batch batch)
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() < 128)
            queue_.push_back(std::move(batch));
        cv_.notify_one();
    }

  private:
    void Run()
    {
        RefreshFamiliar();
        for (;;)
        {
            Batch batch;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::seconds(10), [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty())
                {
                    if (stopping_)
                        return;
                    lock.unlock();
                    RefreshFamiliar();
                    continue;
                }
                batch = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!GetConfiguredVocabularyEnabled())
                continue;
            try
            {
                Store::Open(DatabasePath())->Record(batch, GetConfiguredVocabularyRetentionDays());
                RefreshFamiliar();
            }
            catch (...)
            { /* A locked or full disk must not interrupt typing. The settings page reports read errors. */
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Batch> queue_;
    bool stopping_ = false;
    std::thread thread_;
};
Writer &BackgroundWriter()
{
    static Writer writer;
    return writer;
}
} // namespace
bool IsFamiliar(const std::string &language, const std::string &word)
{
    if (!GetConfiguredVocabularyEnabled())
        return true;
    (void)BackgroundWriter();
    return std::atomic_load(&familiar_snapshot)->count({language, word}) != 0;
}
void Enqueue(Batch batch)
{
    if (!GetConfiguredVocabularyEnabled())
        return;
    BackgroundWriter().Push(std::move(batch));
}
boost::json::object HandleRequest(const boost::json::object &data)
{
    namespace json = boost::json;
    json::object response{{"requestId", data.at("requestId")},
                          {"type", "vocabularyResponse"},
                          {"protocolVersion", 1},
                          {"ok", true},
                          {"message", "成功"},
                          {"action", data.at("action")},
                          {"rows", json::array{}}};
    const auto action = json::value_to<std::string>(data.at("action"));
    try
    {
        const auto path = DatabasePath();
        // Merely opening settings, including with tracking disabled, creates no database.
        if (!std::filesystem::exists(path))
            return response;
        auto store = Store::Open(path);
        if (action == "clear")
        {
            store->Clear(NowMs());
            return response;
        }
        const auto entries = store->Read(GetConfiguredVocabularyRetentionDays(), NowMs());
        auto &rows = response["rows"].as_array();
        std::string content = "语言\t译词\t看到轮次\t选词次数\t直接使用次数\t首次记录毫秒\t最后记录毫秒\n";
        int64_t familiar = 0, selected = 0, used = 0;
        for (const auto &entry : entries)
        {
            familiar += entry.seen >= 3;
            selected += entry.selected > 0;
            used += entry.used > 0;
            if (rows.size() < 500)
                rows.push_back(json::object{{"language", entry.language},
                                            {"word", entry.word},
                                            {"seen", entry.seen},
                                            {"selected", entry.selected},
                                            {"used", entry.used},
                                            {"last", entry.last_ms}});
            if (action == "export")
                content += entry.language + '\t' + entry.word + '\t' + std::to_string(entry.seen) + '\t' +
                           std::to_string(entry.selected) + '\t' + std::to_string(entry.used) + '\t' +
                           std::to_string(entry.first_ms) + '\t' + std::to_string(entry.last_ms) + '\n';
        }
        response["total"] = entries.size();
        response["familiar"] = familiar;
        response["selected"] = selected;
        response["used"] = used;
        if (action == "export")
            response["content"] = content;
    }
    catch (...)
    {
        response["ok"] = false;
        response["message"] = "生词记录无法读写，请检查数据目录权限后重试。";
    }
    return response;
}
} // namespace Vocabulary
