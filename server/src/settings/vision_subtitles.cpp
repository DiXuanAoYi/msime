#include "settings/vision_subtitles.h"
#include "settings/vision_protocol.h"
#include "english/voice_studio.h"
#include <msime/voice/audio_capture.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
namespace Vision
{
std::string SubtitleTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        seconds = 0;
    auto ms = static_cast<unsigned long long>(seconds * 1000);
    char out[48]{};
    snprintf(out, sizeof(out), "%02llu:%02llu:%02llu,%03llu", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60,
             ms % 1000);
    return out;
}
void RunSubtitles(bool playback, bool translate, const std::string &source, const std::string &target,
                  const std::function<bool()> &cancelled, const std::function<bool()> &finished,
                  const std::function<void(const nlohmann::json &, const std::string &)> &update)
{
    using Capture = metasequoia::voice::AudioCapture;
    std::mutex mutex;
    std::deque<float> queued;
    std::atomic<bool> overflow{false};
    Capture capture;
    if (!capture.start(
            [&](const float *p, size_t n) {
                std::lock_guard lock(mutex);
                if (queued.size() + n > 16000 * 30)
                {
                    overflow = true;
                    return;
                }
                queued.insert(queued.end(), p, p + n);
            },
            playback ? Capture::Source::SystemPlayback : Capture::Source::Microphone))
        throw std::runtime_error(playback ? "无法采集系统声音，请检查当前播放设备"
                                          : "无法打开麦克风，请检查麦克风权限");
    nlohmann::json lines = nlohmann::json::array();
    uint64_t consumed = 0;
    bool ended = false;
    while (!cancelled())
    {
        if (overflow)
            throw std::runtime_error("识别速度跟不上声音，已停止以避免字幕错位；请使用更快的本地 ASR 或缩短录制");
        if (capture.callback_failed())
            throw std::runtime_error("音频采集设备发生错误");
        if (finished() && !ended)
        {
            capture.stop();
            ended = true;
        }
        std::vector<float> chunk;
        {
            std::lock_guard lock(mutex);
            if (queued.size() >= 16000 * 5 || ended)
            {
                const auto n = std::min(queued.size(), size_t(16000 * 5));
                chunk.assign(queued.begin(), queued.begin() + n);
                queued.erase(queued.begin(), queued.begin() + n);
            }
        }
        if (chunk.empty())
        {
            if (ended)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        const double start = consumed / 16000.0;
        consumed += chunk.size();
        double energy = 0;
        for (float f : chunk)
            energy += double(f) * f;
        if (std::sqrt(energy / chunk.size()) < .001)
            continue;
        auto text = VoiceStudio::TranscribeAudio(chunk, cancelled);
        if (cancelled())
            break;
        if (text.empty())
            continue;
        std::string translated;
        if (translate)
            translated = Translate(text, source, target, cancelled);
        if (cancelled())
            break;
        lines.push_back({{"start", start}, {"end", consumed / 16000.0}, {"text", text}, {"translation", translated}});
        std::string srt;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const auto &line = lines[i];
            srt += std::to_string(i + 1) + "\n" + SubtitleTime(line.at("start").get<double>()) + " --> " +
                   SubtitleTime(line.at("end").get<double>()) + "\n" + line.at("text").get<std::string>();
            if (!line.at("translation").get<std::string>().empty())
                srt += "\n" + line.at("translation").get<std::string>();
            srt += "\n\n";
        }
        update(lines, srt);
        if (lines.size() >= 720)
            break;
    }
    capture.stop();
}
} // namespace Vision
