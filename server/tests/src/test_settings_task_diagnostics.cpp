#include "settings/settings_task_diagnostics.h"
#include "tests/includes/test_framework.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

TEST_CASE(settings_task_diagnostics_classifies_worker_failure_without_logging_content)
{
    std::vector<std::string> events;
    SettingsTaskDiagnostics::Context received{"unknown"};
    SettingsTaskDiagnostics::Failure failure;
    bool reported = false;
    auto task = SettingsTaskDiagnostics::Wrap(
        {"neuralModelsRequest", true, "status"},
        []() -> SerialTaskQueue::Completion {
            (void)nlohmann::json::object().at("SYNTHETIC_PRIVATE_KEY_DO_NOT_LOG");
            return {};
        },
        [&](const auto &context, const char *stage, const auto *error) {
            events.push_back(std::string(context.operation) + ":" + context.action + ":" + stage);
            if (error)
                failure = *error;
        },
        [&](const auto &context, const auto &error) {
            received = context;
            failure = error;
            reported = true;
        });
    auto complete = task();
    REQUIRE(!reported);
    REQUIRE_EQ(failure.json_id, 403);
    REQUIRE_EQ(std::string(failure.category), std::string("json"));
    complete();
    REQUIRE(reported);
    REQUIRE(received.automatic);
    REQUIRE_EQ(std::string(received.operation), std::string("neuralModelsRequest"));
    for (const auto &event : events)
        REQUIRE(event.find("SYNTHETIC_PRIVATE_KEY") == std::string::npos);
    REQUIRE_EQ(std::string(SettingsTaskDiagnostics::Operation("SYNTHETIC_PRIVATE_KEY")), std::string("unknown"));
    REQUIRE_EQ(std::string(SettingsTaskDiagnostics::Action("SYNTHETIC_PRIVATE_KEY")), std::string("unknown"));
}
TEST_CASE(settings_completion_failure_does_not_interrupt_later_accepted_work)
{
    std::mutex mutex;
    std::condition_variable ready;
    std::atomic<int> unwrapped{0};
    int reported = 0;
    bool nextDelivered = false;
    SerialTaskQueue queue([&] { ready.notify_all(); }, [&] { ++unwrapped; });
    const auto trace = [](const auto &, const char *, const auto *) {};
    const auto error = [&](const auto &, const auto &failure) {
        REQUIRE_EQ(std::string(failure.category), std::string("standard"));
        ++reported;
    };
    queue.Submit(SettingsTaskDiagnostics::Wrap(
        {"statsRequest", true, "overview"},
        [] { return SerialTaskQueue::Completion([] { throw std::runtime_error("SYNTHETIC_SECRET_URL_DO_NOT_LOG"); }); },
        trace, error));
    queue.Submit(SettingsTaskDiagnostics::Wrap(
        {"dictionaryRequest", false, "export"},
        [&] { return SerialTaskQueue::Completion([&] { nextDelivered = true; }); }, trace, error));
    queue.Stop();
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, std::chrono::seconds(3), [&] { return queue.Stopped(); }));
    }
    queue.Drain();
    REQUIRE_EQ(reported, 1);
    REQUIRE(nextDelivered);
    REQUIRE_EQ(unwrapped.load(), 0);
}
