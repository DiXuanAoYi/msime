#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace ClipboardHistory
{
constexpr size_t kMaxItems = 50;
constexpr size_t kMaxChars = 4000;
constexpr uint32_t kMaxCopies = 2147483647;
struct Entry
{
    std::wstring text;
    uint32_t copies = 1;
};

std::filesystem::path StorePath();
std::vector<std::wstring> Load();
std::vector<Entry> LoadEntries();
// A nonzero clipboard sequence identifies one OS copy event across the Server and Settings monitors.
bool AddText(std::wstring text, uint32_t clipboardSequence = 0);
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
} // namespace ClipboardMonitor
