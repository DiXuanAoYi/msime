#include "english/speech_service.h"
#include "english/pcm_stream.h"
#include "tests/includes/test_framework.h"

namespace
{
template <class F> void RejectSpeech(F f)
{
    bool failed = false;
    try
    {
        f();
    }
    catch (const std::exception &)
    {
        failed = true;
    }
    REQUIRE(failed);
}
} // namespace
TEST_CASE(speech_stream_plays_before_terminal_frame_and_accepts_fragmented_transport)
{
    std::string bytes = TtsStreamContract::magic;
    auto put = [&](uint32_t n) {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<char>(n >> (i * 8)));
    };
    put(24000);
    put(1);
    put(4);
    bytes.append("\1\0\2\0", 4);
    int calls = 0;
    Speech::PcmStream parser([&](unsigned rate, unsigned channels, std::string pcm) {
        REQUIRE(rate == 24000);
        REQUIRE(channels == 1);
        REQUIRE(pcm.size() == 4);
        ++calls;
    });
    for (const char &c : bytes)
        parser.Feed(std::string_view(&c, 1));
    REQUIRE(calls == 1);
    RejectSpeech([&] { parser.Finish(); });
    parser.Feed(std::string(4, '\0'));
    parser.Finish();
    RejectSpeech([&] { parser.Feed("extra"); });
}
TEST_CASE(speech_stream_rejects_invalid_format_and_unaligned_chunks)
{
    for (uint32_t rate : {1u, 192000u, 24000u})
    {
        std::string bytes = TtsStreamContract::magic;
        auto put = [&](uint32_t n) {
            for (int i = 0; i < 4; ++i)
                bytes.push_back(static_cast<char>(n >> (i * 8)));
        };
        put(rate);
        put(1);
        put(3);
        bytes += "abc";
        Speech::PcmStream parser([](unsigned, unsigned, std::string) { REQUIRE(false); });
        RejectSpeech([&] { parser.Feed(bytes); });
    }
}
TEST_CASE(speech_defaults_are_offline_and_cloud_payloads_are_provider_specific)
{
    auto c = Speech::Defaults();
    REQUIRE(c.at("provider") == "sapi");
    c["provider"] = "azure";
    c["azure_key"] = "fixture";
    auto r = Speech::BuildRequest(c, "a < b & c", "en", 1700000000);
    REQUIRE(r.url == "https://eastasia.tts.speech.microsoft.com/cognitiveservices/v1");
    REQUIRE(r.body.find("a &lt; b &amp; c") != std::string::npos);
    c["azure_region"] = "eastasia.invalid/";
    RejectSpeech([&] { Speech::BuildRequest(c, "test", "en", 1700000000); });
    c["provider"] = "tencent";
    c["tencent_secret_id"] = "fixtureId";
    c["tencent_secret_key"] = "fixtureKey";
    r = Speech::BuildRequest(c, "你好", "zh", 1700000000);
    REQUIRE(r.url == "https://tts.tencentcloudapi.com");
    REQUIRE(nlohmann::json::parse(r.body)["Codec"] == "wav");
    REQUIRE(r.headers.back().find("/tts/tc3_request") != std::string::npos);
    c["provider"] = "elevenlabs";
    c["elevenlabs_key"] = "fixture";
    c["elevenlabs_voice_en"] = "abc123";
    r = Speech::BuildRequest(c, "[laughs] Hello!", "en", 1700000000);
    REQUIRE(r.url == "https://api.elevenlabs.io/v1/text-to-speech/abc123?output_format=pcm_24000");
    REQUIRE(nlohmann::json::parse(r.body)["model_id"] == "eleven_flash_v2_5");
    c["provider"] = "volcengine";
    c["volcengine_app_id"] = "fixture";
    c["volcengine_key"] = "fixture";
    r = Speech::BuildRequest(c, "你好", "zh", 1700000000);
    REQUIRE(nlohmann::json::parse(r.body)["req_params"]["audio_params"]["format"] == "pcm");
}
TEST_CASE(speech_local_endpoint_and_headers_reject_remote_or_injected_destinations)
{
    auto c = Speech::Defaults();
    c["provider"] = "local";
    REQUIRE(Speech::BuildRequest(c, "test", "en", 1).url == c.at("local_endpoint"));
    for (const auto *url : {"https://example.com/v1/audio/speech", "http://127.0.0.1.evil:123/v1/audio/speech",
                            "http://user@localhost:123/v1/audio/speech"})
    {
        c["local_endpoint"] = url;
        RejectSpeech([&] { Speech::BuildRequest(c, "test", "en", 1); });
    }
    c["provider"] = "azure";
    c["azure_key"] = "key\r\nInjected: value";
    RejectSpeech([&] { Speech::BuildRequest(c, "test", "en", 1); });
    RejectSpeech([&] { Speech::BuildRequest(c, std::string(2001, 'a'), "en", 1); });
}
TEST_CASE(speech_pcm_wrapping_and_cloud_errors_do_not_play_garbage)
{
    auto wav = Speech::DecodeAudio("elevenlabs", std::string("\1\0\2\0", 4));
    REQUIRE(wav.size() == 48);
    REQUIRE(Speech::DecodeAudio("azure", wav) == wav);
    RejectSpeech([&] { Speech::DecodeAudio("azure", "<html>error</html>"); });
    RejectSpeech([&] { Speech::DecodeAudio("tencent", R"({"Response":{"Error":{"Message":"denied"}}})"); });
    REQUIRE(Speech::DecodeAudio("volcengine", "{\"code\":0,\"data\":\"AQACAA==\"}\n{\"code\":20000000}\n") == wav);
    RejectSpeech([&] { Speech::DecodeAudio("volcengine", "{\"code\":0,\"data\":\"AQACAA==\"}\n"); });
    RejectSpeech([&] { Speech::DecodeAudio("elevenlabs", "x"); });
}
