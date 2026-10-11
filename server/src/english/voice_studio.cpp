#include "voice_studio.h"
#include "speech_service.h"
#include "pcm_player.h"
#include "settings/local_tts_models.h"
#include "voice-input/voice_batch_protocol.h"
#include "voice-input/voice_providers.h"
#include "config/ime_config.h"
#include "engine/core/data_path.h"
#include "utils/common_utils.h"
#include <msime/voice/audio_capture.h>
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shell32.lib")

namespace VoiceStudio
{
namespace
{
using Microsoft::WRL::ComPtr;
using nlohmann::json;
constexpr size_t maxFrames = 16000 * 90;
struct Job
{
    std::string action, text, language;
    Speech::Config config;
    std::filesystem::path path;
    Speech::PcmAudio audio;
    unsigned device = WAVE_MAPPER;
    uint64_t generation = 0;
};
json Devices()
{
    json devices = json::array();
    for (unsigned i = 0; i < waveOutGetNumDevs(); ++i)
    {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
        {
            const auto name = wstring_to_string(caps.szPname);
            auto lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool bridge = lower.find("cable") != std::string::npos ||
                                lower.find("voicemeeter") != std::string::npos ||
                                lower.find("virtual") != std::string::npos || name.find("虚拟") != std::string::npos;
            devices.push_back({{"id", i}, {"name", name}, {"virtual", bridge}});
        }
    }
    return devices;
}
std::filesystem::path ChooseFile(HWND owner, bool save)
{
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("无法打开音频文件选择窗口");
    const COMDLG_FILTERSPEC filters[] = {{L"音频文件", save ? L"*.wav" : L"*.wav;*.mp3;*.m4a;*.wma;*.aac;*.flac"}};
    dialog->SetFileTypes(1, filters);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    if (save)
    {
        dialog->SetDefaultExtension(L"wav");
        dialog->SetFileName(L"水杉语音.wav");
    }
    if (dialog->Show(owner) != S_OK)
        return {};
    ComPtr<IShellItem> item;
    PWSTR name = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
        throw std::runtime_error("无法取得所选音频路径");
    std::filesystem::path path(name);
    CoTaskMemFree(name);
    return path;
}
std::vector<float> ReadAudio(const std::filesystem::path &path, const std::function<bool()> &cancelled)
{
    if (std::filesystem::file_size(path) > 32 * 1024 * 1024)
        throw std::runtime_error("导入音频限 32 MB / 90 秒");
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)))
        throw std::runtime_error("无法初始化本机音频解码器");
    struct Shutdown
    {
        ~Shutdown()
        {
            MFShutdown();
        }
    } shutdown;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> format;
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1)))
        throw std::runtime_error("无法准备音频解码参数");
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader)) ||
        FAILED(MFCreateMediaType(&format)))
        throw std::runtime_error("无法读取此音频，请转成 WAV 后导入");
    format->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    format->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    format->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    format->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 16000);
    format->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    format->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    format->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 64000);
    if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
                                           format.Get())))
        throw std::runtime_error("本机无法解码此音频格式，请使用 PCM WAV");
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    std::vector<float> output;
    for (;;)
    {
        if (cancelled())
            throw std::runtime_error("已取消音频导入");
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags,
                                      nullptr, &sample)))
            throw std::runtime_error("音频解码失败");
        if (sample)
        {
            ComPtr<IMFMediaBuffer> buffer;
            BYTE *bytes = nullptr;
            DWORD length = 0;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&bytes, nullptr, &length)))
                throw std::runtime_error("无法读取音频样本");
            if (length % 4 || output.size() + length / 4 > maxFrames)
            {
                buffer->Unlock();
                throw std::runtime_error("导入音频超过 90 秒或样本无效");
            }
            const auto old = output.size();
            output.resize(old + length / 4);
            memcpy(output.data() + old, bytes, length);
            buffer->Unlock();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    if (output.empty() || !std::all_of(output.begin(), output.end(), [](float x) { return std::isfinite(x); }))
        throw std::runtime_error("音频为空或包含无效样本");
    return output;
}
std::string Transcribe(const std::vector<float> &samples, const std::function<bool()> &cancelled)
{
    auto config = GetConfiguredVoiceInput();
    if (config.asr_provider != "local")
        throw std::runtime_error("此功能使用本地 ASR；请先在语音设置中选择并启动本地识别模型");
    if (VoiceInput::ResolveAsrToken(config).empty())
        throw std::runtime_error("请先启动本地语音识别服务");
    const auto request = VoiceInput::BuildAsrHttpRequest(samples, config, std::time(nullptr));
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::runtime_error("无法创建本地识别请求");
    curl_slist *raw = curl_slist_append(nullptr, ("Content-Type: " + request.content_type).c_str());
    for (const auto &header : request.headers)
        raw = curl_slist_append(raw, header.c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, request.endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION, +[](char *data, size_t size, size_t count, void *opaque) -> size_t {
            auto &out = *static_cast<std::string *>(opaque);
            const auto length = size * count;
            if (out.size() + length > 1024 * 1024)
                return 0;
            out.append(data, length);
            return length;
        });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        curl.get(), CURLOPT_XFERINFOFUNCTION, +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return (*static_cast<const std::function<bool()> *>(p))() ? 1 : 0;
        });
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancelled);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 180L);
    const auto code = curl_easy_perform(curl.get());
    long http = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http);
    if (cancelled())
        throw std::runtime_error("已取消识别");
    if (code != CURLE_OK || http != 200)
        throw std::runtime_error("本地识别失败，请检查模型是否运行");
    return VoiceInput::ParseAsrHttpResponse(response, "local");
}
class Studio
{
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> recordStop_{false};
    bool quit_ = false, pending_ = false;
    Job job_;
    std::vector<float> input_;
    Speech::PcmAudio audio_;
    std::filesystem::path draft_;
    std::string artifact_, text_, phase_ = "idle", message_ = "输入文字，或录音 / 导入音频后识别，再选择音色生成。";
    double seconds_ = 0;
    std::thread worker_;
    void Update(uint64_t id, std::string phase, std::string message)
    {
        std::lock_guard lock(mutex_);
        if (generation_ == id)
        {
            phase_ = std::move(phase);
            message_ = std::move(message);
        }
    }
    void Run()
    {
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return quit_ || pending_; });
                if (quit_)
                    break;
                job = std::move(job_);
                pending_ = false;
            }
            const auto cancelled = [&] { return generation_.load() != job.generation; };
            try
            {
                if (FAILED(hr))
                    throw std::runtime_error("无法初始化本机语音工作台");
                if (job.action == "record")
                {
                    Update(job.generation, "recording",
                           "正在录音，最多 90 秒；点击结束后进行本地识别，不会上屏或发送。");
                    metasequoia::voice::AudioCapture capture;
                    std::mutex captureMutex;
                    std::vector<float> samples;
                    std::atomic<bool> full{false};
                    if (!capture.start([&](const float *data, size_t size) {
                            std::lock_guard lock(captureMutex);
                            const auto count = std::min(size, maxFrames - samples.size());
                            samples.insert(samples.end(), data, data + count);
                            full = samples.size() >= maxFrames;
                        }))
                        throw std::runtime_error("无法打开麦克风，请检查设备和麦克风权限");
                    while (!recordStop_ && !full && !cancelled())
                        Sleep(40);
                    capture.stop();
                    if (capture.callback_failed())
                        throw std::runtime_error("录音设备发生错误，请重新录音");
                    if (!cancelled())
                    {
                        std::lock_guard lock(mutex_);
                        input_ = std::move(samples);
                        seconds_ = input_.size() / 16000.0;
                        phase_ = "captured";
                        message_ = "录音已保存在内存，点击“本地识别成文字”后可编辑。";
                    }
                }
                else if (job.action == "import")
                {
                    auto samples = ReadAudio(job.path, cancelled);
                    if (!cancelled())
                    {
                        std::lock_guard lock(mutex_);
                        input_ = std::move(samples);
                        seconds_ = input_.size() / 16000.0;
                        phase_ = "captured";
                        message_ = "音频已导入，点击本地识别；不会发送给聊天软件。";
                    }
                }
                else if (job.action == "transcribe")
                {
                    std::vector<float> samples;
                    {
                        std::lock_guard lock(mutex_);
                        samples = input_;
                    }
                    if (samples.empty())
                        throw std::runtime_error("请先录音或导入音频");
                    auto text = Transcribe(samples, cancelled);
                    if (!cancelled())
                    {
                        std::lock_guard lock(mutex_);
                        text_ = std::move(text);
                        phase_ = "transcribed";
                        message_ = "识别完成。请检查文字，选择目标方言音色，再生成语音。";
                    }
                }
                else if (job.action == "generate")
                {
                    Speech::PcmPlayer player(generation_, job.generation, 100);
                    auto audio = Speech::RenderAudio(job.config, job.text, job.language, cancelled,
                                                     [&](unsigned rate, unsigned channels, const std::string &pcm) {
                                                         player.Write(rate, channels, pcm);
                                                     });
                    player.Finish();
                    if (cancelled())
                        continue;
                    GUID guid{};
                    if (FAILED(CoCreateGuid(&guid)))
                        throw std::runtime_error("无法创建语音草稿");
                    wchar_t id[64]{};
                    StringFromGUID2(guid, id, 64);
                    auto path = metasequoia::data_directory() / "voice-outbox" / (std::wstring(id) + L".wav");
                    std::filesystem::create_directories(path.parent_path());
                    const auto wav = Speech::EncodeWav(audio);
                    std::ofstream file(path, std::ios::binary);
                    file.write(wav.data(), wav.size());
                    file.close();
                    if (!file)
                        throw std::runtime_error("无法保存语音草稿");
                    std::lock_guard lock(mutex_);
                    if (!cancelled())
                    {
                        audio_ = std::move(audio);
                        draft_ = path;
                        artifact_ = wstring_to_string(id);
                        phase_ = "ready";
                        message_ = "语音已生成并试听，保存在本机草稿目录。请选择手动发送方式。";
                    }
                }
                else if (job.action == "play" || job.action == "route")
                {
                    if (job.action == "route")
                    {
                        for (int remaining = 3; remaining > 0 && !cancelled(); --remaining)
                        {
                            Update(job.generation, "routing",
                                   std::to_string(remaining) +
                                       " 秒后输出；请切回聊天软件并开始录音。发送由你手动操作。");
                            for (int i = 0; i < 20 && !cancelled(); ++i)
                                Sleep(50);
                        }
                    }
                    Speech::PcmPlayer player(generation_, job.generation, 100, job.device);
                    for (size_t pos = 0; pos < job.audio.pcm.size(); pos += 16384)
                        player.Write(job.audio.rate, job.audio.channels, job.audio.pcm.substr(pos, 16384));
                    player.Finish();
                    Update(job.generation, "ready",
                           job.action == "route" ? "已输出到所选音频设备，请在聊天软件中手动结束录音并发送。"
                                                 : "试听完成，未发送任何消息。");
                }
            }
            catch (const std::exception &e)
            {
                Update(job.generation, "error", e.what());
            }
        }
        if (SUCCEEDED(hr))
            CoUninitialize();
    }

  public:
    Studio() : worker_([this] { Run(); })
    {
    }
    ~Studio()
    {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
            ++generation_;
        }
        cv_.notify_one();
        worker_.join();
    }
    void Enqueue(Job job)
    {
        Speech::Stop();
        std::lock_guard lock(mutex_);
        job.generation = ++generation_;
        recordStop_ = false;
        if (job.action == "generate")
        {
            artifact_.clear();
            audio_ = {};
            draft_.clear();
        }
        phase_ = job.action == "record" ? "recording" : "working";
        message_ = "正在处理，请稍候；可以随时停止。";
        job_ = std::move(job);
        pending_ = true;
        cv_.notify_one();
    }
    void Stop()
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        pending_ = false;
        recordStop_ = true;
        phase_ = artifact_.empty() ? "idle" : "ready";
        message_ = "已停止；未发送消息。";
    }
    void EndRecording()
    {
        recordStop_ = true;
    }
    json Status()
    {
        std::lock_guard lock(mutex_);
        return {{"phase", phase_},
                {"message", message_},
                {"transcript", text_},
                {"inputSeconds", seconds_},
                {"hasInput", !input_.empty()},
                {"artifactId", artifact_},
                {"outputSeconds", audio_.rate ? audio_.pcm.size() / double(audio_.rate * audio_.channels * 2) : 0.0},
                {"devices", Devices()}};
    }
    Job AudioJob(const std::string &id, const std::string &action)
    {
        std::lock_guard lock(mutex_);
        if (id.empty() || id != artifact_ || audio_.pcm.empty())
            throw std::runtime_error("音频草稿已变化，请重新生成后再操作");
        Job job;
        job.action = action;
        job.audio = audio_;
        job.path = draft_;
        return job;
    }
    std::string LoadDraft(const std::string &filename)
    {
        if (filename.size() != 42 || filename.front() != '{' || filename.substr(37) != "}.wav" ||
            filename.substr(0, 38).find_first_not_of("{}-0123456789ABCDEFabcdef") != std::string::npos)
            throw std::runtime_error("收藏音频路径无效");
        const auto path = metasequoia::data_directory() / "voice-outbox" / filename;
        if (std::filesystem::file_size(path) > 18 * 1024 * 1024)
            throw std::runtime_error("音频超过上限");
        std::ifstream file(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), {});
        auto audio = Speech::ParseWav(bytes);
        std::lock_guard lock(mutex_);
        ++generation_;
        pending_ = false;
        recordStop_ = true;
        audio_ = std::move(audio);
        draft_ = path;
        artifact_ = filename.substr(0, 38);
        phase_ = "ready";
        message_ = "已载入收藏语音。";
        return artifact_;
    }
};
std::atomic<Studio *> activeStudio{nullptr};
Studio &Instance()
{
    static Studio value;
    activeStudio = &value;
    return value;
}
void CopyAudio(HWND owner, const std::filesystem::path &path)
{
    const auto text = path.wstring();
    HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (text.size() + 2) * sizeof(wchar_t));
    if (!memory)
        throw std::runtime_error("无法准备音频剪贴板");
    auto *data = static_cast<DROPFILES *>(GlobalLock(memory));
    if (!data)
    {
        GlobalFree(memory);
        throw std::runtime_error("无法准备音频剪贴板");
    }
    data->pFiles = sizeof(DROPFILES);
    data->fWide = TRUE;
    memcpy(reinterpret_cast<char *>(data) + sizeof(DROPFILES), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!OpenClipboard(owner))
    {
        GlobalFree(memory);
        throw std::runtime_error("剪贴板正被占用，请重试");
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_HDROP, memory) != nullptr;
    CloseClipboard();
    if (!ok)
    {
        GlobalFree(memory);
        throw std::runtime_error("无法复制音频文件");
    }
}
} // namespace
void Stop()
{
    if (auto *studio = activeStudio.load())
        studio->Stop();
}
std::string LoadSavedDraft(const std::string &filename)
{
    return Instance().LoadDraft(filename);
}
std::string TranscribeAudio(const std::vector<float> &samples, const std::function<bool()> &cancelled)
{
    return Transcribe(samples, cancelled);
}
std::vector<float> ReadAudioFile(const std::filesystem::path &path, const std::function<bool()> &cancelled)
{
    return ReadAudio(path, cancelled);
}
nlohmann::json Handle(const nlohmann::json &request, HWND owner)
{
    json response = {{"type", "voiceStudioResponse"},
                     {"protocolVersion", 1},
                     {"requestId", request.value("requestId", "")},
                     {"ok", true},
                     {"message", ""}};
    auto &studio = Instance();
    try
    {
        const auto action = request.at("action").get<std::string>();
        if (action == "stop")
            studio.Stop();
        else if (action == "recordStop")
            studio.EndRecording();
        else if (action == "recordStart" || action == "transcribe")
        {
            Job job;
            job.action = action == "recordStart" ? "record" : action;
            studio.Enqueue(std::move(job));
        }
        else if (action == "import")
        {
            auto path = ChooseFile(owner, false);
            if (!path.empty())
            {
                Job job;
                job.action = "import";
                job.path = std::move(path);
                studio.Enqueue(std::move(job));
            }
        }
        else if (action == "generate")
        {
            Job job;
            job.action = action;
            job.text = request.value("text", "");
            job.language = request.value("language", "zh");
            if (job.text.empty() || job.text.size() > 2000)
                throw std::runtime_error("生成文字限 1–2000 UTF-8 字节，请缩短后重试");
            ReloadImeConfigIfChanged();
            LocalTts::Handle({{"action", "status"}});
            job.config = Speech::GetConfig();
            const auto voice = request.value("voice", "");
            if (!voice.empty())
                job.config[job.config.at("provider") + "_voice_" + (job.language == "zh" ? "zh" : "en")] = voice;
            if (request.contains("rate"))
                job.config["rate"] = std::to_string(request.at("rate").get<double>());
            if (request.contains("optionsJson") && job.config.at("provider") == "local")
            {
                auto parameters = json::parse(request.at("optionsJson").get<std::string>());
                if (!parameters.is_object())
                    throw std::runtime_error("模型参数格式无效");
                job.config["local_options"] = json{{job.config.at("local_model"), parameters}}.dump();
            }
            studio.Enqueue(std::move(job));
        }
        else if (action == "play" || action == "route" || action == "copy" || action == "save")
        {
            auto job = studio.AudioJob(request.value("artifactId", ""), action);
            if (action == "copy")
            {
                CopyAudio(owner, job.path);
                response["message"] = "音频文件已复制；切到聊天窗口按 Ctrl+V，再由你手动发送。";
            }
            else if (action == "save")
            {
                auto path = ChooseFile(owner, true);
                if (!path.empty())
                {
                    std::filesystem::copy_file(job.path, path, std::filesystem::copy_options::overwrite_existing);
                    response["message"] = "音频已保存。";
                }
            }
            else
            {
                if (action == "route")
                {
                    const auto id = request.at("deviceId").get<unsigned>();
                    bool found = false;
                    for (const auto &device : Devices())
                        if (device.at("id") == id && device.at("virtual") == true)
                            found = true;
                    if (!found)
                        throw std::runtime_error("请选择可用的虚拟音频设备；普通扬声器不能作为聊天软件的麦克风");
                    job.device = id;
                }
                studio.Enqueue(std::move(job));
            }
        }
        else if (action != "status")
            throw std::runtime_error("未知语音工作台操作");
    }
    catch (const std::exception &e)
    {
        response["ok"] = false;
        response["message"] = e.what();
    }
    response["state"] = studio.Status();
    return response;
}
} // namespace VoiceStudio
