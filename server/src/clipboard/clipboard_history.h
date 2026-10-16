#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace ClipboardHistory
{
// Upper bound only; the user's limit comes from [clipboard] max_items.
constexpr size_t kMaxItems = 500;
constexpr size_t kMaxChars = 4000;
constexpr uint32_t kMaxCopies = 2147483647;
struct Entry
{
    std::wstring text;
    uint32_t copies = 1;
    bool pinned = false;
    // Milliseconds since the Unix epoch of the latest copy; 0 for records written before timestamps existed.
    int64_t at = 0;
    // File name of the program the text was copied from, empty when unknown.
    std::wstring app;
};

// [clipboard] settings read straight from config.toml, so the Emoji panel process needs no config module.
struct Settings
{
    bool sortRecent = true;
    size_t maxItems = 200;
    int retentionDays = 30;
    bool respectPrivacyFlag = true;
    bool pauseOnLock = true;
    std::vector<std::wstring> excludedApps;
};
Settings LoadSettings();

std::filesystem::path StorePath();
std::vector<std::wstring> Load();
std::vector<Entry> LoadEntries();
// A nonzero clipboard sequence identifies one OS copy event across the Server and Settings monitors.
bool AddText(std::wstring text, uint32_t clipboardSequence = 0, const std::wstring &app = {});
bool SetPinned(const std::wstring &text, bool pinned);
bool RemoveText(const std::wstring &text);
// Edits a saved record without copying it or incrementing its counter; collisions merge counters.
bool UpdateEntryText(const std::wstring &previous, std::wstring text);
void Clear();
bool TryClear();
bool IsEnabled();
bool SetEnabled(bool enabled);
std::filesystem::file_time_type StoreWriteTime();
std::filesystem::file_time_type ConfigWriteTime();
} // namespace ClipboardHistory

namespace ClipboardMonitor
{
void Start();
void Stop();
void Sync(bool enabled);
bool IsRunning();
// Copies made by the quick actions themselves (borrowing the clipboard to read a selection) are not history.
void SuppressFor(unsigned milliseconds);
} // namespace ClipboardMonitor
