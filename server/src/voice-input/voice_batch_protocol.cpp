#include "voice_batch_protocol.h"
#include "voice_providers.h"
#include <msime/voice/wav_writer.h>
#include <nlohmann/json.hpp>
#include "cloud/tencent_tmt.h"
#include <Windows.h>
#include <wincrypt.h>
#include <stdexcept>
#pragma comment(lib, "crypt32.lib")

namespace VoiceInput
{
metasequoia::voice::MultipartRequest BuildBatchTranscription(const std::vector<float> &samples,
                                                             const VoiceInputConfig &config)
{
    const bool siliconflow = NormalizeProviderId(config.asr_provider) == "siliconflow";
    std::vector<float> padded;
    if (siliconflow)
    {
        constexpr std::size_t pad_frames = metasequoia::voice::sample_rate / 5;
        padded.reserve(samples.size() + pad_frames * 2);
        padded.insert(padded.end(), pad_frames, 0.0f);
        padded.insert(padded.end(), samples.begin(), samples.end());
        padded.insert(padded.end(), pad_frames, 0.0f);
        if (padded.size() < metasequoia::voice::sample_rate)
            padded.resize(metasequoia::voice::sample_rate, 0.0f);
    }
    constexpr std::size_t upload_sample_limit = (metasequoia::voice::maximum_encoded_audio_bytes - 44) / 2;
    const auto wav = metasequoia::voice::WavWriter::create_wav(siliconflow ? padded : samples,
                                                               metasequoia::voice::sample_rate, upload_sample_limit);
    // SiliconFlow rejects the optional language field. Keep the existing Windows mapping elsewhere.
    const std::string language = !siliconflow && config.language == "zh-cn" ? "zh"
                                 : !siliconflow && config.language == "en"  ? "en"
                                                                            : "";
    return metasequoia::voice::make_transcription_request(
        std::string_view(reinterpret_cast<const char *>(wav.data()), wav.size()), ResolveAsrModel(config), language);
}

namespace
{
std::string Base64(const std::vector<unsigned char> &bytes)
{
    DWORD length = 0;
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &length))
        throw std::runtime_error("音频编码失败。");
    std::string value(length, '\0');
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              value.data(), &length))
        throw std::runtime_error("音频编码失败。");
    value.resize(length);
    return value;
}
} // namespace
AsrHttpRequest BuildAsrHttpRequest(const std::vector<float> &samples, const VoiceInputConfig &config, int64_t timestamp)
{
    using nlohmann::json;
    const auto provider = NormalizeProviderId(config.asr_provider);
    const auto token = ResolveAsrToken(config);
    if (provider == "local" && config.asr_endpoint != DefaultAsrEndpoint("local"))
        throw std::runtime_error("本地模型只连接本机 127.0.0.1:18766，不会将音频发送到外部地址。");
    if (provider != "local" && config.asr_endpoint.rfind("https://", 0) != 0)
        throw std::runtime_error("云端语音接口必须使用 HTTPS。");
    if (provider == "tencent" && config.asr_endpoint != DefaultAsrEndpoint("tencent"))
        throw std::runtime_error("腾讯云 ASR 使用固定官方接口 https://asr.tencentcloudapi.com。");
    if (provider != "aliyun" && provider != "tencent")
    {
        auto p = BuildBatchTranscription(samples, config);
        return {config.asr_endpoint, p.content_type, p.body,
                std::vector<std::string>{"Authorization: Bearer " + token}};
    }
    if (provider == "tencent" && (samples.empty() || samples.size() > 60 * metasequoia::voice::sample_rate))
        throw std::runtime_error("腾讯云一句话识别每次最多 60 秒，请分段录音。");
    auto wav = metasequoia::voice::WavWriter::create_wav(samples, metasequoia::voice::sample_rate,
                                                         (metasequoia::voice::maximum_encoded_audio_bytes - 44) / 2);
    const auto audio = Base64(wav);
    if (provider == "aliyun")
    {
        json body = {
            {"model", ResolveAsrModel(config)},
            {"stream", false},
            {"messages",
             json::array(
                 {{{"role", "user"},
                   {"content", json::array({{{"type", "input_audio"},
                                             {"input_audio", {{"data", "data:audio/wav;base64," + audio}}}}})}}})},
            {"asr_options", {{"enable_itn", true}}}};
        return {config.asr_endpoint, "application/json", body.dump(), {"Authorization: Bearer " + token}};
    }
    if (IsPlaceholderToken(config.tencent_secret_id) || token.empty())
        throw std::runtime_error("请填写腾讯云 SecretId 与 SecretKey（语音识别服务）。");
    json body = {{"ProjectId", 0},
                 {"SubServiceType", 2},
                 {"EngSerViceType", ResolveAsrModel(config)},
                 {"SourceType", 1},
                 {"VoiceFormat", "wav"},
                 {"Data", audio},
                 {"DataLen", wav.size()},
                 {"UsrAudioKey", "msime-" + std::to_string(timestamp)}};
    const auto payload = body.dump();
    auto auth = TencentTmt::AsrAuthorization({config.tencent_secret_id, token, "ap-shanghai"}, payload,
                                             static_cast<time_t>(timestamp));
    if (auth.empty())
        throw std::runtime_error("腾讯云请求签名失败。");
    return {config.asr_endpoint,
            "application/json; charset=utf-8",
            payload,
            {"Authorization: " + auth, "X-TC-Action: SentenceRecognition", "X-TC-Version: 2019-06-14",
             "X-TC-Timestamp: " + std::to_string(timestamp), "X-TC-Region: ap-shanghai"}};
}
std::string ParseAsrHttpResponse(const std::string &body, const std::string &provider)
{
    auto root = nlohmann::json::parse(body);
    if (provider == "tencent")
    {
        auto response = root.at("Response");
        if (response.contains("Error"))
            throw std::runtime_error("腾讯云：" + response["Error"].value("Code", "UnknownError"));
        return response.at("Result").get<std::string>();
    }
    if (provider == "aliyun")
        return root.at("choices").at(0).at("message").at("content").get<std::string>();
    return metasequoia::voice::parse_transcription(body);
}

std::string BuildBatchPolish(const std::string &text, const VoiceInputConfig &config)
{
    auto body = nlohmann::json::parse(metasequoia::voice::make_polish_request(
        ResolvePolishModel(config), ResolvePolishSystemPrompt(config), WrapAsrUserMessage(text)));
    if (config.polish_provider == "siliconflow")
        body["enable_thinking"] = false;
    else if (config.polish_provider == "deepseek")
        body["thinking"] = {{"type", "disabled"}};
    return body.dump();
}
} // namespace VoiceInput
