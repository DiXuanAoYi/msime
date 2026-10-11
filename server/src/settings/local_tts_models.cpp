#include "local_tts_models.h"
#include "engine/core/data_path.h"
#include "english/speech_service.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace LocalTts
{
namespace
{
using nlohmann::json;
json Read(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec || std::filesystem::file_size(path, ec) > 256 * 1024)
        return json::object();
    std::ifstream stream(path);
    return json::parse(stream, nullptr, false);
}
bool Alive(const json &state)
{
    if (!state.is_object() || !state.contains("pid") || !state["pid"].is_number_unsigned())
        return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, state["pid"].get<DWORD>());
    if (!p)
        return false;
    DWORD code = 0;
    bool active = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return active;
}
std::wstring Quote(std::wstring text)
{
    // Fixed executable/action/id and canonical Windows directory; no shell interpreter.
    if (text.find(L'"') != std::wstring::npos)
        throw std::runtime_error("路径无效");
    return L"\"" + text + L"\"";
}
void Launch(const std::filesystem::path &runtime, const std::filesystem::path &root, const std::string &id, bool serve)
{
    auto exe = runtime / L"MetasequoiaLocalASR.exe";
    if (!std::filesystem::is_regular_file(exe))
        throw std::runtime_error("本地运行组件缺失，请重新安装完整安装包。");
    std::wstring command = Quote(exe.wstring()) + L" --tts --root " + Quote(root.wstring()) +
                           (serve ? L" --action serve --id " : L" --action download --id ") +
                           std::wstring(id.begin(), id.end());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        runtime.c_str(), &startup, &process))
        throw std::runtime_error("无法启动本地模型组件。");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
} // namespace
nlohmann::json Handle(const nlohmann::json &request)
{
    using nlohmann::json;
    json response = {{"type", "localTtsResponse"},
                     {"protocolVersion", 1},
                     {"requestId", request.value("requestId", "")},
                     {"ok", true},
                     {"message", ""},
                     {"models", json::array()},
                     {"service", json::object()}};
    try
    {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        auto runtime = std::filesystem::path(path).parent_path() / L"local-asr";
        auto catalog = Read(runtime / "_internal" / "tts_catalog.json");
        if (!catalog.is_array())
            throw std::runtime_error("本地发音模型目录缺失，请安装包含本地发音组件的完整版本。");
        auto root = metasequoia::data_directory() / "models" / "tts";
        std::filesystem::create_directories(root);
        auto action = request.value("action", "status"), id = request.value("modelId", "");
        if (action == "stop")
        {
            std::ofstream(root / "stop") << "stop";
            response["message"] = "已请求停止本地发音";
        }
        else if (action != "status")
        {
            auto found =
                std::find_if(catalog.begin(), catalog.end(), [&](const json &row) { return row.at("id") == id; });
            if (found == catalog.end())
                throw std::runtime_error("未知模型，未执行操作。");
            if (found->value("engine", "") == "external")
                throw std::runtime_error("此模型请通过官方下载链接部署后填写本机服务地址。");
            if (action == "cancel")
            {
                std::filesystem::create_directories(root / id);
                std::ofstream(root / id / "cancel") << "stop";
            }
            else if (action == "download")
            {
                for (const auto &model : catalog)
                {
                    auto state = Read(root / model.at("id").get<std::string>() / "state.json");
                    if (Alive(state) && state.value("state", "") != "ready" && state.value("state", "") != "error")
                        throw std::runtime_error("已有模型正在下载或校验，请等它完成或先暂停。");
                }
                Launch(runtime, root, id, false);
                response["message"] = "已开始下载，关闭设置窗口后仍会继续";
            }
            else if (action == "start")
            {
                auto ready = Read(root / id / "ready.json");
                if (!ready.is_object() || ready.value("files", json::array()) != found->at("files"))
                    throw std::runtime_error("请先下载并完成校验。");
                if (Alive(Read(root / "service.json")))
                    throw std::runtime_error("已有本地模型正在启动或运行，请先停止它。");
                std::filesystem::remove(root / "stop");
                Launch(runtime, root, id, true);
                Speech::SetConfig("provider", "local");
                Speech::SetConfig("local_endpoint", "http://127.0.0.1:18767/v1/audio/speech");
                Speech::SetConfig("local_model", id);
                Speech::SetConfig("local_voice_zh", id == "kokoro" ? "zf_xiaobei" : "Vivian");
                Speech::SetConfig("local_voice_en", id == "kokoro" ? "af_heart" : "Ryan");
                response["message"] = "正在启动；就绪后候选发音将使用这个本地模型";
            }
            else
                throw std::runtime_error("未知模型操作。");
        }
        for (auto row : catalog)
        {
            auto state = Read(root / row.at("id").get<std::string>() / "state.json");
            if (state.is_object())
            {
                auto stage = state.value("state", "");
                if ((stage == "downloading" || stage == "verifying" || stage == "extracting") && !Alive(state))
                {
                    state["state"] = "paused";
                    state["message"] = "下载进程已退出，可点击继续下载";
                }
                state.erase("pid");
            }
            row["status"] = state.is_object() ? state : json::object();
            row.erase("files");
            response["models"].push_back(std::move(row));
        }
        auto service = Read(root / "service.json");
        if (service.is_object())
        {
            if ((service.value("state", "") == "loading" || service.value("state", "") == "running") && !Alive(service))
            {
                service["state"] = "stopped";
                service["message"] = "本地发音进程已退出";
            }
            service.erase("pid");
            response["service"] = service;
        }
    }
    catch (const std::exception &e)
    {
        response["ok"] = false;
        response["message"] = e.what();
    }
    return response;
}
} // namespace LocalTts
