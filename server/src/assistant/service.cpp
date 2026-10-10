#include "service.h"
#include "protocol.h"
#include "context_store.h"
#include "modes.h"
#include "config/ime_config.h"
#include "voice-input/voice_providers.h"
#include "utils/common_utils.h"
#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <chrono>
#include <stdexcept>
namespace ReplyStudio
{
using nlohmann::json;
namespace
{
struct Transfer
{
    std::string body;
    std::function<bool()> cancelled;
};
size_t Write(char *data, size_t size, size_t n, void *ptr)
{
    auto &t = *static_cast<Transfer *>(ptr);
    if (size && n > (1024 * 1024 - t.body.size()) / size)
        return 0;
    t.body.append(data, size * n);
    return size * n;
}
int Progress(void *ptr, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<Transfer *>(ptr)->cancelled() ? 1 : 0;
}
json Run(const json &request, const std::function<bool()> &cancelled)
{
    Validate(request);
    auto action = request.at("action").get<std::string>();
    if (cancelled())
        throw std::runtime_error("已取消。");
    if (action != "generate" && action != "summarize")
    {
        auto result = ContextStore(std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"reply-contexts.dat")
                          .Apply(request, std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
        result["modes"] = json::array();
        for (const auto &mode : Modes())
            result["modes"].push_back({{"id", mode.id}, {"name", mode.name}, {"description", mode.description}});
        return result;
    }
    auto config = GetConfiguredVoiceInput();
    auto token = VoiceInput::ResolvePolishToken(config);
    if (token.empty() || config.polish_endpoint.empty() || VoiceInput::ResolvePolishModel(config).empty())
        throw std::runtime_error("请先在“语音输入 → 润色”配置模型、接口和 API Token。回复助手共用该配置。");
    const auto &endpoint = config.polish_endpoint;
    if (endpoint.rfind("https://", 0) != 0 && endpoint.rfind("http://127.0.0.1:", 0) != 0 &&
        endpoint.rfind("http://localhost:", 0) != 0)
        throw std::runtime_error("模型接口需要 HTTPS，或本机 localhost / 127.0.0.1 地址。");
    auto payload = BuildPayload(request, VoiceInput::ResolvePolishModel(config));
    if (config.polish_provider == "deepseek")
        payload["thinking"] = {{"type", "disabled"}};
    if (config.polish_provider == "siliconflow")
        payload["enable_thinking"] = false;
    auto body = payload.dump();
    Transfer transfer{{}, cancelled};
    CURL *curl = curl_easy_init();
    if (!curl)
        throw std::runtime_error("网络初始化失败。");
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 45000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    auto rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (cancelled())
        throw std::runtime_error("已取消，原文保留。");
    if (rc != CURLE_OK)
        throw std::runtime_error(rc == CURLE_OPERATION_TIMEDOUT ? "模型响应超时，原文保留，可重试。"
                                                                : "网络请求失败，原文保留，请检查接口或网络。");
    if (status < 200 || status >= 300)
        throw std::runtime_error("模型请求失败（HTTP " + std::to_string(status) + "），请检查配额和配置。");
    try
    {
        return ParseResult(transfer.body, action, request.value("count", 3));
    }
    catch (...)
    {
        throw std::runtime_error("模型返回格式不符合要求，原文保留，请重试或更换支持 JSON 输出的模型。");
    }
}
} // namespace
Host::Host(std::function<void()> notify) : queue_(std::move(notify), {})
{
}
Host::~Host()
{
    Stop();
}
void Host::Submit(json request, Reply reply)
{
    json response = {
        {"type", "assistantResponse"},           {"protocolVersion", 1}, {"requestId", request.value("requestId", "")},
        {"action", request.value("action", "")}, {"ok", true},           {"message", "完成"}};
    if (request.value("action", "") == "cancel")
    {
        ++generation_;
        pending_.reset();
        response["message"] = "已取消，等待中的结果不会显示。";
        reply(response);
        return;
    }
    if (busy_)
    {
        ++generation_;
        pending_ = std::make_pair(std::move(request), std::move(reply));
        return;
    }
    busy_ = true;
    const auto generation = ++generation_;
    if (!queue_.Submit([this, request = std::move(request), response = std::move(response), reply = std::move(reply),
                        generation]() mutable -> SerialTaskQueue::Completion {
            try
            {
                response.update(Run(request, [this, generation] { return generation_.load() != generation; }));
            }
            catch (const std::exception &e)
            {
                response["ok"] = false;
                response["message"] = e.what();
            }
            return [this, generation, response = std::move(response), reply = std::move(reply)] {
                busy_ = false;
                if (generation_.load() == generation)
                    reply(response);
            };
        }))
        busy_ = false;
}
void Host::Drain(bool deliver)
{
    queue_.Drain(deliver);
    if (deliver && !busy_ && pending_)
    {
        auto next = std::move(*pending_);
        pending_.reset();
        Submit(std::move(next.first), std::move(next.second));
    }
}
void Host::Stop()
{
    ++generation_;
    pending_.reset();
    queue_.Stop();
}
bool Host::Stopped()
{
    return queue_.Stopped();
}
} // namespace ReplyStudio
