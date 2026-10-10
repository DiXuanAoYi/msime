#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "voice-input/voice_batch_protocol.h"
#include "tests/includes/test_framework.h"
#include <msime/voice/stt_service.h>
#include <nlohmann/json.hpp>

TEST_CASE(voice_batch_protocol_preserves_provider_fields_and_padding)
{
    VoiceInputConfig config;
    config.asr_provider = "openai";
    config.language = "zh-cn";
    config.asr_model = "fixture-asr";
    const std::vector<float> samples(160, 0.125f);
    const auto openai = VoiceInput::BuildBatchTranscription(samples, config);
    REQUIRE(openai.body.find("name=\"language\"\r\n\r\nzh") != std::string::npos);
    REQUIRE(openai.body.find("fixture-asr") != std::string::npos);
    config.asr_provider = "siliconflow";
    const auto siliconflow = VoiceInput::BuildBatchTranscription(samples, config);
    REQUIRE(siliconflow.body.find("name=\"language\"") == std::string::npos);
    const auto wave = siliconflow.body.find("RIFF");
    REQUIRE(wave != std::string::npos);
    // Tiny clips still contain a full second of padded PCM, as required by the existing provider adapter.
    const auto *bytes = reinterpret_cast<const unsigned char *>(siliconflow.body.data() + wave);
    const unsigned data_bytes = bytes[40] | (bytes[41] << 8) | (bytes[42] << 16) | (bytes[43] << 24);
    REQUIRE_EQ(data_bytes, 32000u);
    const std::vector<float> long_clip(61 * metasequoia::voice::sample_rate, 0.125f);
    REQUIRE(VoiceInput::BuildBatchTranscription(long_clip, config).body.size() > long_clip.size() * 2);
}

TEST_CASE(voice_batch_protocol_preserves_polish_prompt_and_thinking_settings)
{
    VoiceInputConfig config;
    config.polish_model = "fixture-polish";
    config.polish_prompt = "fixture prompt";
    config.polish_provider = "deepseek";
    auto body = nlohmann::json::parse(VoiceInput::BuildBatchPolish("原始文本", config));
    REQUIRE_EQ(body["model"].get<std::string>(), "fixture-polish");
    REQUIRE_EQ(body["messages"][0]["content"].get<std::string>(), "fixture prompt");
    REQUIRE_EQ(body["messages"][1]["content"].get<std::string>(), "<asr_text>\n原始文本\n</asr_text>");
    REQUIRE_EQ(body["thinking"]["type"].get<std::string>(), "disabled");
    config.polish_provider = "siliconflow";
    body = nlohmann::json::parse(VoiceInput::BuildBatchPolish("原始文本", config));
    REQUIRE_EQ(body["enable_thinking"].get<bool>(), false);
    REQUIRE(!body.contains("thinking"));
}

TEST_CASE(voice_cloud_asr_uses_provider_specific_wire_protocols)
{
    VoiceInputConfig c;
    c.asr_provider = "aliyun";
    c.asr_endpoint = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    c.asr_model = "qwen3-asr-flash";
    c.asr_tokens["aliyun"] = "fixture-key";
    auto req = VoiceInput::BuildAsrHttpRequest(std::vector<float>(160, 0), c, 1700000000);
    auto body = nlohmann::json::parse(req.body);
    REQUIRE_EQ(req.content_type, "application/json");
    REQUIRE(body["messages"][0]["content"][0]["input_audio"]["data"].get<std::string>().rfind(
                "data:audio/wav;base64,UklGR", 0) == 0);
    REQUIRE(req.body.find("fixture-key") == std::string::npos);
    c.asr_provider = "tencent";
    c.asr_endpoint = "https://asr.tencentcloudapi.com";
    c.asr_model = "16k_zh";
    c.tencent_secret_id = "fixture-id";
    c.asr_tokens["tencent"] = "fixture-secret";
    req = VoiceInput::BuildAsrHttpRequest(std::vector<float>(160, 0), c, 1700000000);
    body = nlohmann::json::parse(req.body);
    REQUIRE_EQ(body["DataLen"].get<int>(), 364);
    REQUIRE_EQ(body["SourceType"].get<int>(), 1);
    REQUIRE(req.headers[0].find("TC3-HMAC-SHA256") != std::string::npos);
    REQUIRE(req.headers[0].find("/2023-11-14/asr/tc3_request") != std::string::npos);
    REQUIRE(req.headers[0].find("fixture-secret") == std::string::npos);
    bool rejected = false;
    try
    {
        VoiceInput::BuildAsrHttpRequest(std::vector<float>(61 * 16000, 0), c, 1700000000);
    }
    catch (...)
    {
        rejected = true;
    }
    REQUIRE(rejected);
    c.asr_endpoint = "https://wrong.example";
    rejected = false;
    try
    {
        VoiceInput::BuildAsrHttpRequest(std::vector<float>(160, 0), c, 1700000000);
    }
    catch (...)
    {
        rejected = true;
    }
    REQUIRE(rejected);
}

TEST_CASE(voice_cloud_asr_parses_business_errors_even_on_http_success)
{
    REQUIRE_EQ(VoiceInput::ParseAsrHttpResponse(R"({"Response":{"Result":"测试"}})", "tencent"), "测试");
    REQUIRE_EQ(VoiceInput::ParseAsrHttpResponse(R"({"choices":[{"message":{"content":"hello"}}]})", "aliyun"), "hello");
    bool rejected = false;
    try
    {
        VoiceInput::ParseAsrHttpResponse(R"({"Response":{"Error":{"Code":"AuthFailure"}}})", "tencent");
    }
    catch (...)
    {
        rejected = true;
    }
    REQUIRE(rejected);
}
