#include "english/candidate_reading.h"
#include "settings/dictionary_manager.h"
#include <cpp-pinyin/G2pglobal.h>
#include <cpp-pinyin/Pinyin.h>
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
namespace CandidatePhonetics
{
Pinyin::Pinyin &Engine()
{
    static std::once_flag once;
    static std::unique_ptr<Pinyin::Pinyin> engine;
    std::call_once(once, [] {
        wchar_t name[32768]{};
        DWORD n = GetModuleFileNameW(nullptr, name, 32768);
        if (!n || n >= 32768)
            throw std::runtime_error("无法定位拼音词典");
        Pinyin::setDictionaryPath(std::filesystem::path(name).parent_path() / "dict");
        engine = std::make_unique<Pinyin::Pinyin>();
    });
    return *engine;
}
} // namespace CandidatePhonetics
namespace SettingsDictionary
{
std::string CandidateReading(const std::string &word)
{
    if (word.empty() || word.size() > 1500 ||
        std::all_of(word.begin(), word.end(), [](unsigned char c) { return c < 128; }))
        return {};
    static std::mutex mutex;
    static std::map<std::string, std::string> cache;
    std::lock_guard lock(mutex);
    if (const auto it = cache.find(word); it != cache.end())
        return it->second;
    std::string reading;
    try
    {
        const auto results = CandidatePhonetics::Engine().hanziToPinyin(word, Pinyin::ManTone::Style::TONE,
                                                                        Pinyin::Error::Default, false, true, false);
        bool recognized = false;
        for (const auto &part : results)
        {
            if (!part.error)
                recognized = true;
            if (!reading.empty())
                reading += " ";
            reading += part.error ? part.hanzi : part.pinyin;
        }
        if (!recognized)
            reading.clear();
    }
    catch (...)
    {
        reading.clear();
    }
    if (cache.size() >= 1024)
        cache.clear();
    cache.emplace(word, reading);
    return reading;
}

} // namespace SettingsDictionary
