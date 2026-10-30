#pragma once

// Small JSON documents that hold personal content (contact notes, recognized text). They are
// encrypted with DPAPI for the current Windows account, written through a temp file and renamed,
// and guarded by a named mutex so the Server and Settings processes never interleave writes.

#include <Windows.h>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#pragma comment(lib, "crypt32.lib")

namespace ProtectedFile
{
class NamedLock
{
  public:
    explicit NamedLock(const wchar_t *name)
    {
        handle_ = CreateMutexW(nullptr, FALSE, name);
        if (handle_)
        {
            const DWORD rc = WaitForSingleObject(handle_, 3000);
            locked_ = rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED;
        }
    }
    ~NamedLock()
    {
        if (locked_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }
    NamedLock(const NamedLock &) = delete;
    NamedLock &operator=(const NamedLock &) = delete;
    explicit operator bool() const
    {
        return locked_;
    }

  private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

inline std::string Protect(const std::string &bytes, bool encrypt, const wchar_t *description)
{
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data()))};
    DATA_BLOB output{};
    const BOOL ok =
        encrypt ? CryptProtectData(&input, description, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
                : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok)
        throw std::runtime_error("无法读取加密数据，请使用保存时的 Windows 用户账户。");
    std::string result(reinterpret_cast<char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

// A missing file reads as an empty optional; a damaged one throws so callers never overwrite it.
inline std::optional<nlohmann::json> Read(const std::filesystem::path &path, const wchar_t *description,
                                          size_t limit = 16u * 1024 * 1024)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return std::nullopt;
    if (std::filesystem::file_size(path, error) > limit)
        throw std::runtime_error("数据文件过大，已停止读取。");
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    if (!in.good() && !in.eof())
        throw std::runtime_error("读取数据文件失败。");
    return nlohmann::json::parse(Protect(bytes, false, description));
}

inline void Write(const std::filesystem::path &path, const nlohmann::json &document, const wchar_t *description)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto encrypted = Protect(document.dump(), true, description);
    auto temp = path;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(encrypted.data(), static_cast<std::streamsize>(encrypted.size()));
        out.flush();
        if (!out)
            throw std::runtime_error("保存失败，原文件未被覆盖。");
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temp, error);
        throw std::runtime_error("替换数据文件失败，原文件未被覆盖。");
    }
}
} // namespace ProtectedFile
