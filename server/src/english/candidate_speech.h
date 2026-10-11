#pragma once
#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/candidate_translation_policy.h"
#include "utils/common_utils.h"
#include <sapi.h>
#include <wrl/client.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#pragma comment(lib, "sapi.lib")

namespace CandidateSpeech
{
inline std::string Text(const CandidateViewItem &item, bool translation)
{
    if (!translation)
        return item.text;
    auto senses = FanyImeIpc::SplitTranslationGloss(item.translation);
    std::string text;
    for (size_t i = 0; i < (std::min)(size_t{2}, senses.size()); ++i)
    {
        if (!text.empty())
            text += ". ";
        text += senses[i];
    }
    return text;
}
// One bounded latest-request slot. COM and synthesis stay off the input/UI thread.
// Windows installed SAPI voices are used locally; no text is sent to a provider.
class Speaker
{
  public:
    Speaker() : thread_([this] { Run(); })
    {
    }
    ~Speaker()
    {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }
    void Speak(std::wstring text, std::wstring language)
    {
        {
            std::lock_guard lock(mutex_);
            text_ = std::move(text);
            language_ = std::move(language);
            pending_ = true;
        }
        cv_.notify_one();
    }

  private:
    void Run()
    {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com))
            return;
        {
            Microsoft::WRL::ComPtr<ISpVoice> voice;
            CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&voice));
            for (;;)
            {
                std::wstring text, language;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [&] { return stop_ || pending_; });
                    if (stop_)
                        break;
                    text = std::move(text_);
                    language = std::move(language_);
                    pending_ = false;
                }
                if (!voice)
                {
                    MessageBeep(MB_ICONWARNING);
                    continue;
                }
                Microsoft::WRL::ComPtr<ISpObjectToken> token;
                Microsoft::WRL::ComPtr<ISpObjectTokenCategory> category;
                Microsoft::WRL::ComPtr<IEnumSpObjectTokens> tokens;
                const bool found =
                    SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&category))) &&
                    SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE)) &&
                    SUCCEEDED(category->EnumTokens((L"Language=" + language).c_str(), nullptr, &tokens)) &&
                    tokens->Next(1, &token, nullptr) == S_OK;
                if (!found)
                {
                    MessageBeep(MB_ICONWARNING);
                    continue;
                }
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                voice->SetVoice(token.Get());
                voice->Speak(text.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
            }
            if (voice)
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
        }
        CoUninitialize();
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false, pending_ = false;
    std::wstring text_, language_;
    std::thread thread_;
};
inline void Speak(size_t index, bool translation)
{
    const auto page = Global::LoadCandidatePageSnapshot();
    if (!page || index >= page->page_views.size())
        return;
    auto text = Text(page->page_views[index], translation);
    if (text.empty() || text.size() > 2000)
        return;
    std::wstring lang = L"804";
    const auto &source = page->page_views[index].text;
    const bool sourceAscii = std::all_of(source.begin(), source.end(), [](unsigned char c) { return c < 128; });
    if (!translation && sourceAscii)
        lang = L"409";
    else if (translation && !sourceAscii)
    {
        const auto target = GetConfiguredTencentTmt().target_language;
        if (target == "en")
            lang = L"409";
        else if (target == "ja")
            lang = L"411";
        else if (target == "es")
            lang = L"40A";
        else if (target == "vi")
            lang = L"42A";
        else if (target == "fr")
            lang = L"40C";
        else if (target == "de")
            lang = L"407";
        else if (target == "ru")
            lang = L"419";
        else if (target == "ko")
            lang = L"412";
    }
    static Speaker speaker;
    speaker.Speak(string_to_wstring(text), lang);
}
} // namespace CandidateSpeech
