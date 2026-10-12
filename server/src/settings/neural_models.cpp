#include "neural_models.h"
#include "config/ime_config.h"
#include "engine/core/data_path.h"
#include <ShObjIdl.h>
#include <fstream>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace NeuralModels
{
namespace
{
using nlohmann::json;
struct Entry
{
    const char *id;
    const char *name;
    const char *file;
    const char *capability;
    uint64_t bytes;
    const char *url;
    const char *license;
    const char *note;
};
const Entry catalog[] = {
    {"desktop", "水杉神经整句 · 效果优先", "sentence-model-desktop.safetensors", "sentence", 25480184,
     "https://github.com/metasequoiaime/chinese-ime-lm/releases/download/model-v1/sentence-model-desktop.safetensors",
     "Apache-2.0", "约 24.30 MiB；原数据目录模型保留；与现有神经整句引擎兼容"},
    {"tongbian", "含章·通变 · 拼音生成", "hanzhang-tongbian-small.qjm", "p2c", 143814324,
     "https://github.com/qingjian-team/qingjian/releases/download/data-v6/qingjian-data.tar.gz", "GPL-3.0-or-later",
     "约 137.15 MiB 官方资料包（含其它数据），仅提取所选模型；模型实际大小完成后显示。只支持拼音生成"},
    {"zhiwei", "含章·知微 · 候选重排", "hanzhang-zhiwei-small.qjm", "rerank", 143814324,
     "https://github.com/qingjian-team/qingjian/releases/download/data-v6/qingjian-data.tar.gz", "GPL-3.0-or-later",
     "同一资料包约 137.15 MiB，仅提取所选模型。只重排已有完整候选，不能生成问答或续写"}};
std::mutex mutex;
std::string pendingModel;
AiAssistantConfig beforeStart;
const Entry &Find(const std::string &id)
{
    for (const auto &row : catalog)
        if (id == row.id)
            return row;
    throw std::runtime_error("未知本地整句模型");
}
std::filesystem::path Root()
{
    return metasequoia::data_directory() / "models" / "neural";
}
std::filesystem::path Path(const Entry &e)
{
    return Root() / e.id / e.file;
}
json Read(const std::filesystem::path &p)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || ec || std::filesystem::file_size(p, ec) > 256 * 1024)
        return json::object();
    std::ifstream f(p);
    auto value = json::parse(f, nullptr, false);
    return value.is_object() ? value : json::object();
}
std::wstring Quote(const std::filesystem::path &p)
{
    auto s = p.wstring();
    if (s.find(L'"') != std::wstring::npos)
        throw std::runtime_error("路径无效");
    return L"\"" + s + L"\"";
}
void Launch(const std::string &action, const Entry &e, const std::filesystem::path &source)
{
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    const auto runtime = std::filesystem::path(module).parent_path() / L"local-asr";
    const auto exe = runtime / L"MetasequoiaLocalASR.exe";
    if (!std::filesystem::is_regular_file(exe))
        throw std::runtime_error("本地运行组件缺失，请安装完整新版安装包");
    std::filesystem::create_directories(Root());
    auto cmd = Quote(exe) + L" --neural --root " + Quote(Root()) + L" --action " +
               std::wstring(action.begin(), action.end()) + L" --id " + std::wstring(e.id, e.id + strlen(e.id));
    if (!source.empty())
        cmd += L" --source " + Quote(source);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, runtime.c_str(),
                        &startup, &process))
        throw std::runtime_error("无法启动本地整句模型组件");
    const auto statePath =
        (action == "serve" || action == "switch") ? Root() / "service.json" : Path(e).parent_path() / "state.json";
    json state = {{"state", action == "serve" || action == "switch" ? "loading"
                            : action == "download"                  ? "downloading"
                                                                    : "importing"},
                  {"model", e.id},
                  {"pid", process.dwProcessId},
                  {"message", action == "serve" || action == "switch" ? "正在准备本地整句服务" : "正在准备所选模型"}};
    const auto temp = statePath.wstring() + L".tmp";
    std::filesystem::create_directories(statePath.parent_path());
    {
        std::ofstream out(temp);
        out << state.dump();
    }
    MoveFileExW(temp.c_str(), statePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
bool Running(const json &state)
{
    auto pid = state.find("pid");
    if (pid == state.end() || !pid->is_number_unsigned())
        return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid->get<DWORD>());
    if (!p)
        return false;
    DWORD code = 0;
    const bool active = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return active;
}
void StopService()
{
    std::filesystem::create_directories(Root());
    std::ofstream stop(Root() / "stop");
    stop << "stop";
    pendingModel.clear();
}
void Activate(const std::string &id)
{
    const auto state = Read(Root() / "service.json");
    if (state.value("state", "") != "running" || state.value("model", "") != id || !Running(state))
        throw std::runtime_error("模型尚未就绪，请等本地服务加载完成");
    std::ifstream input(Root() / "service-token.txt");
    std::string token;
    std::getline(input, token);
    if (token.size() < 16 || token.size() > 256 || token.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("本地模型认证未准备好");
    // Explicit start/use only. Provider slots preserve the previous cloud/Custom values.
    if (!SetConfiguredLocalAiConnection("http://127.0.0.1:18769/v1/chat/completions", id, token, true))
        throw std::runtime_error("模型已启动，但配置保存失败，请重试“使用模型”");
}
} // namespace
std::string EffectiveDesktopModelPath()
{
    const auto managed = Path(Find("desktop"));
    std::error_code ec;
    if (std::filesystem::is_regular_file(managed, ec) && !ec)
        return metasequoia::path_to_utf8(managed);
    // Empty means the engine uses its established legacy fallback path.
    return {};
}
std::filesystem::path PromptForModelFile(HWND owner, const std::string &id)
{
    Find(id);
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::filesystem::path selected;
    IFileOpenDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        const COMDLG_FILTERSPEC qjm[] = {{L"青简含章模型 (*.qjm)", L"*.qjm"}},
                                desktop[] = {{L"水杉效果优先模型 (*.safetensors)", L"*.safetensors"}};
        dialog->SetFileTypes(1, id == "desktop" ? desktop : qjm);
        dialog->SetTitle(L"选择用户自行下载的模型（原文件保留）");
        DWORD opts = 0;
        if (SUCCEEDED(dialog->GetOptions(&opts)))
            dialog->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return selected;
}
json Handle(const json &request, const std::filesystem::path &source)
{
    std::lock_guard lock(mutex);
    json response = {{"type", "neuralModelsResult"},
                     {"requestId", request.value("requestId", "")},
                     {"ok", true},
                     {"message", ""},
                     {"models", json::array()},
                     {"service", json::object()}};
    try
    {
        const std::string action = request.value("action", "status"), id = request.value("id", "");
        if (action != "status")
        {
            const auto &e = Find(id);
            const auto current = Read(Root() / "service.json");
            const bool busy = Running(current) &&
                              (current.value("state", "") == "loading" || current.value("state", "") == "running");
            if (action == "download" || action == "import")
            {
                auto state = Read(Path(e).parent_path() / "state.json");
                if (state.value("state", "") == "downloading" || state.value("state", "") == "importing")
                    throw std::runtime_error("已有下载或导入任务正在运行");
                if (action == "import" && source.empty())
                    response["message"] = "已取消导入";
                else
                {
                    Launch(action, e, source);
                    response["message"] = "已开始校验或下载；只安装所选模型";
                }
            }
            else if (action == "cancel")
            {
                std::filesystem::create_directories(Path(e).parent_path());
                std::ofstream cancel(Path(e).parent_path() / "cancel");
                cancel << "cancel";
                response["message"] = "已请求取消下载，原文件保留";
            }
            else if (action == "delete")
            {
                if (busy && current.value("model", "") == id)
                    throw std::runtime_error("请先停止当前模型再删除");
                std::error_code ec;
                std::filesystem::remove(Path(e), ec);
                if (ec)
                    throw std::runtime_error("模型仍被占用，请停止或重启输入法后再删除");
                std::filesystem::remove(Path(e).parent_path() / "state.json", ec);
                if (id == "desktop")
                    SetConfiguredAssocSentenceNeuralDesktop(false);
                response["message"] = "已删除所选托管模型；原数据目录模型、备份和配置保留";
            }
            else if (action == "stop")
            {
                if (id == "desktop")
                {
                    if (!SetConfiguredAssocSentenceNeuralDesktop(false))
                        throw std::runtime_error("停用效果优先模型的配置保存失败");
                    response["message"] = "已停用效果优先模型，文件保留";
                }
                else
                {
                    StopService();
                    response["message"] = "已请求停止本地整句服务";
                }
            }
            else if (action == "activate")
            {
                if (id == "desktop")
                {
                    if (EffectiveDesktopModelPath().empty() &&
                        !std::filesystem::is_regular_file(metasequoia::data_directory() / e.file))
                        throw std::runtime_error("请先下载或导入模型");
                    SetConfiguredAssocSentenceNeuralDesktop(true);
                }
                else
                    Activate(id);
                response["message"] = "正在使用";
            }
            else if (action == "start")
            {
                if (id == "desktop")
                {
                    if (EffectiveDesktopModelPath().empty() &&
                        !std::filesystem::is_regular_file(metasequoia::data_directory() / e.file))
                        throw std::runtime_error("请先下载或导入模型");
                    SetConfiguredAssocSentenceNeuralDesktop(true);
                    response["message"] = "正在使用效果优先模型";
                }
                else if (busy)
                {
                    if (current.value("model", "") == id && current.value("state", "") == "running")
                    {
                        StopService();
                        response["message"] = "已请求停止";
                    }
                    else
                    {
                        beforeStart = GetConfiguredAiAssistantSnapshot();
                        Launch("switch", e, {});
                        pendingModel = id;
                        response["message"] = "正在停止旧模型并切换到所选模型";
                    }
                }
                else
                {
                    if (!std::filesystem::is_regular_file(Path(e)))
                        throw std::runtime_error("请先按需下载或导入模型");
                    beforeStart = GetConfiguredAiAssistantSnapshot();
                    Launch("serve", e, {});
                    pendingModel = id;
                    response["message"] = "正在加载；首次启动可能下载必要运行依赖，不会下载权重";
                }
            }
            else
                throw std::runtime_error("未知本地模型操作");
        }
        auto service = Read(Root() / "service.json");
        if ((service.value("state", "") == "loading" || service.value("state", "") == "running") && !Running(service))
        {
            service["state"] = "stopped";
            service["message"] = "本地模型进程已退出";
        }
        if (!pendingModel.empty() && service.value("state", "") == "running" &&
            service.value("model", "") == pendingModel)
        {
            const auto now = GetConfiguredAiAssistantSnapshot();
            if (now.provider == beforeStart.provider && now.model == beforeStart.model &&
                now.endpoint == beforeStart.endpoint && now.token == beforeStart.token &&
                now.enabled == beforeStart.enabled && now.prompt == beforeStart.prompt)
            {
                Activate(pendingModel);
                response["message"] = "已启动并使用";
            }
            else
                response["message"] = "服务已就绪；检测到连接设置已改变，请自行点击“使用模型”";
            pendingModel.clear();
        }
        const auto activeAi = GetConfiguredAiAssistantSnapshot();
        for (const auto &e : catalog)
        {
            const auto file = Path(e);
            auto state = Read(file.parent_path() / "state.json");
            std::error_code ec;
            const bool installed = std::filesystem::is_regular_file(file, ec) && !ec;
            const auto old = metasequoia::data_directory() / e.file;
            const bool legacy =
                std::string(e.id) == "desktop" && !installed && std::filesystem::is_regular_file(old, ec) && !ec;
            std::string status = state.value("state", installed ? "ready" : legacy ? "legacy" : "absent");
            if (status == "ready" && !installed)
                status = "absent";
            uint64_t size = (installed || legacy) ? std::filesystem::file_size(installed ? file : old, ec) : 0;
            response["models"].push_back(
                {{"id", e.id},
                 {"name", e.name},
                 {"state", status},
                 {"bytes", size},
                 {"downloadBytes", e.bytes},
                 {"url", e.url},
                 {"license", e.license},
                 {"capability", e.capability},
                 {"note", e.note},
                 {"message", state.value("message", "")},
                 {"downloaded", state.value("downloaded", uint64_t(0))},
                 {"total", state.value("total", e.bytes)},
                 {"legacy", legacy},
                 {"active", std::string(e.id) == "desktop"
                                ? GetConfiguredAssocSentenceNeuralDesktop() && (installed || legacy)
                                : activeAi.enabled && activeAi.provider == "local" && activeAi.model == e.id &&
                                      activeAi.endpoint == "http://127.0.0.1:18769/v1/chat/completions" &&
                                      service.value("state", "") == "running" && service.value("model", "") == e.id}});
        }
        // Never include the token in this status response.
        response["service"] = {{"state", service.value("state", "stopped")},
                               {"model", service.value("model", "")},
                               {"message", service.value("message", "")},
                               {"device", service.value("device", "")}};
    }
    catch (const std::exception &e)
    {
        response["ok"] = false;
        response["message"] = e.what();
    }
    return response;
}
} // namespace NeuralModels
