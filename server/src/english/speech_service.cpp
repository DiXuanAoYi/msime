#include "speech_service.h"
#include "speech_voice_catalog.h"
#include "pcm_stream.h"
#include "pcm_player.h"
#include "settings/local_tts_models.h"
#include "cloud/tencent_tmt.h"
#include "utils/common_utils.h"
#include "utils/network_proxy.h"
#include "engine/core/data_path.h"
#include "config/ime_config.h"
#include <fstream>
#include <Windows.h>
#include <sapi.h>
#include <wrl/client.h>
#include <wincrypt.h>
#include <mmsystem.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cmath>
#include <regex>
#include <ctime>
#include <sstream>
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "crypt32.lib")

namespace Speech
{
namespace
{
using nlohmann::json;
std::string Xml(std::string value)
{
    std::string out;
    for (char c : value)
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    return out;
}
std::string Base64(const std::string &text)
{
    DWORD size = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &size,
                              nullptr, nullptr) ||
        size > 16 * 1024 * 1024)
        throw std::runtime_error("语音音频编码无效");
    std::string out(size, '\0');
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE *>(out.data()), &size, nullptr, nullptr))
        throw std::runtime_error("语音音频解码失败");
    out.resize(size);
    return out;
}
std::string Wav(const std::string &pcm)
{
    if (pcm.empty() || pcm.size() % 2 != 0)
        throw std::runtime_error("语音音频为空或不完整");
    std::string out = "RIFF";
    auto u32 = [&](uint32_t n) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<char>(n >> (8 * i)));
    };
    u32(static_cast<uint32_t>(pcm.size() + 36));
    out += "WAVEfmt ";
    u32(16);
    out.append("\x01\0\x01\0", 4);
    u32(24000);
    u32(48000);
    out.append("\x02\0\x10\0", 4);
    out += "data";
    u32(static_cast<uint32_t>(pcm.size()));
    out += pcm;
    return out;
}
void Require(const std::string &value, const char *name)
{
    if (value.empty() || value.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error(std::string("请填写有效的 ") + name);
}
std::atomic<uint64_t> generation{0};
std::mutex statusMutex;
std::string status = "就绪：按住 Ctrl 点击候选原词或译文试听";
void Status(std::string text)
{
    std::lock_guard lock(statusMutex);
    status = std::move(text);
}
class Speaker
{
    std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false, pending_ = false;
    std::string text_, language_, audio_;
    std::thread thread_;
    struct StreamRequest
    {
        PcmStream &parser;
        std::string error;
    };
    static size_t WriteStream(char *data, size_t size, size_t count, void *p)
    {
        auto &stream = *static_cast<StreamRequest *>(p);
        try
        {
            stream.parser.Feed(std::string_view(data, size * count));
            return size * count;
        }
        catch (const std::exception &e)
        {
            stream.error = e.what();
            return 0;
        }
    }
    static size_t Write(char *data, size_t size, size_t count, void *p)
    {
        auto &out = *static_cast<std::string *>(p);
        size_t n = size * count;
        if (n > 16 * 1024 * 1024 || out.size() > 16 * 1024 * 1024 - n)
            return 0;
        out.append(data, n);
        return n;
    }
    static int Progress(void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        return generation.load() != *static_cast<uint64_t *>(p);
    }
    void Run()
    {
        auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr))
        {
            Status("无法初始化语音组件");
            return;
        }
        {
            Microsoft::WRL::ComPtr<ISpVoice> voice;
            CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&voice));
            for (;;)
            {
                std::string text, lang;
                uint64_t request;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [&] { return quit_ || pending_; });
                    if (quit_)
                        break;
                    text = std::move(text_);
                    lang = std::move(language_);
                    pending_ = false;
                    request = generation.load();
                }
                PlaySoundW(nullptr, nullptr, 0);
                if (voice)
                    voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                audio_.clear();
                if (text.empty())
                {
                    Status("已停止朗读");
                    continue;
                }
                try
                {
                    auto config = GetConfig();
                    if (config.at("enabled") != "true")
                    {
                        Status("发音功能已关闭");
                        continue;
                    }
                    Status("正在合成…");
                    const auto provider = config.at("provider");
                    if (provider == "sapi")
                    {
                        if (!voice)
                            throw std::runtime_error("本机 Windows 语音不可用");
                        Microsoft::WRL::ComPtr<ISpObjectTokenCategory> category;
                        Microsoft::WRL::ComPtr<IEnumSpObjectTokens> tokens;
                        Microsoft::WRL::ComPtr<ISpObjectToken> selected;
                        if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
                                                    IID_PPV_ARGS(&category))) ||
                            FAILED(category->SetId(SPCAT_VOICES, FALSE)))
                            throw std::runtime_error("无法枚举本机音色");
                        std::string lcid = "804";
                        const std::map<std::string, std::string> ids = {{"en", "409"}, {"ja", "411"}, {"es", "40A"},
                                                                        {"vi", "42A"}, {"fr", "40C"}, {"de", "407"},
                                                                        {"ru", "419"}, {"ko", "412"}};
                        if (ids.count(lang))
                            lcid = ids.at(lang);
                        if (FAILED(category->EnumTokens((L"Language=" + string_to_wstring(lcid)).c_str(), nullptr,
                                                        &tokens)))
                            throw std::runtime_error("本机缺少该语言的语音包");
                        auto wanted = config.at(lang == "zh" ? "sapi_voice_zh" : "sapi_voice_en");
                        Microsoft::WRL::ComPtr<ISpObjectToken> token;
                        while (tokens->Next(1, &token, nullptr) == S_OK)
                        {
                            LPWSTR id = nullptr;
                            token->GetId(&id);
                            bool match = wanted.empty() || (id && string_to_wstring(wanted) == id);
                            CoTaskMemFree(id);
                            if (match)
                            {
                                selected = token;
                                break;
                            }
                            token.Reset();
                        }
                        if (!selected)
                            throw std::runtime_error("所选音色不可用，请安装语音包或重新选择音色");
                        voice->SetVoice(selected.Get());
                        voice->SetRate(
                            static_cast<long>(std::clamp(std::log2(std::stod(config.at("rate"))) * 5.0, -10.0, 10.0)));
                        voice->SetVolume(static_cast<USHORT>(std::clamp(std::stoi(config.at("volume")), 0, 100)));
                        if (request != generation.load())
                            continue;
                        if (FAILED(voice->Speak(string_to_wstring(text).c_str(),
                                                SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr)))
                            throw std::runtime_error("Windows 朗读失败");
                    }
                    else
                    {
                        auto payload = BuildRequest(config, text, lang, std::time(nullptr));
                        const bool streaming =
                            provider == "local" && payload.url == "http://127.0.0.1:18767/v1/audio/speech";
                        if (streaming)
                        {
                            std::ifstream file(metasequoia::data_directory() / "models" / "tts" / "service-token.txt");
                            std::string token;
                            file >> token;
                            if (token.size() != 64 || token.find_first_not_of("0123456789abcdef") != std::string::npos)
                                throw std::runtime_error("请先在设置中启动本地发音模型");
                            payload.headers.push_back("Authorization: Bearer " + token);
                            auto body = json::parse(payload.body);
                            body["stream"] = true;
                            payload.body = body.dump();
                        }
                        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
                        if (!curl)
                            throw std::runtime_error("无法初始化发音请求");
                        curl_slist *list = nullptr;
                        for (const auto &header : payload.headers)
                            list = curl_slist_append(list, header.c_str());
                        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(list, curl_slist_free_all);
                        std::string body;
                        PcmPlayer player(generation, request, std::clamp(std::stoi(config.at("volume")), 0, 100));
                        const auto started = std::chrono::steady_clock::now();
                        bool firstAudio = true;
                        long long firstAudioMs = 0;
                        PcmStream parser([&](unsigned rate, unsigned channels, std::string pcm) {
                            player.Write(rate, channels, std::move(pcm));
                            if (firstAudio)
                            {
                                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - started)
                                              .count();
                                firstAudioMs = ms;
                                Status("流式朗读中 · 首段送入播放器 " + std::to_string(ms) + " ms（不会上屏或发送）");
                                firstAudio = false;
                            }
                        });
                        StreamRequest streamRequest{parser, {}};
                        curl_easy_setopt(curl.get(), CURLOPT_URL, payload.url.c_str());
                        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
                        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.body.c_str());
                        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.body.size()));
                        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Write);
                        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
                        if (streaming)
                        {
                            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteStream);
                            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &streamRequest);
                        }
                        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 90000L);
                        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
                        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
                        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, Progress);
                        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &request);
                        NetworkProxy::ApplyToCurl(curl.get());
                        if (provider == "local")
                        {
                            curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
                            curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
                        }
                        auto result = curl_easy_perform(curl.get());
                        if (request != generation.load())
                            continue;
                        if (!streamRequest.error.empty())
                            throw std::runtime_error(streamRequest.error);
                        if (result != CURLE_OK)
                            throw std::runtime_error("发音请求未完成：请检查服务、网络或本地模型是否启动");
                        long code = 0;
                        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
                        if (code != 200)
                            throw std::runtime_error("发音服务 HTTP " + std::to_string(code) +
                                                     "：请检查凭据、音色和额度");
                        if (streaming)
                        {
                            parser.Finish();
                            player.Finish();
                            Status("已完成流式朗读 · 首段送入播放器 " + std::to_string(firstAudioMs) + " ms");
                            continue;
                        }
                        audio_ = DecodeAudio(provider, body);
                        // Scale PCM locally so volume works consistently across providers.
                        int volume = std::clamp(std::stoi(config.at("volume")), 0, 100);
                        for (size_t pos = 12; pos + 8 <= audio_.size();)
                        {
                            uint32_t size = 0;
                            memcpy(&size, audio_.data() + pos + 4, 4);
                            if (size > audio_.size() - pos - 8)
                                break;
                            if (audio_.compare(pos, 4, "data") == 0)
                                for (size_t i = pos + 8; i + 1 < pos + 8 + size; i += 2)
                                {
                                    int16_t sample;
                                    memcpy(&sample, audio_.data() + i, 2);
                                    sample = static_cast<int16_t>(int(sample) * volume / 100);
                                    memcpy(audio_.data() + i, &sample, 2);
                                }
                            pos += 8 + size + (size & 1);
                        }
                        if (request != generation.load())
                            continue;
                        if (!PlaySoundW(reinterpret_cast<LPCWSTR>(audio_.data()), nullptr,
                                        SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
                            throw std::runtime_error("音频播放失败");
                    }
                    if (request == generation.load())
                        Status("已开始朗读（不会上屏或发送）");
                }
                catch (const std::exception &e)
                {
                    if (request == generation.load())
                        Status(e.what());
                }
            }
            PlaySoundW(nullptr, nullptr, 0);
            if (voice)
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
        }
        CoUninitialize();
    }

  public:
    Speaker() : thread_([this] { Run(); })
    {
    }
    ~Speaker()
    {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
            ++generation;
        }
        cv_.notify_one();
        thread_.join();
    }
    void Queue(std::string text, std::string language)
    {
        {
            std::lock_guard lock(mutex_);
            text_ = std::move(text);
            language_ = std::move(language);
            pending_ = true;
            ++generation;
        }
        cv_.notify_one();
    }
};
Speaker &Instance()
{
    static Speaker speaker;
    return speaker;
}
} // namespace

Request BuildRequest(const Config &c, const std::string &text, const std::string &lang, int64_t timestamp)
{
    if (text.empty() || text.size() > 2000)
        throw std::runtime_error("每次朗读限 2000 UTF-8 字节");
    Request r;
    const auto provider = c.at("provider");
    auto voice = c.at(provider + "_voice_" + (lang == "zh" ? "zh" : "en"));
    std::string dialect;
    if ((provider == "azure" || provider == "volcengine") && voice.find('|') != std::string::npos)
    {
        const auto pos = voice.find('|');
        dialect = voice.substr(pos + 1);
        voice.resize(pos);
        if (!std::regex_match(dialect, std::regex("[A-Za-z-]{2,40}")))
            throw std::runtime_error("方言代码格式无效");
    }
    const double rate = std::stod(c.at("rate"));
    if (!std::isfinite(rate) || rate < 0.5 || rate > 2.0)
        throw std::runtime_error("发音语速必须为 0.5–2");
    r.headers = {"Content-Type: application/json"};
    if (provider == "azure")
    {
        Require(c.at("azure_key"), "Azure Key");
        Require(voice, "Azure 音色");
        if (!std::regex_match(c.at("azure_region"), std::regex("[a-z0-9-]{2,40}")))
            throw std::runtime_error("Azure 地域格式无效");
        r.url = "https://" + c.at("azure_region") + ".tts.speech.microsoft.com/cognitiveservices/v1";
        r.headers = {"Content-Type: application/ssml+xml", "Ocp-Apim-Subscription-Key: " + c.at("azure_key"),
                     "X-Microsoft-OutputFormat: riff-24khz-16bit-mono-pcm", "User-Agent: MetasequoiaIME"};
        r.body = "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='" +
                 (lang == "zh" ? std::string("zh-CN") : std::string("en-US")) + "'><voice name='" + Xml(voice) +
                 "'><prosody rate='" + std::to_string(int((rate - 1) * 100)) + "%'>" +
                 (dialect.empty() ? Xml(text) : "<lang xml:lang='" + Xml(dialect) + "'>" + Xml(text) + "</lang>") +
                 "</prosody></voice></speak>";
    }
    else if (provider == "elevenlabs")
    {
        Require(c.at("elevenlabs_key"), "ElevenLabs API Key");
        if (!std::regex_match(voice, std::regex("[A-Za-z0-9_-]{1,128}")))
            throw std::runtime_error("请填写 ElevenLabs Voice ID");
        r.url = "https://api.elevenlabs.io/v1/text-to-speech/" + voice + "?output_format=pcm_24000";
        r.headers.push_back("xi-api-key: " + c.at("elevenlabs_key"));
        r.body = json{{"text", text},
                      {"model_id", c.at("elevenlabs_model")},
                      {"voice_settings", {{"speed", std::clamp(rate, 0.7, 1.2)}}}}
                     .dump();
    }
    else if (provider == "tencent")
    {
        Require(c.at("tencent_secret_id"), "SecretId");
        Require(c.at("tencent_secret_key"), "SecretKey");
        if (!std::regex_match(voice, std::regex("[0-9]{1,9}")))
            throw std::runtime_error("腾讯音色应为数字 VoiceType");
        r.url = "https://tts.tencentcloudapi.com";
        r.body = json{{"Text", text},
                      {"SessionId", std::to_string(timestamp)},
                      {"VoiceType", std::stoi(voice)},
                      {"Codec", "wav"},
                      {"SampleRate", 24000},
                      {"Speed", std::clamp((rate - 1) * 2, -2.0, 2.0)}}
                     .dump();
        r.headers = {"Content-Type: application/json; charset=utf-8",
                     "Host: tts.tencentcloudapi.com",
                     "X-TC-Action: TextToVoice",
                     "X-TC-Version: 2019-08-23",
                     "X-TC-Timestamp: " + std::to_string(timestamp),
                     "X-TC-Region: " + c.at("tencent_region"),
                     "Authorization: " +
                         TencentTmt::TtsAuthorization(
                             {c.at("tencent_secret_id"), c.at("tencent_secret_key"), c.at("tencent_region")}, r.body,
                             timestamp)};
    }
    else if (provider == "volcengine")
    {
        Require(c.at("volcengine_app_id"), "火山 App ID");
        Require(c.at("volcengine_key"), "火山 Access Token");
        Require(voice, "火山音色");
        r.url = "https://openspeech.bytedance.com/api/v3/tts/unidirectional";
        r.headers.insert(r.headers.end(),
                         {"X-Api-App-Id: " + c.at("volcengine_app_id"), "X-Api-Access-Key: " + c.at("volcengine_key"),
                          "X-Api-Resource-Id: " + c.at("volcengine_resource")});
        r.body = json{{"user", {{"uid", "metasequoia-ime"}}},
                      {"req_params",
                       {{"text", text},
                        {"speaker", voice},
                        {"additions", (dialect.empty() ? json::object() : json{{"explicit_dialect", dialect}}).dump()},
                        {"audio_params",
                         {{"format", "pcm"}, {"sample_rate", 24000}, {"speech_rate", int((rate - 1) * 100)}}}}}}
                     .dump();
    }
    else if (provider == "local")
    {
        if (!std::regex_match(c.at("local_endpoint"),
                              std::regex(R"(http://(127\.0\.0\.1|localhost|\[::1\]):[0-9]{1,5}/[A-Za-z0-9/_-]+)")))
            throw std::runtime_error("本地发音地址仅支持本机回环 HTTP 服务");
        r.url = c.at("local_endpoint");
        r.body = json{{"input", text}, {"model", c.at("local_model")}, {"voice", voice}, {"language", lang},
                      {"speed", rate}, {"response_format", "wav"}}
                     .dump();
        if (r.url == "http://127.0.0.1:18767/v1/audio/speech" && c.count("local_options"))
        {
            auto options = json::parse(c.at("local_options"));
            const auto selected = options.value(c.at("local_model"), json::object());
            if (!selected.is_object())
                throw std::runtime_error("本地模型参数无效");
            if (!selected.empty())
            {
                auto body = json::parse(r.body);
                body["options"] = selected;
                r.body = body.dump();
            }
        }
    }
    else
        throw std::runtime_error("未知发音服务");
    for (const auto &header : r.headers)
        if (header.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("凭据格式无效");
    return r;
}
std::string DecodeAudio(const std::string &provider, const std::string &body)
{
    std::string audio = body;
    if (provider == "tencent")
    {
        auto j = nlohmann::json::parse(body);
        if (!j.contains("Response") || !j["Response"].contains("Audio"))
            throw std::runtime_error("腾讯云未返回音频，请检查音色与服务权限");
        audio = Base64(j["Response"]["Audio"]);
    }
    if (provider == "volcengine")
    {
        std::istringstream lines(body);
        std::string line, pcm;
        bool complete = false;
        while (std::getline(lines, line))
        {
            if (line.empty())
                continue;
            auto j = nlohmann::json::parse(line);
            int code = j.value("code", -1);
            if (code == 20000000)
            {
                complete = true;
                continue;
            }
            if (code != 0)
                throw std::runtime_error("火山语音合成失败，请检查资源和音色");
            if (j.contains("data"))
                pcm += Base64(j["data"]);
        }
        if (!complete)
            throw std::runtime_error("火山音频响应不完整");
        audio = Wav(pcm);
    }
    if (provider == "elevenlabs")
        audio = Wav(body);
    if (audio.size() < 44 || audio.size() > 16 * 1024 * 1024 || audio.compare(0, 4, "RIFF") ||
        audio.compare(8, 4, "WAVE"))
        throw std::runtime_error("服务未返回有效 WAV 音频");
    auto u32 = [&](size_t at) {
        uint32_t value;
        memcpy(&value, audio.data() + at, 4);
        return value;
    };
    auto u16 = [&](size_t at) {
        uint16_t value;
        memcpy(&value, audio.data() + at, 2);
        return value;
    };
    if (u32(4) != audio.size() - 8)
        throw std::runtime_error("WAV 音频长度不完整");
    bool format = false, samples = false;
    for (size_t pos = 12; pos + 8 <= audio.size();)
    {
        const auto size = u32(pos + 4);
        if (size > audio.size() - pos - 8)
            throw std::runtime_error("WAV 数据块不完整");
        if (audio.compare(pos, 4, "fmt ") == 0)
        {
            if (size < 16 || u16(pos + 8) != 1 || u16(pos + 22) != 16 || u16(pos + 10) < 1 || u16(pos + 10) > 2)
                throw std::runtime_error("发音服务需返回 PCM16 WAV");
            format = true;
        }
        if (audio.compare(pos, 4, "data") == 0)
            samples = size > 0 && size % 2 == 0;
        pos += 8 + size + (size & 1);
    }
    if (!format || !samples)
        throw std::runtime_error("WAV 缺少音频数据");
    return audio;
}
PcmAudio RenderAudio(const Config &config, const std::string &text, const std::string &language,
                     const std::function<bool()> &cancelled,
                     const std::function<void(unsigned, unsigned, const std::string &)> &onChunk)
{
    if (text.empty() || text.size() > 2000)
        throw std::runtime_error("每段语音限 2000 UTF-8 字节");
    PcmAudio result;
    const int volume = std::clamp(std::stoi(config.at("volume")), 0, 100);
    auto accept = [&](unsigned rate, unsigned channels, std::string pcm) {
        if (cancelled())
            throw std::runtime_error("已取消语音生成");
        if (!result.rate)
        {
            result.rate = rate;
            result.channels = channels;
        }
        if (result.rate != rate || result.channels != channels ||
            result.pcm.size() + pcm.size() > size_t(rate) * channels * 2 * 90)
            throw std::runtime_error("音频格式变化或超过 90 秒");
        for (size_t i = 0; i + 1 < pcm.size(); i += 2)
        {
            int16_t sample;
            memcpy(&sample, pcm.data() + i, 2);
            sample = static_cast<int16_t>(int(sample) * volume / 100);
            memcpy(pcm.data() + i, &sample, 2);
        }
        result.pcm += pcm;
        if (onChunk)
            onChunk(rate, channels, pcm);
    };
    const auto provider = config.at("provider");
    if (provider == "sapi")
    {
        Microsoft::WRL::ComPtr<ISpVoice> voice;
        Microsoft::WRL::ComPtr<ISpStream> speechStream;
        Microsoft::WRL::ComPtr<IStream> memory;
        if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&voice))) ||
            FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&speechStream))) ||
            FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory)))
            throw std::runtime_error("无法初始化 Windows 音频生成");
        WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};
        if (FAILED(speechStream->SetBaseStream(memory.Get(), SPDFID_WaveFormatEx, &format)) ||
            FAILED(voice->SetOutput(speechStream.Get(), TRUE)))
            throw std::runtime_error("Windows 语音不支持 PCM 输出");
        Microsoft::WRL::ComPtr<ISpObjectTokenCategory> category;
        Microsoft::WRL::ComPtr<IEnumSpObjectTokens> tokens;
        const auto wanted = config.at(language == "zh" ? "sapi_voice_zh" : "sapi_voice_en");
        const std::map<std::string, std::wstring> lcids = {{"zh", L"804"}, {"en", L"409"}, {"ja", L"411"},
                                                           {"ko", L"412"}, {"fr", L"40C"}, {"de", L"407"},
                                                           {"es", L"40A"}};
        const auto filter = L"Language=" + (lcids.count(language) ? lcids.at(language) : L"409");
        if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&category))) ||
            FAILED(category->SetId(SPCAT_VOICES, FALSE)) ||
            FAILED(category->EnumTokens(wanted.empty() ? filter.c_str() : nullptr, nullptr, &tokens)))
            throw std::runtime_error("无法获取 Windows 音色");
        Microsoft::WRL::ComPtr<ISpObjectToken> token;
        bool selected = false;
        while (tokens->Next(1, &token, nullptr) == S_OK)
        {
            LPWSTR id = nullptr;
            token->GetId(&id);
            const bool match = wanted.empty() || (id && string_to_wstring(wanted) == id);
            CoTaskMemFree(id);
            if (match)
            {
                voice->SetVoice(token.Get());
                selected = true;
                break;
            }
            token.Reset();
        }
        if (!selected)
            throw std::runtime_error("请安装或选择可用的 Windows 音色");
        voice->SetRate(static_cast<long>(std::clamp(std::log2(std::stod(config.at("rate"))) * 5.0, -10.0, 10.0)));
        voice->SetVolume(100);
        if (FAILED(voice->Speak(string_to_wstring(text).c_str(), SPF_ASYNC | SPF_IS_NOT_XML, nullptr)))
            throw std::runtime_error("Windows 语音合成失败");
        while (voice->WaitUntilDone(50) == S_FALSE)
            if (cancelled())
            {
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                throw std::runtime_error("已取消语音生成");
            }
        STATSTG stat{};
        if (FAILED(memory->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 24000 * 2 * 90)
            throw std::runtime_error("生成音频超过 90 秒");
        memory->Seek({}, STREAM_SEEK_SET, nullptr);
        std::string pcm(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
        ULONG read = 0;
        if (FAILED(memory->Read(pcm.data(), static_cast<ULONG>(pcm.size()), &read)) || read != pcm.size())
            throw std::runtime_error("无法读取生成音频");
        accept(24000, 1, std::move(pcm));
    }
    else
    {
        auto payload = BuildRequest(config, text, language, std::time(nullptr));
        const bool streaming = provider == "local" && payload.url == "http://127.0.0.1:18767/v1/audio/speech";
        if (streaming)
        {
            std::ifstream file(metasequoia::data_directory() / "models" / "tts" / "service-token.txt");
            std::string token;
            file >> token;
            if (token.size() != 64 || token.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("请先启动本地发音模型");
            payload.headers.push_back("Authorization: Bearer " + token);
            auto body = json::parse(payload.body);
            body["stream"] = true;
            payload.body = body.dump();
        }
        PcmStream parser(accept);
        struct Receive
        {
            PcmStream *parser;
            std::string body, error;
        } receive{streaming ? &parser : nullptr, {}, {}};
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl)
            throw std::runtime_error("无法初始化语音请求");
        curl_slist *list = nullptr;
        for (const auto &header : payload.headers)
            list = curl_slist_append(list, header.c_str());
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(list, curl_slist_free_all);
        curl_easy_setopt(curl.get(), CURLOPT_URL, payload.url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.body.size()));
        curl_easy_setopt(
            curl.get(), CURLOPT_WRITEFUNCTION, +[](char *data, size_t size, size_t count, void *opaque) -> size_t {
                auto &r = *static_cast<Receive *>(opaque);
                const auto length = size * count;
                try
                {
                    if (r.parser)
                        r.parser->Feed(std::string_view(data, length));
                    else
                    {
                        if (r.body.size() + length > 16 * 1024 * 1024)
                            throw std::runtime_error("音频响应过大");
                        r.body.append(data, length);
                    }
                    return length;
                }
                catch (const std::exception &e)
                {
                    r.error = e.what();
                    return 0;
                }
            });
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &receive);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(
            curl.get(), CURLOPT_XFERINFOFUNCTION, +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return (*static_cast<const std::function<bool()> *>(p))() ? 1 : 0;
            });
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancelled);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 240L);
        if (provider == "local")
            curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
        else
            NetworkProxy::ApplyToCurl(curl.get());
        const auto code = curl_easy_perform(curl.get());
        long statusCode = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode);
        if (cancelled())
            throw std::runtime_error("已取消语音生成");
        if (!receive.error.empty())
            throw std::runtime_error(receive.error);
        if (code != CURLE_OK || statusCode != 200)
            throw std::runtime_error("语音生成失败，请检查模型状态或服务配置");
        if (streaming)
            parser.Finish();
        else
        {
            auto audio = ParseWav(DecodeAudio(provider, receive.body));
            accept(audio.rate, audio.channels, std::move(audio.pcm));
        }
    }
    EncodeWav(result);
    return result;
}
void Speak(std::string text, std::string language)
{
    if (text.empty() || text.size() > 2000)
        return;
    ReloadImeConfigIfChanged();
    // Apply a completed explicit model switch even if settings was closed while loading.
    LocalTts::Handle({{"action", "status"}});
    Instance().Queue(std::move(text), std::move(language));
}
void Stop()
{
    Instance().Queue({}, {});
}
nlohmann::json Handle(const nlohmann::json &request)
{
    const auto action = request.value("action", "status");
    if (action == "preview")
        Speak(request.value("text", "你好，欢迎使用水杉输入法。Hello, welcome!"), request.value("language", "zh"));
    if (action == "stop")
        Stop();
    json voices = json::array();
    Microsoft::WRL::ComPtr<ISpObjectTokenCategory> category;
    Microsoft::WRL::ComPtr<IEnumSpObjectTokens> tokens;
    if (SUCCEEDED(
            CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&category))) &&
        SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE)) && SUCCEEDED(category->EnumTokens(nullptr, nullptr, &tokens)))
    {
        Microsoft::WRL::ComPtr<ISpObjectToken> token;
        while (tokens->Next(1, &token, nullptr) == S_OK)
        {
            LPWSTR id = nullptr, name = nullptr;
            token->GetId(&id);
            token->GetStringValue(nullptr, &name);
            if (id && name)
                voices.push_back({{"id", wstring_to_string(id)}, {"name", wstring_to_string(name)}});
            CoTaskMemFree(id);
            CoTaskMemFree(name);
            token.Reset();
        }
    }
    std::lock_guard lock(statusMutex);
    return {{"type", "speechResponse"},
            {"protocolVersion", 1},
            {"requestId", request.value("requestId", "")},
            {"ok", true},
            {"message", status},
            {"voices", voices},
            {"modelInfo", GetConfig().at("provider") == "local" ? LocalTts::ModelInfo(GetConfig().at("local_model"))
                                                                : CloudVoiceCatalog(GetConfig().at("provider"))}};
}
} // namespace Speech
