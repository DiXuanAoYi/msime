#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;
namespace Vocabulary
{
struct Batch
{
    std::string language;
    std::vector<std::string> seen;
    std::vector<std::string> selected;
    std::vector<std::string> used;
    int64_t timestamp_ms = 0;
};
struct Entry
{
    std::string language, word;
    int64_t seen = 0, selected = 0, used = 0, first_ms = 0, last_ms = 0;
};
// Owns one connection; callers use separate connections on their worker threads.
class Store
{
  public:
    static std::unique_ptr<Store> Open(const std::filesystem::path &path);
    ~Store();
    void Record(const Batch &batch, int retention_days);
    std::vector<Entry> Read(int retention_days, int64_t now_ms);
    void Clear(int64_t now_ms);

  private:
    explicit Store(sqlite3 *db) : db_(db)
    {
    }
    void Prune(int retention_days, int64_t now_ms);
    sqlite3 *db_;
};
int64_t NowMs();
} // namespace Vocabulary
