#include "settings/vision_subtitles.h"
#include "settings/vision_protocol.h"
#include "english/voice_studio.h"
#include <msime/voice/audio_capture.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <condition_variable>
namespace Vision
{
struct SubtitleCaptureSession::Impl
{
    std::atomic<bool> quit{false}, done{false}, error{false};
    std::mutex mutex;
    std::condition_variable ready;
    bool initialized = false, active = false;
    std::thread control;
    Impl(std::function<bool()> start, std::function<void()> stop, std::function<bool()> finished,
         std::function<bool()> cancelled)
    {
        control = std::thread([this, start = std::move(start), stop = std::move(stop), finished = std::move(finished),
                               cancelled = std::move(cancelled)] {
            try
            {
                const bool success = start();
                {
                    std::lock_guard lock(mutex);
                    active = success;
                    initialized = true;
                }
                ready.notify_all();
                while (success && !quit && !cancelled() && !finished())
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            catch (...)
            {
                error = true;
                {
                    std::lock_guard lock(mutex);
                    initialized = true;
                }
                ready.notify_all();
            }
            try
            {
                stop();
            }
            catch (...)
            {
                error = true;
            }
            done = true;
        });
        std::unique_lock lock(mutex);
        ready.wait(lock, [this] { return initialized; });
    }
    ~Impl()
    {
        quit = true;
        if (control.joinable())
            control.join();
    }
};
SubtitleCaptureSession::SubtitleCaptureSession(std::function<bool()> start, std::function<void()> stop,
                                               std::function<bool()> finished, std::function<bool()> cancelled)
    : impl_(std::make_unique<Impl>(std::move(start), std::move(stop), std::move(finished), std::move(cancelled)))
{
}
SubtitleCaptureSession::~SubtitleCaptureSession() = default;
bool SubtitleCaptureSession::started() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->active;
}
bool SubtitleCaptureSession::ended() const
{
    return impl_->done;
}
bool SubtitleCaptureSession::failed() const
{
    return impl_->error;
}
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
nlohmann::json ParseSubtitleDocument(const std::string &input)
{
    if (input.empty() || input.size() > 1024 * 1024)
        throw std::runtime_error("字幕为空或超过 1 MB，请分段导入");
    std::string text = input;
    if (text.rfind("\xef\xbb\xbf", 0) == 0)
        text.erase(0, 3);
    std::vector<std::string> rows;
    std::istringstream stream(text);
    std::string row;
    while (std::getline(stream, row))
    {
        if (!row.empty() && row.back() == '\r')
            row.pop_back();
        rows.push_back(std::move(row));
    }
    const std::regex timing(
        R"(^\s*((?:\d{1,}:)?\d{2}:\d{2}[.,]\d{3})\s+-->\s+((?:\d{1,}:)?\d{2}:\d{2}[.,]\d{3})(.*)$)");
    const auto blank = [](const std::string &row) { return row.find_first_not_of(" \t") == std::string::npos; };
    const auto seconds = [](std::string value) {
        std::replace(value.begin(), value.end(), ',', '.');
        std::vector<std::string> parts;
        std::istringstream in(value);
        std::string part;
        while (std::getline(in, part, ':'))
            parts.push_back(part);
        if (parts.size() != 2 && parts.size() != 3)
            throw std::runtime_error("字幕时间格式无效");
        const double second = std::stod(parts.back());
        const double minute = std::stod(parts[parts.size() - 2]);
        const double hour = parts.size() == 3 ? std::stod(parts[0]) : 0;
        if (second >= 60 || minute >= 60 || hour > 9999)
            throw std::runtime_error("字幕时间范围无效");
        return hour * 3600 + minute * 60 + second;
    };
    auto lines = nlohmann::json::array();
    bool webvtt = !rows.empty() && rows[0].rfind("WEBVTT", 0) == 0;
    size_t initial = webvtt ? 1 : 0;
    if (webvtt)
        while (initial < rows.size() && !blank(rows[initial]) && rows[initial].find("-->") == std::string::npos)
            ++initial;
    for (size_t i = initial; i < rows.size();)
    {
        while (i < rows.size() && blank(rows[i]))
            ++i;
        if (i == rows.size())
            break;
        if (webvtt && (rows[i].rfind("NOTE", 0) == 0 || rows[i] == "STYLE" || rows[i] == "REGION"))
        {
            while (i < rows.size() && !blank(rows[i]))
                ++i;
            continue;
        }
        std::smatch match;
        if (rows[i].size() > 512)
            throw std::runtime_error("字幕时间轴或段编号过长");
        if (!std::regex_match(rows[i], match, timing))
        {
            ++i; // Optional cue number/name, or WEBVTT header metadata.
            if (i == rows.size() || rows[i].size() > 512 || !std::regex_match(rows[i], match, timing))
                throw std::runtime_error("字幕缺少有效时间轴（支持 SRT / WebVTT）");
        }
        const auto start = seconds(match[1].str()), end = seconds(match[2].str());
        if (end < start)
            throw std::runtime_error("字幕结束时间早于开始时间");
        const auto settings = match[3].str();
        ++i;
        std::string cue;
        while (i < rows.size() && !blank(rows[i]))
        {
            if (!cue.empty())
                cue += '\n';
            cue += rows[i++];
        }
        if (cue.empty() || cue.size() > 24000)
            throw std::runtime_error("字幕段为空或超过单段 24000 字节限制");
        if (lines.size() >= 2000)
            throw std::runtime_error("字幕超过 2000 段，请分段导入");
        lines.push_back({{"id", "file-" + std::to_string(lines.size() + 1)},
                         {"start", start},
                         {"end", end},
                         {"text", cue},
                         {"translation", ""},
                         {"settings", settings}});
    }
    if (lines.empty())
        throw std::runtime_error("未发现有效字幕段");
    return lines;
}
std::string SubtitleDocumentToSrt(const nlohmann::json &lines)
{
    std::string srt;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto &line = lines[i];
        srt += std::to_string(i + 1) + "\n" + SubtitleTime(line.at("start").get<double>()) + " --> " +
               SubtitleTime(line.at("end").get<double>()) + "\n" + line.at("text").get<std::string>();
        auto translation = line.value("translation", std::string());
        if (!translation.empty())
            srt += "\n" + translation;
        srt += "\n\n";
    }
    return srt;
}
nlohmann::json TranslateSubtitleSegments(const nlohmann::json &input,
                                         const std::function<std::string(const std::string &)> &translator,
                                         const std::function<bool()> &cancelled,
                                         const std::function<void(const nlohmann::json &, const std::string &)> &update)
{
    auto lines = input;
    std::unordered_map<std::string, std::string> cache;
    for (auto &line : lines)
    {
        if (cancelled())
            break;
        const auto text = line.at("text").get<std::string>();
        auto found = cache.find(text);
        if (found == cache.end())
            found = cache.emplace(text, translator(text)).first;
        if (cancelled())
            break;
        line["translation"] = found->second;
        if (update)
            update(lines, SubtitleDocumentToSrt(lines));
    }
    return lines;
}
void RunSubtitles(bool playback, bool translate, const std::string &source, const std::string &target,
                  const std::function<bool()> &cancelled, const std::function<bool()> &finished,
                  const std::function<void(const nlohmann::json &, const std::string &)> &update,
                  const std::function<void()> &captureStarted)
{
    using Capture = metasequoia::voice::AudioCapture;
    std::mutex mutex;
    std::deque<float> queued;
    std::atomic<bool> overflow{false};
    Capture capture;
    SubtitleCaptureSession session(
        [&] {
            return capture.start(
                [&](const float *p, size_t n) {
                    std::lock_guard lock(mutex);
                    if (queued.size() + n > 16000 * 30)
                    {
                        overflow = true;
                        return;
                    }
                    queued.insert(queued.end(), p, p + n);
                },
                playback ? Capture::Source::SystemPlayback : Capture::Source::Microphone);
        },
        [&] { capture.stop(); }, finished, [&] { return cancelled() || overflow || capture.callback_failed(); });
    if (!session.started())
        throw std::runtime_error(playback ? "无法采集系统声音，请检查当前播放设备"
                                          : "无法打开麦克风，请检查麦克风权限");
    if (captureStarted)
        captureStarted();
    nlohmann::json lines = nlohmann::json::array();
    uint64_t consumed = 0;
    while (!cancelled())
    {
        if (overflow)
            throw std::runtime_error("识别速度跟不上声音，已停止以避免字幕错位；请使用更快的本地 ASR 或缩短录制");
        if (capture.callback_failed() || session.failed())
            throw std::runtime_error("音频采集设备发生错误");
        const bool ended = session.ended();
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
        lines.push_back({{"id", "capture-" + std::to_string(lines.size() + 1)},
                         {"start", start},
                         {"end", consumed / 16000.0},
                         {"text", text},
                         {"translation", translated}});
        update(lines, SubtitleDocumentToSrt(lines));
        if (lines.size() >= 720)
            break;
    }
}
} // namespace Vision
