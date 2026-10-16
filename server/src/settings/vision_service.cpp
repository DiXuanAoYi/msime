#include "settings/vision_service.h"
#include "settings/vision_capture.h"
#include "settings/vision_protocol.h"
#include "settings/vision_subtitles.h"
#include "engine/core/data_path.h"
#include "quick/quick_actions.h"
#include "utils/protected_file.h"
#include "utils/common_utils.h"
#include <winrt/base.h>
#include <commdlg.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    std::string message = "就绪", error, warning, image, text, translation, format = "text";
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
        s.warning.clear();
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
void ResetSubtitles()
{
    auto &s = Current();
    std::lock_guard lock(s.mutex);
    if (s.busy)
        throw std::runtime_error("请先停止当前任务");
    s.subtitles = json::array();
    s.srt.clear();
    ++s.revision;
}
void UpdateSubtitles(const json &lines, const std::string &srt)
{
    auto &s = Current();
    std::lock_guard lock(s.mutex);
    s.subtitles = lines;
    s.srt = srt;
    ++s.revision;
}
std::string Utf8Lower(std::string text)
{
    for (auto &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
bool EndsSentence(const std::string &line)
{
    static const char *const marks[] = {"。", "！", "？", "：", "；", "…", "”", "）", ".", "!", "?", ":", ";"};
    for (const auto *mark : marks)
    {
        const std::string m = mark;
        if (line.size() >= m.size() && line.compare(line.size() - m.size(), m.size(), m) == 0)
            return true;
    }
    return false;
}
bool StartsListItem(const std::string &line)
{
    if (line.empty())
        return false;
    const unsigned char c = static_cast<unsigned char>(line[0]);
    if (c == '-' || c == '*' || c == '#' || c == '>')
        return true;
    size_t i = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])))
        ++i;
    return i > 0 && i < line.size() && (line[i] == '.' || line[i] == ')' || line[i] == ',');
}
// line_mode: smart joins hard-wrapped lines of a paragraph, one joins everything, keep leaves the lines alone.
std::string ApplyLineMode(const std::string &text, const std::string &mode)
{
    if (mode == "keep")
        return text;
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size())
    {
        auto end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        auto line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto &line = lines[i];
        if (line.empty())
        {
            if (mode == "smart" && !out.empty() && out.back() != '\n')
                out += '\n';
            continue;
        }
        if (!out.empty() && out.back() != '\n')
        {
            const bool join = mode == "one" || (!EndsSentence(lines[i - 1]) && !StartsListItem(line));
            out += join ? (static_cast<unsigned char>(out.back()) >= 0x80 || static_cast<unsigned char>(line[0]) >= 0x80
                               ? ""
                               : " ")
                        : "\n";
        }
        out += line;
    }
    return out;
}
json PostProcess(json result, const VisionConfig::Config &config, const std::string &task)
{
    if (task != "document" || !result.contains("text") || !result["text"].is_string())
        return result;
    auto text = result["text"].get<std::string>();
    if (config.at("cjk_space") != "true")
        text = winrt::to_string(NormalizeChineseOcrSpacing(winrt::to_hstring(text).c_str()));
    result["text"] = ApplyLineMode(text, config.at("line_mode"));
    return result;
}
json RecognizeWith(const Image &im, const std::string &task, const std::function<bool()> &cancelled)
{
    auto config = VisionConfig::Get();
    const auto provider = config.at("provider");
    json result;
    if (provider == "smart")
    {
        // Plain text is what Windows OCR does well and costs nothing; tables and formulas need a model.
        if (task == "table" || task == "formula")
        {
            auto local = config;
            local["provider"] = "local";
            try
            {
                result = RecognizeRemote(local, Base64(EncodePng(im)), task, cancelled);
            }
            catch (const std::exception &e)
            {
                result = WindowsOcr(im, cancelled);
                result["warning"] = std::string("本地模型不可用（") + e.what() + "），已改用 Windows OCR，仅提取文字。";
            }
        }
        else
        {
            result = WindowsOcr(im, cancelled);
            if (task == "code")
                ReconstructCodeIndentation(result);
        }
    }
    else if (provider == "windows")
    {
        if (task == "table" || task == "formula")
            throw std::runtime_error("Windows OCR 仅提取文字。结构化表格与公式请在下方选择文档模型或云服务。");
        result = WindowsOcr(im, cancelled);
        if (task == "code")
            ReconstructCodeIndentation(result);
    }
    else
        result = RecognizeRemote(config, Base64(EncodePng(im)), task, cancelled);
    return PostProcess(std::move(result), config, task);
}
json Ocr(const Image &im, const std::string &task)
{
    auto result = RecognizeWith(im, task, Cancelled);
    if (Cancelled())
        return {};
    auto text = result.value("text", "");
    {
        auto &s = Current();
        std::lock_guard lock(s.mutex);
        s.text = text;
        s.format = result.value("format", "text");
        s.raw = result;
        s.warning = result.value("warning", std::string());
        if (result.value("truncated", false) && s.warning.empty())
            s.warning = "识别结果达到输出长度上限，内容不完整；请提高输出长度或缩小识别区域。";
        ++s.revision;
    }
    return result;
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
    return DecodeImage(Read(dir / "stitch.png", MaxEncodedImageBytes));
}
void LiveLoop(RECT r, const std::string &source, const std::string &target, const std::string &task)
{
    const auto refresh = VisionConfig::Get().at("live_refresh");
    const unsigned interval = refresh == "2s" ? 2000u : refresh == "5s" ? 5000u : 700u;
    const std::string live = refresh == "change" ? "change" : "always";
    std::vector<unsigned char> lastPixels;
    std::vector<OverlayBlock> lastBlocks;
    std::unordered_map<std::string, std::string> translations;
    std::string cachedProvider;
    std::string previousText;
    bool firstFrame = true;
    while (!Cancelled())
    {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            break;
        HideTranslationOverlay();
        Pause(100);
        if (Cancelled())
            break;
        auto im = CaptureRectangle(r);
        if (live == "change" && !firstFrame && im.pixels == lastPixels)
        {
            ShowTranslationOverlay(r, lastBlocks);
            Pause(interval);
            continue;
        }
        lastPixels = im.pixels;
        UpdateImage(im, false);
        auto result = Ocr(im, task);
        if (Cancelled())
            break;
        const auto recognized = result.value("text", std::string());
        if (firstFrame || recognized != previousText)
        {
            auto &s = Current();
            std::lock_guard lock(s.mutex);
            s.translation.clear();
            ++s.revision;
            firstFrame = false;
            previousText = recognized;
        }
        auto blocks = OverlayBlocksForImage(result, im.width, im.height);
        const auto supplied =
            result.contains("blocks") && result.at("blocks").is_array() ? result.at("blocks").size() : size_t(0);
        if (supplied > blocks.size())
        {
            auto &s = Current();
            std::lock_guard lock(s.mutex);
            if (!s.warning.empty())
                s.warning += '\n';
            s.warning += "部分文字缺少有效坐标或超过 256 "
                         "块；不会伪造定位，请缩小框选区域，完整原文可在编辑区查看。";
        }
        const auto provider = VisionConfig::Get().at("translation_provider");
        if (provider != cachedProvider || translations.size() > 1024)
        {
            translations.clear();
            cachedProvider = provider;
        }
        std::vector<std::string> missing;
        std::unordered_set<std::string> missingSet;
        for (const auto &block : blocks)
            if (!translations.count(block.source) && missingSet.insert(block.source).second)
                missing.push_back(block.source);
        const auto translated = TranslateBlocks(missing, source, target, Cancelled);
        if (Cancelled())
            break;
        for (size_t i = 0; i < missing.size(); ++i)
            translations.emplace(missing[i], translated[i]);
        size_t unavailable = 0, regions = 0;
        std::string combined;
        for (auto &block : blocks)
        {
            if (Cancelled())
                break;
            if (block.region)
                ++regions;
            block.text = translations.at(block.source);
            if (block.text.empty())
                ++unavailable;
            else
            {
                if (!combined.empty())
                    combined += '\n';
                combined += block.text;
            }
        }
        if (Cancelled())
            break;
        {
            auto &s = Current();
            std::lock_guard lock(s.mutex);
            s.translation = combined;
            ++s.revision;
        }
        lastBlocks = blocks;
        ShowTranslationOverlay(r, blocks);
        Status("屏幕翻译中：" + std::to_string(blocks.size()) + " 个定位块" +
               (regions ? "；文档模型仅提供区域定位" : "") +
               (unavailable ? "；离线词典未收录 " + std::to_string(unavailable) + " 块，请选择整段翻译服务" : "") +
               " · Esc 停止");
        Pause(interval);
    }
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
        s.warning.clear();
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
namespace
{
constexpr wchar_t kHistoryLock[] = L"Local\\MetasequoiaImeVisionHistory";
std::filesystem::path HistoryPath()
{
    return std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"vision-history.dat";
}
json ReadHistory()
{
    try
    {
        if (auto stored = ProtectedFile::Read(HistoryPath(), L"MetasequoiaIME vision history"))
            if (stored->is_array())
                return *stored;
    }
    catch (const std::exception &)
    {
    }
    return json::array();
}
int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace
std::string MergeOcrLines(const std::string &text, const std::string &mode)
{
    return ApplyLineMode(text, mode);
}
json RecognizeImage(const Image &image, const std::string &task, const std::function<bool()> &cancelled)
{
    return RecognizeWith(image, task, cancelled);
}
bool Busy()
{
    std::lock_guard lock(Current().mutex);
    return Current().busy;
}
bool StartLive(RECT rectangle, const std::string &source, const std::string &target, const std::string &task)
{
    {
        std::lock_guard lock(Current().mutex);
        if (Current().busy)
            return false;
    }
    try
    {
        Start([rectangle, source, target, task] {
            Pause(200);
            if (!Cancelled())
                LiveLoop(rectangle, source, target, task);
        });
    }
    catch (const std::exception &)
    {
        return false;
    }
    return true;
}
void AddHistory(const std::string &kind, const std::string &text, const std::string &translation)
{
    const auto config = VisionConfig::Get();
    if (config.at("history_enabled") != "true" || (text.empty() && translation.empty()))
        return;
    try
    {
        ProtectedFile::NamedLock lock(kHistoryLock);
        if (!lock)
            return;
        auto history = ReadHistory();
        const auto now = NowMs();
        history.push_back({{"id", std::to_string(now) + "-" + std::to_string(history.size())},
                           {"at", now},
                           {"kind", kind},
                           {"text", text.substr(0, 20000)},
                           {"translation", translation.substr(0, 20000)}});
        const int days = std::max(1, std::atoi(config.at("history_days").c_str()));
        const auto oldest = now - static_cast<int64_t>(days) * 86400000;
        json kept = json::array();
        for (const auto &row : history)
            if (row.value("at", int64_t(0)) >= oldest)
                kept.push_back(row);
        while (kept.size() > 200)
            kept.erase(kept.begin());
        ProtectedFile::Write(HistoryPath(), kept, L"MetasequoiaIME vision history");
    }
    catch (const std::exception &)
    {
    }
}
json HandleHistory(const json &request)
{
    const auto action = request.at("action").get<std::string>();
    ProtectedFile::NamedLock lock(kHistoryLock);
    if (!lock)
        throw std::runtime_error("历史记录正忙，请重试");
    auto history = ReadHistory();
    if (action == "historyClear")
        history = json::array();
    else if (action == "historyDelete")
    {
        const auto id = request.value("id", "");
        json kept = json::array();
        for (const auto &row : history)
            if (row.value("id", "") != id)
                kept.push_back(row);
        history = kept;
    }
    if (action != "history")
        ProtectedFile::Write(HistoryPath(), history, L"MetasequoiaIME vision history");
    json out = json::array();
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        out.push_back(*it);
    return out;
}
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
            ResetSubtitles();
            Start([playback, translate, source, target] {
                Status("字幕采集中：每约 5 秒识别一次，点击完成保留最后一段，停止立即取消");
                RunSubtitles(
                    playback, translate, source, target, Cancelled, [] { return Current().finish.load(); },
                    UpdateSubtitles);
            });
        }
        else if (action == "importSubtitles" || action == "translateSubtitles")
        {
            auto lines = ParseSubtitleDocument(request.at("text").get<std::string>());
            const auto source = request.value("source", "auto"), target = request.value("target", "en");
            ResetSubtitles();
            Start([action, lines = std::move(lines), source, target] {
                UpdateSubtitles(lines, SubtitleDocumentToSrt(lines));
                if (action == "translateSubtitles")
                {
                    Status("正在逐段翻译字幕，保留时间轴；可以停止并保留已完成结果");
                    size_t cursor = 0;
                    std::unordered_map<std::string, std::string> prefetched;
                    TranslateSubtitleSegments(
                        lines,
                        [&](const std::string &text) {
                            auto found = prefetched.find(text);
                            if (found == prefetched.end())
                            {
                                std::vector<std::string> batch;
                                std::unordered_set<std::string> seen;
                                while (cursor < lines.size() && batch.size() < 32)
                                {
                                    const auto value = lines[cursor++].at("text").get<std::string>();
                                    if (!prefetched.count(value) && seen.insert(value).second)
                                        batch.push_back(value);
                                }
                                const auto translated = TranslateBlocks(batch, source, target, Cancelled);
                                if (Cancelled())
                                    return std::string();
                                for (size_t i = 0; i < batch.size(); ++i)
                                    prefetched.emplace(batch[i], translated[i]);
                                found = prefetched.find(text);
                            }
                            if (found == prefetched.end() || found->second.empty())
                                throw std::runtime_error("离线词典未收录此字幕段，请选择整段翻译服务");
                            return found->second;
                        },
                        Cancelled, UpdateSubtitles);
                }
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
            wchar_t path[32768] = L"知语截图.png";
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
            Start([bytes = std::move(bytes), task] {
                auto result = Ocr(DecodeImage(bytes), task);
                if (!Cancelled())
                    AddHistory("ocr", result.value("text", std::string()), "");
            });
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
                            const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(3);
                            while (!Cancelled() && !Current().finish && frames.size() < 60 &&
                                   std::chrono::steady_clock::now() < until)
                            {
                                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
                                    break;
                                auto png = EncodePng(CaptureRectangle(r));
                                if (frames.empty() || png != frames.back())
                                {
                                    bytes += png.size();
                                    if (bytes > 120 * 1024 * 1024)
                                        throw std::runtime_error("截图缓存达到 120 MB，请分段截图");
                                    if (frames.empty())
                                        UpdateImage(DecodeImage(png));
                                    frames.push_back(std::move(png));
                                }
                                {
                                    auto &s = Current();
                                    std::lock_guard lock(s.mutex);
                                    s.frames = static_cast<unsigned>(frames.size());
                                }
                                Status("长截图捕获中：" + std::to_string(frames.size()) +
                                       " 个不同画面；缓慢滚动，Esc 完成（最多 3 分钟）");
                                Pause(900);
                            }
                            if (Cancelled() || frames.empty())
                                return;
                            Status("正在对齐与拼接截图…");
                            UpdateImage(Stitch(frames, x, y));
                            return;
                        }
                        LiveLoop(r, source, target, task);
                    },
                    owner);
            }
            catch (...)
            {
                ShowWindow(owner, SW_SHOW);
                throw;
            }
        }
        else if (action == "history" || action == "historyDelete" || action == "historyClear")
            response["history"] = HandleHistory(request);
        else if (action == "runQuick")
        {
            const auto id = request.value("id", "");
            if (!Quick::Actions::Run(id))
                throw std::runtime_error("未知的快捷操作");
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
            response["warning"] = s.warning;
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
