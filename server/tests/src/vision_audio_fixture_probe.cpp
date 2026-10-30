#include "config/ime_config_internal.h"
#include "settings/vision_subtitles.h"
#include "english/voice_studio.h"
#include "voice-input/voice_providers.h"
#include "engine/core/data_path.h"
#include "utils/common_utils.h"
#include <Windows.h>
#include <mmsystem.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#pragma comment(lib, "winmm.lib")
namespace
{
using Clock = std::chrono::steady_clock;
bool MatchesControlSentence(const std::string &text)
{
    std::wstring clean;
    for (const auto c : string_to_wstring(text))
        if (std::wstring_view(L" \t\r\n，。！？；：,.!?;:").find(c) == std::wstring_view::npos)
            clean += c;
    const auto numeral = clean.find(L"三点");
    if (numeral != std::wstring::npos)
        clean.replace(numeral, 2, L"3点");
    return clean == L"今天下午3点见面记得带上文件";
}
struct LocalProbeConfig
{
    VoiceInputConfig original;
    LocalProbeConfig()
    {
        wchar_t data[32768]{}, config[32768]{};
        if (!GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", data, 32768) ||
            !GetEnvironmentVariableW(L"METASEQUOIA_IME_CONFIG_DIR", config, 32768))
            throw std::runtime_error("audio probe requires explicit isolated data/config overrides");
        const auto production = std::filesystem::path(CommonUtils::get_local_appdata_path_w()) / L"metasequoiaime";
        if (std::filesystem::weakly_canonical(data) == std::filesystem::weakly_canonical(production) ||
            std::filesystem::weakly_canonical(config) == std::filesystem::weakly_canonical(production))
            throw std::runtime_error("audio probe will not modify or run against the production data directory");
        VoiceInputConfig local;
        local.asr_provider = "local";
        local.asr_endpoint = VoiceInput::DefaultAsrEndpoint("local");
        local.asr_model = VoiceInput::DefaultAsrModel("local");
        local.language = "zh-cn";
        local.polish_text = false;
        if (VoiceInput::ResolveAsrToken(local).empty())
            throw std::runtime_error("isolated probe data lacks the existing local ASR authentication file");
        // Process-memory only; never load, merge or persist the user's config/credential slots.
        std::unique_lock lock(ime_config_detail::g_voice_input_mutex);
        original = ime_config_detail::g_voice_input;
        ime_config_detail::g_voice_input = std::move(local);
    }
    ~LocalProbeConfig()
    {
        std::unique_lock lock(ime_config_detail::g_voice_input_mutex);
        ime_config_detail::g_voice_input = std::move(original);
    }
};
} // namespace
int RunVisionAudioFixtureProbe(const char *path, bool loopback)
{
    try
    {
        LocalProbeConfig config;
        const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment))
            throw std::runtime_error("could not prepare local audio decoder COM apartment");
        struct Apartment
        {
            ~Apartment()
            {
                CoUninitialize();
            }
        } apartmentScope;
        const std::filesystem::path wav(path);
        const auto start = Clock::now();
        const auto cancelled = [&] { return Clock::now() - start > std::chrono::seconds(loopback ? 25 : 60); };
        const auto audio = VoiceStudio::ReadAudioFile(wav, cancelled);
        const double duration = audio.size() / 16000.0;
        if (duration < .1 || duration > 10.1)
            throw std::runtime_error("own control audio must be at most 10 seconds");
        nlohmann::json lines = nlohmann::json::array();
        std::string text, srt;
        if (!loopback)
        {
            text = VoiceStudio::TranscribeAudio(audio, cancelled);
            lines.push_back(
                {{"id", "fixture-1"}, {"start", 0}, {"end", duration}, {"text", text}, {"translation", ""}});
            srt = Vision::SubtitleDocumentToSrt(lines);
        }
        else
        {
            if (duration < 9.9)
                throw std::runtime_error("loopback control fixture must contain exactly 10 seconds including silence");
            std::atomic<int64_t> deadline{std::numeric_limits<int64_t>::max()};
            const auto milliseconds = [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
            };
            struct StopOwnPlayback
            {
                ~StopOwnPlayback()
                {
                    PlaySoundW(nullptr, nullptr, 0);
                }
            } ownPlayback;
            Vision::RunSubtitles(
                true, false, "zh", "zh", cancelled, [&] { return milliseconds() >= deadline.load(); },
                [&](const nlohmann::json &result, const std::string &document) {
                    lines = result;
                    srt = document;
                },
                [&] {
                    // This callback runs only once capture has actually started. Parent/user opt-in is required by the
                    // CLI.
                    if (!PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
                        throw std::runtime_error("could not play the own ten-second control audio");
                    deadline = milliseconds() + 10000;
                });
            for (const auto &line : lines)
                text += line.at("text").get<std::string>();
        }
        const bool matches = MatchesControlSentence(text);
        auto parsed = srt.empty() ? nlohmann::json::array() : Vision::ParseSubtitleDocument(srt);
        nlohmann::json report = {{"ok", matches && !parsed.empty()},
                                 {"mode", loopback ? "controlled_system_playback" : "local_wav_import"},
                                 {"provider", "local"},
                                 {"source", wav.filename().u8string()},
                                 {"durationSeconds", duration},
                                 {"elapsedSeconds", std::chrono::duration<double>(Clock::now() - start).count()},
                                 {"recognized", text},
                                 {"matches_expected", matches},
                                 {"segments", lines},
                                 {"srt", srt},
                                 {"microphone_used", false},
                                 {"cloud_used", false}};
        std::cout << report.dump() << std::endl;
        return report.at("ok").get<bool>() ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
