#pragma once
#include "config/ime_config.h"
#include "utils/common_utils.h"
#include <Windows.h>
#include <commdlg.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#pragma comment(lib, "comdlg32.lib")

namespace TranslationLibrary
{
inline nlohmann::json Handle(const nlohmann::json &data, HWND owner)
{
    using nlohmann::json;
    json response = {{"type", "dictionaryLibraryResponse"},
                     {"protocolVersion", 1},
                     {"requestId", data.at("requestId")},
                     {"ok", false},
                     {"message", ""},
                     {"books", json::array()}};
    try
    {
        const auto root = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"translation-library";
        std::ifstream input(root / L"catalog.json");
        auto books = json::parse(input);
        if (!books.is_array() || books.size() > 200)
            throw std::runtime_error("catalog");
        const auto disabled = "," + *GetConfiguredDisabledTranslationDictionaries() + ",";
        for (auto &book : books)
        {
            const auto id = book.at("id").get<std::string>();
            if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
                throw std::runtime_error("id");
            book["enabled"] = disabled.find("," + id + ",") == std::string::npos;
        }
        if (data.at("action") == "export")
        {
            const auto id = data.value("id", "");
            auto found = std::find_if(books.begin(), books.end(), [&](const json &b) { return b.at("id") == id; });
            if (found == books.end())
                throw std::runtime_error("missing book");
            wchar_t filename[32768]{};
            const auto suggested = std::wstring(id.begin(), id.end()) + L".tsv";
            wcscpy_s(filename, suggested.c_str());
            OPENFILENAMEW dialog{sizeof(dialog)};
            dialog.hwndOwner = owner;
            dialog.lpstrFile = filename;
            dialog.nMaxFile = 32768;
            dialog.lpstrFilter = L"UTF-8 TSV 词典\0*.tsv\0所有文件\0*.*\0";
            dialog.lpstrDefExt = L"tsv";
            dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetSaveFileNameW(&dialog))
            {
                const auto source = root / suggested;
                if (std::filesystem::weakly_canonical(source) == std::filesystem::weakly_canonical(filename))
                    throw std::runtime_error("cannot overwrite installed dictionary");
                // Stage in the destination directory, then replace atomically.
                // A failed copy must never truncate an existing user export.
                auto temporary = std::filesystem::path(filename);
                temporary += L".msime-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
                if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE))
                    throw std::runtime_error("export");
                if (!MoveFileExW(temporary.c_str(), filename, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    DeleteFileW(temporary.c_str());
                    throw std::runtime_error("export replace");
                }
                response["message"] = "词典已导出（UTF-8 TSV，包含来源和许可说明）。";
            }
            else if (CommDlgExtendedError())
                throw std::runtime_error("dialog");
            else
                response["message"] = "已取消导出。";
        }
        response["books"] = std::move(books);
        response["ok"] = true;
    }
    catch (...)
    {
        response["message"] = "离线词典目录无法读取或导出失败，请检查安装文件和保存位置。";
    }
    return response;
}
} // namespace TranslationLibrary
