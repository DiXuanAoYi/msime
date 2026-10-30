#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
namespace ReplyStudio
{
// Pure adapter and overlap logic: neither function can click, type or send.
nlohmann::json ParseChatWindow(const nlohmann::json &window);
size_t ChatAppendOffset(const nlohmann::json &before, const nlohmann::json &after);
nlohmann::json CaptureChatWindows(const std::function<bool()> &cancelled, bool selfTest = false);
class ChatReader
{
  public:
    explicit ChatReader(std::filesystem::path path) : path_(std::move(path))
    {
    }
    nlohmann::json Apply(const nlohmann::json &request, const std::function<bool()> &cancelled);
    // Public deterministic seam used by fixture tests; snapshots are never accepted from WebView.
    nlohmann::json Poll(const nlohmann::json &bindings, const nlohmann::json &windows, int64_t now);

  private:
    std::filesystem::path path_;
    std::map<std::string, nlohmann::json> baselines_;
};
} // namespace ReplyStudio
