#include "english/speech_service.h"
#include "english/pcm_stream.h"
#include "english/voice_studio.h"
#include "tests/includes/test_framework.h"
#include <fstream>
#include <cmath>
#include "config/prompt_library.h"
#include "settings/dictionary_manager.h"

TEST_CASE(prompt_library_validates_import_and_resolves_selected_template)
{
    auto book = nlohmann::json{{"version", 1},
                               {"selected", "b"},
                               {"items",
                                {{{"id", "a"}, {"name", "原有规则"}, {"prompt", "保留原文"}},
                                 {{"id", "b"}, {"name", "会议纪要"}, {"prompt", "整理待办"}}}}};
    REQUIRE(PromptLibrary::Validate(book.dump()));
    REQUIRE(PromptLibrary::Active(book.dump()) == "整理待办");
    book["items"][1]["id"] = "a";
    REQUIRE(!PromptLibrary::Validate(book.dump()));
    REQUIRE(!PromptLibrary::Validate("{bad"));
}

TEST_CASE(candidate_reading_uses_phrase_tones_without_touching_output_text)
{
    REQUIRE(SettingsDictionary::CandidateReading("你好") == "nǐ hǎo");
    REQUIRE(SettingsDictionary::CandidateReading("银行") == "yín háng");
    REQUIRE(SettingsDictionary::CandidateReading("重庆") == "chóng qìng");
    REQUIRE(SettingsDictionary::CandidateReading("hello").empty());
}

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
TEST_CASE(speech_wav_roundtrip_validates_stereo_alignment_and_duration)
{
    Speech::PcmAudio audio{48000, 2, std::string(400, '\0')};
    const auto decoded = Speech::ParseWav(Speech::EncodeWav(audio));
    REQUIRE(decoded.rate == 48000);
    REQUIRE(decoded.channels == 2);
    REQUIRE(decoded.pcm == audio.pcm);
    auto damaged = Speech::EncodeWav(audio);
    damaged.pop_back();
    RejectSpeech([&] { Speech::ParseWav(damaged); });
    audio.pcm.push_back('x');
    RejectSpeech([&] { Speech::EncodeWav(audio); });
    audio = {24000, 1, std::string(24000 * 2 * 91, '\0')};
    RejectSpeech([&] { Speech::EncodeWav(audio); });
}
TEST_CASE(speech_model_parameters_are_scoped_to_the_requested_model)
{
    auto config = Speech::Defaults();
    config["provider"] = "local";
    config["local_model"] = "qwen-0.6b";
    config["local_options"] = R"({"qwen-0.6b":{"temperature":0.65,"do_sample":false},"moss-nano":{"audio_top_k":10}})";
    const auto request = nlohmann::json::parse(Speech::BuildRequest(config, "你好", "zh", 0).body);
    REQUIRE(request["options"]["temperature"] == 0.65);
    REQUIRE(request["options"]["do_sample"] == false);
    REQUIRE(!request["options"].contains("audio_top_k"));
}
TEST_CASE(voice_studio_decodes_stereo_wav_to_local_asr_format)
{
    const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto path = std::filesystem::temp_directory_path() /
                      (L"msime-voice-decode-" + std::to_wstring(GetCurrentProcessId()) + L".wav");
    Speech::PcmAudio audio{48000, 2, std::string(48000 * 2 * 2 / 5, '\0')};
    for (size_t pos = 0; pos < audio.pcm.size(); pos += 2)
    {
        const int16_t sample = 8192;
        memcpy(audio.pcm.data() + pos, &sample, 2);
    }
    const auto wav = Speech::EncodeWav(audio);
    {
        std::ofstream output(path, std::ios::binary);
        output.write(wav.data(), wav.size());
    }
    try
    {
        const auto samples = VoiceStudio::ReadAudioFile(path, [] { return false; });
        REQUIRE(samples.size() > 2800);
        REQUIRE(samples.size() < 3600);
        REQUIRE(std::all_of(samples.begin(), samples.end(), [](float value) { return std::isfinite(value); }));
        REQUIRE(
            std::any_of(samples.begin(), samples.end(), [](float value) { return value > 0.20f && value < 0.30f; }));
        RejectSpeech([&] { VoiceStudio::ReadAudioFile(path, [] { return true; }); });
    }
    catch (...)
    {
        std::filesystem::remove(path);
        if (SUCCEEDED(hr))
            CoUninitialize();
        throw;
    }
    std::filesystem::remove(path);
    if (SUCCEEDED(hr))
        CoUninitialize();
}
TEST_CASE(voice_studio_rejects_stale_audio_and_empty_generation_without_side_effects)
{
    for (const auto *action : {"copy", "save", "play", "route"})
    {
        const auto result =
            VoiceStudio::Handle({{"action", action}, {"artifactId", "stale"}, {"deviceId", 0}}, nullptr);
        REQUIRE(result["ok"] == false);
    }
    const auto empty = VoiceStudio::Handle({{"action", "generate"}, {"text", ""}}, nullptr);
    REQUIRE(empty["ok"] == false);
    VoiceStudio::Stop();
}
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
TEST_CASE(speech_dialect_choices_send_real_provider_language_controls)
{
    auto c = Speech::Defaults();
    c["provider"] = "azure";
    c["azure_key"] = "fixture";
    c["azure_voice_zh"] = "zh-CN-XiaoxiaoDialectsNeural|zh-CN-henan";
    auto r = Speech::BuildRequest(c, "你好", "zh", 1);
    REQUIRE(r.body.find("<lang xml:lang='zh-CN-henan'>你好</lang>") != std::string::npos);
    REQUIRE(r.body.find("name='zh-CN-XiaoxiaoDialectsNeural'") != std::string::npos);
    c["provider"] = "volcengine";
    c["volcengine_app_id"] = "fixture";
    c["volcengine_key"] = "fixture";
    c["volcengine_voice_zh"] = "zh_female_vv_uranus_bigtts|shanghai";
    const auto body = nlohmann::json::parse(Speech::BuildRequest(c, "你好", "zh", 1).body);
    REQUIRE(body.at("req_params").at("speaker") == "zh_female_vv_uranus_bigtts");
    REQUIRE(nlohmann::json::parse(body.at("req_params").at("additions").get<std::string>()).at("explicit_dialect") ==
            "shanghai");
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
