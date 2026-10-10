#include "english/vocabulary_service.h"
#include "config/ime_config.h"
#include "utils/common_utils.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Vocabulary
{
namespace
{
std::filesystem::path DatabasePath()
{
    return std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"vocabulary.db";
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
        for (;;)
        {
            Batch batch;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty())
                    return;
                batch = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!GetConfiguredVocabularyEnabled())
                continue;
            try
            {
                Store::Open(DatabasePath())->Record(batch, GetConfiguredVocabularyRetentionDays());
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
} // namespace
void Enqueue(Batch batch)
{
    if (!GetConfiguredVocabularyEnabled())
        return;
    static Writer writer;
    writer.Push(std::move(batch));
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
