#pragma once
#include <functional>
#include <string>
#include <memory>
#include <nlohmann/json.hpp>
namespace Vision
{
// The host control thread serializes device start/stop independently of slow recognition.
class SubtitleCaptureSession
{
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    SubtitleCaptureSession(std::function<bool()> start, std::function<void()> stop, std::function<bool()> finished,
                           std::function<bool()> cancelled);
    ~SubtitleCaptureSession();
    bool started() const;
    bool ended() const;
    bool failed() const;
};
std::string SubtitleTime(double seconds);
nlohmann::json ParseSubtitleDocument(const std::string &text);
std::string SubtitleDocumentToSrt(const nlohmann::json &lines);
nlohmann::json TranslateSubtitleSegments(
    const nlohmann::json &lines, const std::function<std::string(const std::string &)> &translator,
    const std::function<bool()> &cancelled,
    const std::function<void(const nlohmann::json &, const std::string &)> &update = {});
void RunSubtitles(bool playback, bool translate, const std::string &source, const std::string &target,
                  const std::function<bool()> &cancelled, const std::function<bool()> &finished,
                  const std::function<void(const nlohmann::json &, const std::string &)> &update,
                  const std::function<void()> &captureStarted = {});
} // namespace Vision
