#include "ai_assistant.h"
#include "ai_assistant_cache_key.h"
#include "ai_assistant_request.h"
#include "utils/network_proxy.h"
#include "voice-input/voice_providers.h"
#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace
{
constexpr auto kIdleDelay = std::chrono::milliseconds(650);
std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_generation{0};
AiAssistant::Request g_latest;
std::chrono::steady_clock::time_point g_last_input;
AiAssistant::ApplyCallback g_callback;
AiAssistant::TaskCallback g_task_callback;
std::unordered_map<std::string, AiAssistant::Result> g_cache;
struct Response
{
    std::string text;
};
size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    if (size && count > SIZE_MAX / size)
        return 0;
    const auto bytes = size * count;
    auto &text = static_cast<Response *>(user)->text;
    if (bytes > 512 * 1024 - (std::min)(text.size(), size_t(512 * 1024)))
        return 0;
    text.append(data, bytes);
    return bytes;
}
int Progress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto *cancelled = static_cast<const std::function<bool()> *>(user);
    try
    {
        return *cancelled && (*cancelled)() ? 1 : 0;
    }
    catch (...)
    {
        return 1;
    }
}
void Deliver(AiAssistant::Result result, const AiAssistant::Request &request, uint64_t generation)
{
    if (!g_running || g_generation != generation)
        return;
    result.identity = request.identity;
    result.generation = generation;
    if (g_task_callback)
        g_task_callback(result);
    else if (result.task == AiAssistant::Task::Pinyin)
    {
        if (g_callback && !result.candidates.empty())
            g_callback(result.candidates.front().text, request.identity, generation);
    }
}
void WorkerLoop()
{
    uint64_t observed = UINT64_MAX;
    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] { return !g_running || g_generation != observed; });
        if (!g_running)
            break;
        observed = g_generation;
        auto request = g_latest;
        auto target = g_last_input + kIdleDelay;
        while (g_running && g_cv.wait_until(lock, target, [&] { return !g_running || g_generation != observed; }))
        {
            if (!g_running)
                return;
            observed = g_generation;
            request = g_latest;
            target = g_last_input + kIdleDelay;
        }
        if (request.identity.empty())
            continue;
        const auto key = AiAssistant::detail::BuildSuggestionCacheKey(request);
        const auto cached = g_cache.find(key);
        if (cached != g_cache.end())
        {
            auto result = cached->second;
            lock.unlock();
            Deliver(std::move(result), request, observed);
            continue;
        }
        lock.unlock();
        auto result = AiAssistant::RunTask(request, [observed] { return !g_running || g_generation != observed; });
        if (result.error.empty() && g_running && g_generation == observed)
        {
            {
                std::lock_guard cacheLock(g_mutex);
                if (g_cache.size() >= 128)
                    g_cache.clear();
                g_cache[key] = result;
            }
            Deliver(std::move(result), request, observed);
        }
    }
}
} // namespace

namespace AiAssistant
{
Result RunTask(const Request &request, const std::function<bool()> &cancelled)
{
    Result result;
    result.task = request.task;
    result.identity = request.identity;
    const auto stop = [&] { return cancelled && cancelled(); };
    if (stop())
    {
        result.error = "已取消";
        return result;
    }
    if (request.context.size() > 24000 || request.text.size() > 12000 || request.pinyin_segments.size() > 128 ||
        request.source_candidates.size() > 32)
    {
        result.error = "输入或上下文过长，请分段使用";
        return result;
    }
    size_t pinyinBytes = 0;
    for (const auto &segment : request.pinyin_segments)
    {
        if (segment.size() > 4096 - pinyinBytes)
        {
            result.error = "拼音输入过长，请分段使用";
            return result;
        }
        pinyinBytes += segment.size();
    }
    if (request.task == Task::Question)
    {
        result.candidates = LookupOfflineQuestion(request.text);
        if (result.candidates.empty())
        {
            std::string pinyin;
            for (const auto &segment : request.pinyin_segments)
                pinyin += segment;
            if (pinyin.size() <= 4096)
                result.candidates = LookupOfflineQuestion(pinyin);
        }
        if (!result.candidates.empty())
            return result;
    }
    const auto &config = request.config;
    const bool local = config.provider == "local";
    if (local && !IsLoopbackEndpoint(config.endpoint))
    {
        result.error = "本地 AI 地址必须是 localhost、127.0.0.1 或 ::1；不允许外网或重定向";
        return result;
    }
    if (!config.enabled || config.endpoint.empty() || config.model.empty() ||
        (!local && !IsLoopbackEndpoint(config.endpoint) && VoiceInput::IsPlaceholderToken(config.token)))
    {
        result.error = "请先配置 AI 接口地址、模型和必要的凭据；本地服务不要求 Token";
        return result;
    }
    if ((request.task == Task::Pinyin && request.pinyin_segments.empty()) ||
        (request.task != Task::Pinyin && request.text.empty() && request.context.empty()))
    {
        result.error = "请输入当前任务所需内容";
        return result;
    }
    std::string payload;
    try
    {
        payload = detail::BuildRequestBody(request).dump();
    }
    catch (...)
    {
        result.error = "输入或提示词编码无效，请检查后重试";
        return result;
    }
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        result.error = "无法初始化 AI 连接";
        return result;
    }
    curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (!config.token.empty())
        headers = curl_slist_append(headers, ("Authorization: Bearer " + config.token).c_str());
    Response response;
    curl_easy_setopt(curl, CURLOPT_URL, config.endpoint.c_str());
    if (local || IsLoopbackEndpoint(config.endpoint))
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    }
    else
        NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, detail::RequestTimeoutMs(config));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    const std::function<bool()> progressCancelled = stop;
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressCancelled);
    const auto code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (stop())
    {
        result.error = "已取消";
        return result;
    }
    if (code != CURLE_OK || status < 200 || status >= 300)
    {
        result.error = code != CURLE_OK ? std::string("AI 连接失败：") + curl_easy_strerror(code)
                                        : "AI 服务拒绝请求（HTTP " + std::to_string(status) + "）";
        return result;
    }
    try
    {
        const auto outer = nlohmann::json::parse(response.text);
        auto content = outer.at("choices").at(0).at("message").at("content").get<std::string>();
        const std::string fence(3, char(96));
        if (content.rfind(fence, 0) == 0)
        {
            const auto begin = content.find('\n'), end = content.rfind(fence);
            if (begin != std::string::npos && end > begin)
                content = content.substr(begin + 1, end - begin - 1);
        }
        result = ParseTaskContent(request.task, content,
                                  request.task == Task::Completion ? request.context + request.text : request.text,
                                  config.candidate_limit);
        result.identity = request.identity;
        result.rerank_only = IsLocalRerankResponse(request.task, config.provider, config.endpoint, outer);
    }
    catch (...)
    {
        result.error = "模型没有返回本任务的有效 JSON；请检查模型和提示词";
    }
    return result;
}
void Start(ApplyCallback callback, TaskCallback taskCallback)
{
    if (g_running)
        return;
    g_callback = std::move(callback);
    g_task_callback = std::move(taskCallback);
    g_running = true;
    g_worker = std::thread(WorkerLoop);
}
void Stop()
{
    if (!g_running)
        return;
    g_running = false;
    ++g_generation;
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
    std::lock_guard lock(g_mutex);
    g_cache.clear();
    g_latest = {};
}
void OnInputChanged(Request request)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest = std::move(request);
        g_last_input = std::chrono::steady_clock::now();
        ++g_generation;
    }
    g_cv.notify_one();
}
void Clear()
{
    OnInputChanged({});
}
} // namespace AiAssistant
