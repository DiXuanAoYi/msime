#pragma once
#include "settings/serial_task_queue.h"
#include "chat_reader.h"
#include <memory>
#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <optional>
namespace ReplyStudio
{
using Reply = std::function<void(const nlohmann::json &)>;
// Runs one generate/summarize request synchronously on the calling (worker) thread.
nlohmann::json RunRequest(const nlohmann::json &request, const std::function<bool()> &cancelled);
// Created and drained on the owning window thread. One bounded request at a time.
class Host
{
  public:
    explicit Host(std::function<void()> notify, bool chat = false);
    ~Host();
    void Submit(nlohmann::json request, Reply reply);
    void Drain(bool deliver = true);
    void Stop();
    bool Stopped();

  private:
    std::unique_ptr<ChatReader> chat_;
    std::atomic<uint64_t> generation_{0};
    bool busy_{false};
    std::optional<std::pair<nlohmann::json, Reply>> pending_;
    SerialTaskQueue queue_;
};
} // namespace ReplyStudio
