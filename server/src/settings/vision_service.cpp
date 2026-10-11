#include "settings/vision_service.h"
#include "settings/vision_capture.h"
#include "settings/vision_protocol.h"
#include "settings/vision_subtitles.h"
#include "engine/core/data_path.h"
#include "utils/common_utils.h"
#include <winrt/base.h>
#include <commdlg.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#pragma comment(lib, "comdlg32.lib")
namespace Vision
{
namespace
{
using nlohmann::json;
struct State
{
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> cancel{false}, finish{false};
    bool busy = false;
    unsigned revision = 0, imageRevision = 0;
    std::string message = "就绪", error, image, text, translation, format = "text";
    json raw = json::object();
    json subtitles = json::array();
    std::string srt;
    unsigned width = 0, height = 0, frames = 0;
    ~State()
    {
        cancel = true;
        if (worker.joinable())
            worker.join();
    }
};
State &Current()
{
    // Construct the overlay before State so State joins its worker first at shutdown.
    static const bool overlayReady = [] {
        HideTranslationOverlay();
        return true;
    }();
    static State s;
    return s;
}
std::string Read(const std::filesystem::path &path, size_t limit)
{
    std::error_code ec;
    auto n = std::filesystem::file_size(path, ec);
    if (ec || n > limit)
        throw std::runtime_error("文件过大或无法读取");
    std::ifstream f(path, std::ios::binary);
    std::string out(static_cast<size_t>(n), '\0');
    f.read(out.data(), out.size());
    if (!f)
        throw std::runtime_error("读取文件失败");
    return out;
}
void Write(const std::filesystem::path &p, const std::string &bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), bytes.size());
    if (!f)
        throw std::runtime_error("保存失败，请检查磁盘空间与权限");
}
void UpdateImage(const Image &im, bool clear = true)
{
    auto bytes = EncodePng(im);
    auto &s = Current();
    std::lock_guard lock(s.mutex);
    s.image = std::move(bytes);
    s.width = im.width;
    s.height = im.height;
    if (clear)
    {
        s.text.clear();
        s.translation.clear();
        s.raw = json::object();
    }
    ++s.imageRevision;
    ++s.revision;
}
bool Cancelled()
{
    return Current().cancel.load();
}
void Pause(unsigned ms)
{
    for (unsigned i = 0; i < ms && !Cancelled(); i += 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
void Status(std::string message)
{
    auto &s = Current();
    std::lock_guard lock(s.mutex);
    s.message = std::move(message);
    ++s.revision;
}
std::string Ocr(const Image &im, const std::string &task)
{
    const auto config = VisionConfig::Get();
    if (config.at("provider") == "windows" && (task == "table" || task == "formula"))
        throw std::runtime_error("Windows OCR 仅提取文字。结构化表格与公式请在下方选择文档模型或云服务。");
    auto result = config.at("provider") == "windows" ? WindowsOcr(im)
                                                     : RecognizeRemote(config, Base64(EncodePng(im)), task, Cancelled);
    if (Cancelled())
        return {};
    auto text = result.value("text", "");
    {
        auto &s = Current();
        std::lock_guard lock(s.mutex);
        s.text = text;
        s.format = result.value("format", "text");
        s.raw = std::move(result);
        ++s.revision;
    }
    return text;
}
std::wstring Quote(const std::filesystem::path &p)
{
    if (p.wstring().find(L'"') != std::wstring::npos)
        throw std::runtime_error("路径无效");
    return L"\"" + p.wstring() + L"\"";
}
Image Stitch(const std::vector<std::string> &frames, bool x, bool y)
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("无法创建截图任务");
    wchar_t id[40]{};
    StringFromGUID2(guid, id, 40);
    auto dir = metasequoia::data_directory() / "capture-cache" / id;
    std::filesystem::create_directories(dir);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{dir};
    json files = json::array();
    for (size_t i = 0; i < frames.size(); ++i)
    {
        auto p = dir / (std::to_string(i) + ".png");
        Write(p, frames[i]);
        files.push_back(p.u8string());
    }
    auto job = dir / "stitch.json";
    Write(job, json{{"frames", files}, {"horizontal", x}, {"vertical", y}}.dump());
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    auto exe = std::filesystem::path(module).parent_path() / "local-asr" / "MetasequoiaLocalASR.exe";
    if (!std::filesystem::is_regular_file(exe))
        throw std::runtime_error("缺少截图拼接组件，请安装完整版本");
    auto command = Quote(exe) + L" --image-job " + Quote(job);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        exe.parent_path().c_str(), &si, &pi))
        throw std::runtime_error("无法启动拼接组件");
    CloseHandle(pi.hThread);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT)
    {
        if (Cancelled() || std::chrono::steady_clock::now() > until)
        {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            throw std::runtime_error("截图拼接已取消或超时");
        }
    }
    CloseHandle(pi.hProcess);
    auto result = json::parse(Read(dir / "stitch.result.json", 65536));
    if (!result.value("ok", false))
        throw std::runtime_error(result.value("error", "拼接失败"));
    return DecodeImage(Read(dir / "stitch.png", 20 * 1024 * 1024));
}
void Start(std::function<void()> work, HWND restore = nullptr)
{
    auto &s = Current();
    {
        std::lock_guard lock(s.mutex);
        if (s.busy)
            throw std::runtime_error("请先停止当前任务");
    }
    if (s.worker.joinable())
        s.worker.join();
    s.cancel = false;
    s.finish = false;
    {
        std::lock_guard lock(s.mutex);
        s.busy = true;
        s.error.clear();
        s.message = "正在处理…";
        ++s.revision;
    }
    s.worker = std::thread([work = std::move(work), restore] {
        auto &s = Current();
        bool apartment = false;
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;
            work();
        }
        catch (const winrt::hresult_error &)
        {
            std::lock_guard lock(s.mutex);
            s.error = "Windows 图像或 OCR 组件不可用，请检查图片格式与系统语言包";
        }
        catch (const std::exception &e)
        {
            std::lock_guard lock(s.mutex);
            s.error = e.what();
        }
        HideTranslationOverlay();
        if (restore && !s.cancel && IsWindow(restore))
            ShowWindowAsync(restore, SW_SHOW);
        {
            std::lock_guard lock(s.mutex);
            s.busy = false;
            s.message = s.cancel ? "已停止" : s.error.empty() ? "处理完成" : s.error;
            ++s.revision;
        }
        if (apartment)
            winrt::uninit_apartment();
    });
}
} // namespace
void Stop()
{
    Current().cancel = true;
    HideTranslationOverlay();
}
json Handle(const json &request, HWND owner)
{
    auto &s = Current();
    json response = {{"type", "visionResponse"},
                     {"protocolVersion", 1},
                     {"requestId", request.value("requestId", "")},
                     {"ok", true},
                     {"message", ""}};
    try
    {
        const auto action = request.at("action").get<std::string>();
        if (action == "stop")
            Stop();
        else if (action == "finish")
            s.finish = true;
        else if (action == "subtitles")
        {
            const bool playback = request.value("playback", false), translate = request.value("translate", false);
            const auto source = request.value("source", "auto"), target = request.value("target", "en");
            Start([playback, translate, source, target] {
                {
                    auto &s = Current();
                    std::lock_guard lock(s.mutex);
                    s.subtitles = json::array();
                    s.srt.clear();
                    ++s.revision;
                }
                Status("字幕采集中：每约 5 秒识别一次，点击完成保留最后一段，停止立即取消");
                RunSubtitles(
                    playback, translate, source, target, Cancelled, [] { return Current().finish.load(); },
                    [](const json &lines, const std::string &srt) {
                        auto &s = Current();
                        std::lock_guard lock(s.mutex);
                        s.subtitles = lines;
                        s.srt = srt;
                        ++s.revision;
                    });
            });
        }
        else if (action == "saveImage")
        {
            std::string bytes;
            {
                std::lock_guard lock(s.mutex);
                bytes = s.image;
            }
            if (bytes.empty())
                throw std::runtime_error("请先截图或导入照片");
            wchar_t path[32768] = L"水杉截图.png";
            OPENFILENAMEW of{};
            of.lStructSize = sizeof(of);
            of.hwndOwner = owner;
            of.lpstrFilter = L"PNG 图像\0*.png\0\0";
            of.lpstrFile = path;
            of.nMaxFile = 32768;
            of.lpstrDefExt = L"png";
            of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetSaveFileNameW(&of))
                Write(path, bytes);
        }
        else if (action == "import")
        {
            auto bytes = Unbase64(request.at("image").get<std::string>());
            Start([bytes = std::move(bytes)] { UpdateImage(DecodeImage(bytes)); });
        }
        else if (action == "recognize")
        {
            std::string bytes;
            {
                std::lock_guard lock(s.mutex);
                bytes = s.image;
            }
            if (bytes.empty())
                throw std::runtime_error("请先截图或导入照片");
            auto task = request.value("task", VisionConfig::Get().at("task"));
            Start([bytes = std::move(bytes), task] { Ocr(DecodeImage(bytes), task); });
        }
        else if (action == "translate")
        {
            auto text = request.at("text").get<std::string>(), source = request.value("source", "auto"),
                 target = request.value("target", "en");
            Start([text, source, target] {
                auto out = Translate(text, source, target, Cancelled);
                if (Cancelled())
                    return;
                auto &s = Current();
                std::lock_guard lock(s.mutex);
                s.translation = out;
                ++s.revision;
            });
        }
        else if (action == "capture" || action == "longCapture" || action == "screen")
        {
            {
                std::lock_guard lock(s.mutex);
                if (s.busy)
                    throw std::runtime_error("请先停止当前任务");
            }
            const bool x = request.value("horizontal", false), y = request.value("vertical", true);
            if (action == "longCapture" && !x && !y)
                throw std::runtime_error("长截图至少启用一个方向");
            auto r = PickRectangle(owner);
            if (r.right <= r.left)
                return response;
            const auto source = request.value("source", "auto"), target = request.value("target", "en"),
                       task = request.value("task", "document");
            try
            {
                Start(
                    [r, action, x, y, source, target, task] {
                        Pause(200);
                        if (Cancelled())
                            return;
                        if (action == "capture")
                        {
                            UpdateImage(CaptureRectangle(r));
                            return;
                        }
                        if (action == "longCapture")
                        {
                            std::vector<std::string> frames;
                            size_t bytes = 0;
                            while (!Cancelled() && !Current().finish && frames.size() < 60)
                            {
                                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
                                    break;
                                auto png = EncodePng(CaptureRectangle(r));
                                bytes += png.size();
                                if (bytes > 120 * 1024 * 1024)
                                    throw std::runtime_error("截图缓存达到 120 MB，请分段截图");
                                frames.push_back(std::move(png));
                                {
                                    auto &s = Current();
                                    std::lock_guard lock(s.mutex);
                                    s.frames = static_cast<unsigned>(frames.size());
                                }
                                Status("长截图捕获中：缓慢滚动，Esc 完成");
                                Pause(900);
                            }
                            if (Cancelled() || frames.empty())
                                return;
                            Status("正在对齐与拼接截图…");
                            UpdateImage(Stitch(frames, x, y));
                            return;
                        }
                        std::string previous;
                        while (!Cancelled())
                        {
                            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
                                break;
                            HideTranslationOverlay();
                            Pause(100);
                            if (Cancelled())
                                break;
                            auto im = CaptureRectangle(r);
                            UpdateImage(im, false);
                            auto text = Ocr(im, task);
                            if (!text.empty())
                            {
                                if (text != previous)
                                {
                                    auto out = Translate(text, source, target, Cancelled);
                                    if (Cancelled())
                                        break;
                                    {
                                        auto &s = Current();
                                        std::lock_guard lock(s.mutex);
                                        s.translation = out;
                                        ++s.revision;
                                    }
                                    previous = text;
                                }
                                std::string out;
                                {
                                    auto &s = Current();
                                    std::lock_guard lock(s.mutex);
                                    out = s.translation;
                                }
                                ShowTranslationOverlay(r, out);
                            }
                            Status("屏幕翻译中 · Esc 停止");
                            Pause(static_cast<unsigned>(std::stoi(VisionConfig::Get().at("screen_interval_ms"))));
                        }
                    },
                    owner);
            }
            catch (...)
            {
                ShowWindow(owner, SW_SHOW);
                throw;
            }
        }
        else if (action != "status")
            throw std::runtime_error("不支持的截图操作");
        {
            std::lock_guard lock(s.mutex);
            response["busy"] = s.busy;
            response["revision"] = s.revision;
            response["imageRevision"] = s.imageRevision;
            response["message"] = s.message;
            response["error"] = s.error;
            if (request.value("revision", -1) != static_cast<int>(s.revision))
            {
                response["text"] = s.text;
                response["translation"] = s.translation;
                response["format"] = s.format;
                response["raw"] = s.raw;
                response["width"] = s.width;
                response["height"] = s.height;
                response["frames"] = s.frames;
                response["subtitles"] = s.subtitles;
                response["srt"] = s.srt;
            }
            if (!s.image.empty() && request.value("imageRevision", -1) != static_cast<int>(s.imageRevision))
                response["image"] = "data:image/png;base64," + Base64(s.image);
        }
    }
    catch (const std::exception &e)
    {
        response["ok"] = false;
        response["message"] = e.what();
    }
    return response;
}
} // namespace Vision
