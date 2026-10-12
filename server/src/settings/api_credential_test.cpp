#include "api_credential_test.h"
#include "ai/ai_task.h"

#include "cloud/custom_translation.h"
#include "cloud/niutrans_translation.h"
#include "cloud/tencent_tmt.h"
#include "cloud/translation_gloss.h"
#include "voice-input/doubao_asr_client.h"
#include "voice-input/voice_batch_protocol.h"
#include <ctime>

#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace
{
constexpr long kConnectTimeoutMs = 5000;
constexpr long kRequestTimeoutMs = 15000;
constexpr std::size_t kMaxResponseBytes = 256 * 1024;
constexpr std::size_t kMaxModelListBytes = 8 * 1024 * 1024;

struct HttpResponse
{
    CURLcode code = CURLE_FAILED_INIT;
    long status = 0;
    std::string body;
    std::string error;
    std::size_t limit = kMaxResponseBytes;
};

std::string Value(const ApiCredentialTest::Request &request, const char *key)
{
    const auto found = request.config.find(key);
    return found == request.config.end() ? std::string{} : CloudTranslation::TrimSecret(found->second);
}

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    if (size && count > SIZE_MAX / size)
        return 0;
    const size_t bytes = size * count;
    auto *response = static_cast<HttpResponse *>(user);
    if (bytes > response->limit - (std::min)(response->body.size(), response->limit))
        return 0;
    response->body.append(data, bytes);
    return bytes;
}

void InitCurl()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool IsHttpEndpoint(const std::string &endpoint)
{
    return endpoint.rfind("https://", 0) == 0 || endpoint.rfind("http://", 0) == 0;
}

std::string ErrorDetail(const HttpResponse &response)
{
    if (response.code != CURLE_OK)
        return response.error.empty() ? curl_easy_strerror(response.code) : response.error;
    try
    {
        const auto root = nlohmann::json::parse(response.body);
        if (root.contains("error"))
        {
            const auto &error = root.at("error");
            if (error.is_object() && error.contains("message") && error.at("message").is_string())
                return error.at("message").get<std::string>();
            if (error.is_string())
                return error.get<std::string>();
        }
        if (root.contains("message") && root.at("message").is_string())
            return root.at("message").get<std::string>();
        if (root.contains("errorMsg") && root.at("errorMsg").is_string())
            return root.at("errorMsg").get<std::string>();
    }
    catch (...)
    {
    }
    return "HTTP " + std::to_string(response.status);
}

HttpResponse PerformJsonRequest(const std::string &endpoint, const std::string &token, const std::string *payload,
                                std::size_t limit = kMaxResponseBytes)
{
    InitCurl();
    HttpResponse response;
    response.limit = limit;
    CURL *curl = curl_easy_init();
    if (!curl)
        return response;
    char error[CURL_ERROR_SIZE] = {};
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string authorization = "Authorization: Bearer " + token;
    if (!token.empty())
        headers = curl_slist_append(headers, authorization.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    if (AiAssistant::IsLoopbackEndpoint(endpoint))
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    }
    else
        NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (payload)
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload->size()));
    }
    else
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    response.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.error = error;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse PerformJsonPost(const std::string &endpoint, const std::string &token, const std::string &payload)
{
    return PerformJsonRequest(endpoint, token, &payload);
}

ApiCredentialTest::Result TestChat(const ApiCredentialTest::Request &request)
{
    const std::string token = Value(request, "token");
    const std::string endpoint = Value(request, "endpoint");
    const std::string model = Value(request, "model");
    const bool loopback = AiAssistant::IsLoopbackEndpoint(endpoint);
    if (Value(request, "provider") == "local" && !loopback)
        return {false, "本地服务必须使用回环地址，不允许外网。"};
    if (!loopback && !CloudTranslation::IsUsableSecret(token))
        return {false, "请先填写有效的 API Key。"};
    if (!IsHttpEndpoint(endpoint) || model.empty())
        return {false, "请填写有效的 HTTPS 接口地址和模型名。"};
    nlohmann::json body = {{"model", model},
                           {"stream", false},
                           {"max_tokens", 1},
                           {"messages", {{{"role", "user"}, {"content", "Reply OK"}}}}};
    const std::string provider = Value(request, "provider");
    if (provider == "deepseek")
        body["thinking"] = {{"type", "disabled"}};
    else if (provider == "siliconflow")
        body["enable_thinking"] = false;
    const HttpResponse response = PerformJsonPost(endpoint, token, body.dump());
    if (response.code == CURLE_OK && response.status >= 200 && response.status < 300)
        return {true, loopback ? "连接成功，本地服务和模型配置有效；本地连接可不填 Token。"
                               : "连接成功，API Key 和模型配置有效。"};
    return {false, "测试失败：连接错误或 HTTP " + std::to_string(response.status) + "，请核对地址、模型和凭据。"};
}

void AppendLe16(std::vector<unsigned char> &out, std::uint16_t value)
{
    out.push_back(static_cast<unsigned char>(value));
    out.push_back(static_cast<unsigned char>(value >> 8));
}

void AppendLe32(std::vector<unsigned char> &out, std::uint32_t value)
{
    AppendLe16(out, static_cast<std::uint16_t>(value));
    AppendLe16(out, static_cast<std::uint16_t>(value >> 16));
}

std::vector<unsigned char> SilentWav()
{
    constexpr std::uint32_t sample_rate = 16000;
    constexpr std::uint32_t data_size = sample_rate * 2;
    std::vector<unsigned char> wav;
    wav.reserve(44 + data_size);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    AppendLe32(wav, 36 + data_size);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    AppendLe32(wav, 16);
    AppendLe16(wav, 1);
    AppendLe16(wav, 1);
    AppendLe32(wav, sample_rate);
    AppendLe32(wav, sample_rate * 2);
    AppendLe16(wav, 2);
    AppendLe16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    AppendLe32(wav, data_size);
    wav.resize(44 + data_size, 0);
    return wav;
}

ApiCredentialTest::Result TestBatchAsr(const ApiCredentialTest::Request &request)
{
    const std::string token = Value(request, "token");
    const std::string endpoint = Value(request, "endpoint");
    const std::string model = Value(request, "model");
    if (Value(request, "provider") != "local" && !CloudTranslation::IsUsableSecret(token))
        return {false, "请先填写有效的 API Key。"};
    if (!IsHttpEndpoint(endpoint) || model.empty())
        return {false, "请填写有效的 HTTPS 接口地址和模型名。"};

    InitCurl();
    HttpResponse response;
    CURL *curl = curl_easy_init();
    if (!curl)
        return {false, "无法初始化网络请求。"};
    char error[CURL_ERROR_SIZE] = {};
    VoiceInputConfig config;
    config.asr_provider = Value(request, "provider");
    config.asr_tokens[config.asr_provider] = token;
    config.asr_endpoint = endpoint;
    config.asr_model = model;
    config.tencent_secret_id = Value(request, "secretId");
    VoiceInput::AsrHttpRequest payload;
    try
    {
        payload = VoiceInput::BuildAsrHttpRequest(std::vector<float>(16000, 0), config, std::time(nullptr));
    }
    catch (const std::exception &e)
    {
        curl_easy_cleanup(curl);
        return {false, e.what()};
    }
    curl_slist *headers = nullptr;
    for (const auto &h : payload.headers)
        headers = curl_slist_append(headers, h.c_str());
    headers = curl_slist_append(headers, ("Content-Type: " + payload.content_type).c_str());
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    if (config.asr_provider == "local")
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    response.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    response.error = error;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (response.code == CURLE_OK && response.status >= 200 && response.status < 300)
    {
        try
        {
            VoiceInput::ParseAsrHttpResponse(response.body, config.asr_provider);
        }
        catch (const std::exception &e)
        {
            return {false, e.what()};
        }
        return {true, "服务已接受测试音频，配置有效。"};
    }
    return {false, "测试失败：" + ErrorDetail(response)};
}

ApiCredentialTest::Result TestTranslation(const ApiCredentialTest::Request &request)
{
    std::vector<std::string> translated;
    if (request.service == "translation.tencent")
    {
        const TencentTmt::Credentials credentials{Value(request, "secretId"), Value(request, "secretKey"),
                                                  "ap-guangzhou"};
        if (!CloudTranslation::IsUsableSecret(credentials.secret_id) ||
            !CloudTranslation::IsUsableSecret(credentials.secret_key))
            return {false, "请先填写有效的 SecretId 和 SecretKey。"};
        translated = TencentTmt::TextTranslateBatch(credentials, {"测试"}, "zh", "en");
    }
    else if (request.service == "translation.niutrans")
    {
        const NiuTransTranslation::Config config{Value(request, "appId"), Value(request, "apiKey")};
        if (!NiuTransTranslation::IsUsableConfig(config))
            return {false, "请先填写有效的 APP ID 和 API Key。"};
        translated = NiuTransTranslation::TextTranslateBatch(config, {"测试"}, "zh", "en");
    }
    else
    {
        const CustomTranslation::Config config{Value(request, "endpoint"), Value(request, "apiKey")};
        if (!CustomTranslation::IsSupportedEndpoint(config.endpoint))
            return {false, "请先填写有效的翻译接口地址。"};
        translated = CustomTranslation::TextTranslateBatch(config, {"测试"}, "ZH", "EN");
    }
    if (!translated.empty() && !translated.front().empty())
        return {true, "连接成功，翻译服务配置有效。"};
    return {false, "测试失败：服务未返回有效译文，请检查凭据、接口地址和网络。"};
}
} // namespace

namespace ApiCredentialTest
{
std::string ModelListEndpoint(const std::string &endpoint)
{
    const auto query = endpoint.find_first_of("?#");
    std::string path = endpoint.substr(0, query);
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    constexpr std::string_view suffix = "/chat/completions";
    if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
        path.resize(path.size() - suffix.size());
    if (path.size() < 7 || path.compare(path.size() - 7, 7, "/models") != 0)
        path += "/models";
    return path + (query == std::string::npos ? std::string() : endpoint.substr(query));
}
ModelListResult FetchModels(const Request &request)
{
    if (request.service != "ai.assistant" && request.service != "voice.polish" && request.service != "translation.ai")
        return {false, "当前任务不支持查询模型名", {}};
    const auto endpoint = Value(request, "endpoint"), token = Value(request, "token");
    const bool loopback = AiAssistant::IsLoopbackEndpoint(endpoint);
    if (Value(request, "provider") == "local" && !loopback)
        return {false, "本地服务必须使用回环地址", {}};
    if (!IsHttpEndpoint(endpoint))
        return {false, "请填写有效的接口地址", {}};
    if (!loopback && !CloudTranslation::IsUsableSecret(token))
        return {false, "请先填写有效 API Key", {}};
    const auto response = PerformJsonRequest(ModelListEndpoint(endpoint), token, nullptr, kMaxModelListBytes);
    if (response.code != CURLE_OK || response.status < 200 || response.status >= 300)
        return {false, "模型查询失败（HTTP " + std::to_string(response.status) + "）；请检查接口、凭据和网络", {}};
    try
    {
        const auto root = nlohmann::json::parse(response.body);
        const auto &rows = root.is_array() ? root : root.at("data");
        if (!rows.is_array())
            throw std::runtime_error("models");
        std::vector<std::string> models;
        for (const auto &row : rows)
        {
            const auto id = row.is_string() ? row.get<std::string>() : row.value("id", std::string());
            if (!id.empty() && id.size() <= 200 && id.find_first_of("\r\n\0", 0, 3) == std::string::npos)
                models.push_back(id);
            if (models.size() >= 5000)
                break;
        }
        std::sort(models.begin(), models.end());
        models.erase(std::unique(models.begin(), models.end()), models.end());
        if (models.empty())
            return {false, "服务没有返回可用模型名，仍可手动填写", {}};
        return {true, "已读取模型名；没有下载任何模型权重", std::move(models)};
    }
    catch (...)
    {
        return {false, "模型列表格式无效，仍可手动填写模型名", {}};
    }
}
Result Run(const Request &request)
{
    if (request.service == "translation.ai")
        return TestChat(request);
    if (request.service.rfind("translation.", 0) == 0)
        return TestTranslation(request);
    if (request.service == "voice.polish" || request.service == "ai.assistant")
        return TestChat(request);
    if (request.service == "voice.asr")
    {
        if (Value(request, "provider") != "doubao")
            return TestBatchAsr(request);
        const bool legacy = Value(request, "authMode") == "legacy";
        const std::string app_id = Value(request, "appId");
        const std::string token = Value(request, "token");
        if (!CloudTranslation::IsUsableSecret(token) || (legacy && !CloudTranslation::IsUsableSecret(app_id)))
            return {false, legacy ? "请先填写有效的 App ID 和 Access Token。" : "请先填写有效的 API Key。"};
        const std::string error = DoubaoAsrClient::TestCredentials(Value(request, "endpoint"), legacy, app_id, token,
                                                                   Value(request, "resourceId"));
        return error.empty() ? Result{true, "连接成功，豆包语音识别凭据有效。"} : Result{false, "测试失败：" + error};
    }
    return {false, "不支持的配置测试类型。"};
}
} // namespace ApiCredentialTest
