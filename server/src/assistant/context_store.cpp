#include "context_store.h"
#include "protocol.h"
#include <Windows.h>
#include <wincrypt.h>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#pragma comment(lib, "crypt32.lib")
namespace ReplyStudio
{
using nlohmann::json;
namespace
{
struct Guard
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\MetasequoiaIme.ReplyContexts");
    Guard()
    {
        if (!mutex)
            throw std::runtime_error("无法锁定会话存储。");
        auto rc = WaitForSingleObject(mutex, 3000);
        if (rc != WAIT_OBJECT_0 && rc != WAIT_ABANDONED)
        {
            CloseHandle(mutex);
            mutex = nullptr;
            throw std::runtime_error("会话存储正忙，请重试。");
        }
    }
    ~Guard()
    {
        if (mutex)
        {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    }
};
std::string Protect(const std::string &bytes, bool encrypt)
{
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data()))},
        output{};
    const BOOL ok =
        encrypt
            ? CryptProtectData(&input, L"水杉回复会话", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
            : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok)
        throw std::runtime_error("无法读取加密会话，请使用保存时的 Windows 用户账户。");
    std::string result(reinterpret_cast<char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
} // namespace
json ContextStore::Apply(const json &request, int64_t now)
{
    Validate(request);
    Guard guard;
    json rows = json::array();
    if (std::filesystem::exists(path_))
    {
        if (std::filesystem::file_size(path_) > 8 * 1024 * 1024)
            throw std::runtime_error("会话文件过大，已停止读取。");
        std::ifstream in(path_, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        if (!in.good() && !in.eof())
            throw std::runtime_error("读取会话失败。");
        auto data = json::parse(Protect(bytes, false));
        if (data.at("version") != 1 || !data.at("sessions").is_array() || data.at("sessions").size() > 50)
            throw std::runtime_error("会话文件版本或内容无效。");
        rows = data.at("sessions");
    }
    const auto before = rows.size();
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [now](const json &row) {
                                  return now - row.at("updated").get<int64_t>() >
                                         int64_t(row.value("retentionDays", 90)) * 86400000;
                              }),
               rows.end());
    bool changed = before != rows.size();
    const auto action = request.at("action").get<std::string>(), id = request.value("sessionId", "");
    auto found = std::find_if(rows.begin(), rows.end(), [&](const json &row) { return row.at("sessionId") == id; });
    json result = json::object();
    if (action == "save")
    {
        if (found == rows.end() && rows.size() >= 50)
            throw std::runtime_error("最多保存 50 个会话，请先删除不需要的会话。");
        json row = {{"kind", request.value("kind", "person")},
                    {"platform", request.value("platform", "")},
                    {"sessionId", id},
                    {"title", request.value("title", "")},
                    {"context", request.value("context", "")},
                    {"summary", request.value("summary", "")},
                    {"updated", now},
                    {"retentionDays", request.value("retentionDays", 90)}};
        if (found == rows.end())
            rows.push_back(row);
        else
            *found = row;
        result["session"] = row;
        changed = true;
    }
    else if (action == "load")
    {
        if (found == rows.end())
            throw std::runtime_error("会话不存在或已到保留期限。");
        result["session"] = *found;
    }
    else if (action == "delete")
    {
        if (found != rows.end())
        {
            rows.erase(found);
            changed = true;
        }
    }
    if (changed)
    {
        std::filesystem::create_directories(path_.parent_path());
        auto encrypted = Protect(json{{"version", 1}, {"sessions", rows}}.dump(), true);
        auto temp = path_;
        temp += L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(encrypted.data(), encrypted.size());
            out.flush();
            if (!out)
                throw std::runtime_error("保存会话失败，原文件未被覆盖。");
        }
        if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temp);
            throw std::runtime_error("替换会话文件失败，原文件未被覆盖。");
        }
    }
    std::sort(rows.begin(), rows.end(), [](const json &a, const json &b) {
        return a.at("updated").get<int64_t>() > b.at("updated").get<int64_t>();
    });
    result["sessions"] = json::array();
    for (const auto &row : rows)
        result["sessions"].push_back({{"kind", row.value("kind", "person")},
                                      {"platform", row.value("platform", "")},
                                      {"sessionId", row.at("sessionId")},
                                      {"title", row.at("title")},
                                      {"updated", row.at("updated")}});
    return result;
}
} // namespace ReplyStudio
