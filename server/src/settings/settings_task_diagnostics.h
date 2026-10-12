#pragma once
#include "settings/serial_task_queue.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string_view>

namespace SettingsTaskDiagnostics
{
// Only fixed operation names may enter logs. Never include a request ID, config path/value, JSON, URL or
// exception.what().
inline const char *Operation(std::string_view name)
{
    static constexpr const char *known[] = {"dragStart",
                                            "resizeHitTest",
                                            "resizeStart",
                                            "focus",
                                            "windowControl",
                                            "maximizeButtonRect",
                                            "configRequest",
                                            "skinCatalogRequest",
                                            "openSkinDirectory",
                                            "openHelpcodeDirectory",
                                            "openShuangpinDirectory",
                                            "collocationModelDownload",
                                            "collocationModelDelete",
                                            "collocationModelImport",
                                            "collocationModelStatusRequest",
                                            "configUpdate",
                                            "defaultImeRequest",
                                            "neuralModelsRequest",
                                            "apiModelList",
                                            "aiTaskRequest",
                                            "apiCredentialTest",
                                            "openKeyboardPanel",
                                            "openHandwritingPanel",
                                            "restartServer",
                                            "openExternalUrl",
                                            "copyText",
                                            "dictionaryRequest",
                                            "speechRequest",
                                            "personalLibraryRequest",
                                            "voiceStudioRequest",
                                            "visionRequest",
                                            "localOcrRequest",
                                            "localTtsRequest",
                                            "localAsrRequest",
                                            "chatRequest",
                                            "assistantRequest",
                                            "dictionaryLibraryRequest",
                                            "vocabularyRequest",
                                            "statsRequest",
                                            "initialConfig",
                                            "configReload"};
    for (const auto *value : known)
        if (name == value)
            return value;
    return "unknown";
}
inline const char *Action(std::string_view name)
{
    static constexpr const char *known[] = {"activate",
                                            "add",
                                            "appCatalog",
                                            "cancel",
                                            "capture",
                                            "catalog",
                                            "clear",
                                            "clearAll",
                                            "copy",
                                            "copyFavorite",
                                            "create",
                                            "delete",
                                            "domainCatalog",
                                            "download",
                                            "export",
                                            "exportApplication",
                                            "exportDomain",
                                            "favorite",
                                            "favorites",
                                            "finish",
                                            "generate",
                                            "get",
                                            "groups",
                                            "import",
                                            "importApplication",
                                            "importHans",
                                            "importSubtitles",
                                            "list",
                                            "load",
                                            "longCapture",
                                            "openDirectory",
                                            "openSystemSettings",
                                            "overview",
                                            "play",
                                            "playFavorite",
                                            "poll",
                                            "preview",
                                            "putApplication",
                                            "putClip",
                                            "putGroup",
                                            "putRule",
                                            "query",
                                            "recognize",
                                            "recordStart",
                                            "recordStop",
                                            "refresh",
                                            "remove",
                                            "removeApplication",
                                            "removeGroup",
                                            "removeLearning",
                                            "reset",
                                            "review",
                                            "route",
                                            "run",
                                            "save",
                                            "saveImage",
                                            "scan",
                                            "screen",
                                            "search",
                                            "set",
                                            "setApplicationDomains",
                                            "setDefault",
                                            "setGroups",
                                            "setSettings",
                                            "settings",
                                            "start",
                                            "stats",
                                            "status",
                                            "stop",
                                            "study",
                                            "subtitles",
                                            "summarize",
                                            "synthesize",
                                            "transcribe",
                                            "translate",
                                            "translateSubtitles",
                                            "update"};
    if (name.empty() || name == "-")
        return "-";
    for (const auto *value : known)
        if (name == value)
            return value;
    return "unknown";
}
struct Context
{
    const char *operation;
    bool automatic = false;
    const char *action = "-";
};
struct Failure
{
    const char *category = "unknown";
    int json_id = 0;
};
using Trace = std::function<void(const Context &, const char *stage, const Failure *)>;
using Error = std::function<void(const Context &, const Failure &)>;

inline SerialTaskQueue::Task Wrap(Context context, SerialTaskQueue::Task task, Trace trace, Error report)
{
    context.operation = Operation(context.operation);
    context.action = Action(context.action);
    return [context, task = std::move(task), trace, report]() -> SerialTaskQueue::Completion {
        trace(context, "work.begin", nullptr);
        Failure failure;
        try
        {
            auto complete = task();
            trace(context, "work.end", nullptr);
            if (!complete)
                return {};
            return [context, complete = std::move(complete), trace, report] {
                trace(context, "completion.begin", nullptr);
                Failure failed;
                try
                {
                    complete();
                    trace(context, "completion.end", nullptr);
                    return;
                }
                catch (const nlohmann::json::exception &error)
                {
                    failed = {"json", error.id};
                }
                catch (const std::exception &)
                {
                    failed = {"standard", 0};
                }
                catch (...)
                {
                }
                trace(context, "completion.failed", &failed);
                report(context, failed);
            };
        }
        catch (const nlohmann::json::exception &error)
        {
            failure = {"json", error.id};
        }
        catch (const std::exception &)
        {
            failure = {"standard", 0};
        }
        catch (...)
        {
        }
        trace(context, "work.failed", &failure);
        return [context, failure, report] { report(context, failure); };
    };
}
} // namespace SettingsTaskDiagnostics
