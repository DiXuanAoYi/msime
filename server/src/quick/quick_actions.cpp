#include "quick/quick_actions.h"
#include "assistant/chat_reader.h"
#include "assistant/profile_store.h"
#include "assistant/service.h"
#include "clipboard/clipboard_history.h"
#include "config/ime_config.h"
#include "quick/list_panel.h"
#include "quick/quick_thread.h"
#include "quick/text_io.h"
#include "settings/feature_config.h"
#include "settings/personal_library.h"
#include "settings/vision_capture.h"
#include "settings/vision_config.h"
#include "settings/vision_protocol.h"
#include "settings/vision_service.h"
#include "utils/common_utils.h"
#include "utils/hotkey_combo.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace Quick::Actions
{
namespace
{
using nlohmann::json;

constexpr UINT_PTR kTimerHotkeys = 1;
constexpr UINT_PTR kTimerLive = 2;

std::wstring Wide(const std::string &text)
{
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string Utf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Clip(const std::wstring &text, size_t limit)
{
    std::wstring flat;
    for (wchar_t c : text)
        flat.push_back(c == L'\r' || c == L'\n' || c == L'\t' ? L' ' : c);
    return flat.size() > limit ? flat.substr(0, limit) + L"…" : flat;
}

struct ComScope
{
    ComScope()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ~ComScope()
    {
        CoUninitialize();
    }
};

// Runs fn on a short-lived thread. Results go back through Post; a stale generation is simply dropped.
template <class F> void Work(F fn)
{
    std::thread([fn = std::move(fn)]() mutable {
        ComScope com;
        try
        {
            fn();
        }
        catch (...)
        {
        }
    }).detach();
}

bool g_hotkeys = false;
bool g_picking = false;
uint64_t g_registeredRevision = ~0ull;
std::map<int, std::string> g_registered;
std::unique_ptr<ListPanel> g_panel;
std::atomic<uint64_t> g_generation{0};
RECT g_liveAnchor{};

uint64_t NextGeneration()
{
    return ++g_generation;
}

std::function<bool()> CancelledFor(uint64_t generation)
{
    return [generation] { return g_generation.load() != generation; };
}

ListPanel &Panel()
{
    if (!g_panel)
        g_panel = std::make_unique<ListPanel>();
    return *g_panel;
}

void CloseAll()
{
    NextGeneration();
    KillTimer(Controller(), kTimerLive);
    if (g_panel)
        g_panel->Close();
    Vision::HideTranslationOverlay();
}

std::filesystem::path StatusPath()
{
    return std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"quick-hotkeys.json";
}

// ----------------------------------------------------------------------------------- hotkeys

void RegisterAll()
{
    const HWND window = Controller();
    for (const auto &[id, action] : g_registered)
        UnregisterHotKey(window, id);
    g_registered.clear();
    g_registeredRevision = FeatureConfig::Revision();
    const auto values = FeatureConfig::Get("hotkeys");
    json report = json::object();
    size_t count = 0;
    const char *const *actions = FeatureConfig::HotkeyActions(count);
    const bool enabled = values.count("enabled") && values.at("enabled") == "true";
    for (size_t i = 0; i < count; ++i)
    {
        const std::string action = actions[i];
        const auto found = values.find(action);
        const std::string text = found == values.end() ? std::string() : found->second;
        json row = {{"combo", text}, {"ok", false}, {"error", ""}};
        if (text.empty())
            row["error"] = "未设置";
        else if (!enabled)
            row["error"] = "全局快捷键已关闭";
        else if (const auto combo = HotkeyCombo::Parse(text))
        {
            const int id = static_cast<int>(i) + 1;
            if (RegisterHotKey(window, id, combo->modifiers | MOD_NOREPEAT, combo->vk))
            {
                g_registered[id] = action;
                row["ok"] = true;
            }
            else
                row["error"] = GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED ? "已被其他程序占用" : "注册失败";
        }
        else
            row["error"] = "格式无效";
        report[action] = row;
    }
    try
    {
        std::ofstream out(StatusPath(), std::ios::binary | std::ios::trunc);
        out << json{{"hotkeys", report}}.dump();
    }
    catch (...)
    {
    }
}

// ----------------------------------------------------------------------------------- shared helpers

RECT AnchorNow()
{
    RECT rect{};
    if (TextIo::FocusRect(rect))
        return rect;
    POINT cursor{};
    GetCursorPos(&cursor);
    return {cursor.x, cursor.y, cursor.x + 2, cursor.y + 20};
}

void ShowCard(const RECT &anchor, const std::wstring &title, const std::wstring &source, const std::wstring &text,
              const std::wstring &note, const std::wstring &hint, std::function<void()> onPick = {}, bool below = false)
{
    PanelModel model;
    model.title = title;
    model.source = source;
    if (!text.empty())
        model.items.push_back({L"", text, L"", false});
    model.note = note;
    model.hint = hint;
    model.below = below;
    PanelHandlers handlers;
    handlers.onPick = [onPick](int) {
        if (onPick)
            onPick();
        CloseAll();
    };
    handlers.onClose = [] { CloseAll(); };
    Panel().Open(anchor, std::move(handlers), std::move(model));
}

bool SetClipboardImage(const Vision::Image &image)
{
    const size_t stride = static_cast<size_t>(image.width) * 4;
    const size_t bytes = sizeof(BITMAPINFOHEADER) + stride * image.height;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
        return false;
    auto *target = static_cast<unsigned char *>(GlobalLock(memory));
    if (!target)
    {
        GlobalFree(memory);
        return false;
    }
    auto *header = reinterpret_cast<BITMAPINFOHEADER *>(target);
    *header = {};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(image.width);
    header->biHeight = static_cast<LONG>(image.height); // bottom-up: the form every program understands
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    for (unsigned row = 0; row < image.height; ++row)
        memcpy(target + sizeof(BITMAPINFOHEADER) + stride * (image.height - 1 - row),
               image.pixels.data() + stride * row, stride);
    GlobalUnlock(memory);
    ClipboardMonitor::SuppressFor(1500);
    bool ok = false;
    for (int i = 0; i < 20 && !ok; ++i)
    {
        if (OpenClipboard(nullptr))
        {
            EmptyClipboard();
            ok = SetClipboardData(CF_DIB, memory) != nullptr;
            CloseClipboard();
            break;
        }
        Sleep(10);
    }
    if (!ok)
        GlobalFree(memory);
    return ok;
}

bool LooksChinese(const std::wstring &text)
{
    size_t cjk = 0, letters = 0;
    for (wchar_t c : text)
    {
        if (c >= 0x4E00 && c <= 0x9FFF)
            ++cjk;
        if (iswalpha(c))
            ++letters;
    }
    return letters && cjk * 3 >= letters;
}

std::string ChooseTarget(const std::wstring &text)
{
    const auto config = VisionConfig::Get();
    if (config.at("auto_swap") == "true")
        return LooksChinese(text) ? config.at("zh_target") : std::string("zh");
    return config.at("target_language");
}

// ----------------------------------------------------------------------------------- screen actions

void AfterCapture(const std::string &kind, RECT region);

void PickAndRun(const std::string &kind)
{
    if (g_picking)
        return;
    CloseAll();
    g_picking = true;
    RECT region{};
    try
    {
        region = Vision::PickRectangle(nullptr);
    }
    catch (const std::exception &)
    {
        region = {};
    }
    g_picking = false;
    if (region.right <= region.left)
        return;
    AfterCapture(kind, region);
}

void AfterCapture(const std::string &kind, RECT region)
{
    const uint64_t generation = NextGeneration();
    const auto cancelled = CancelledFor(generation);
    const auto config = VisionConfig::Get();
    const RECT anchor{region.left, region.bottom, region.right, region.bottom};
    if (kind == "live")
    {
        if (!Vision::StartLive(region, config.at("source_language"), config.at("target_language"), "document"))
        {
            ShowCard(anchor, L"实时翻译", L"", L"", L"另一个截图任务还在运行，请稍后再试。", L"Esc 关闭");
            return;
        }
        PanelModel model;
        model.title = L"实时翻译";
        model.note = L"画面变化时自动刷新译文。按 Esc 停止，或点右上角 ×。";
        model.hint = L"Esc 停止";
        model.below = true;
        model.passive = true;
        PanelHandlers handlers;
        handlers.onClose = [] {
            Vision::Stop();
            CloseAll();
        };
        g_liveAnchor = anchor;
        Panel().Open(anchor, std::move(handlers), std::move(model));
        SetTimer(Controller(), kTimerLive, 1000, nullptr);
        return;
    }
    Work([=] {
        Sleep(160); // let the dimmed picker leave the screen before the pixels are read
        auto image = Vision::CaptureRectangle(region);
        if (cancelled())
            return;
        if (kind == "shot")
        {
            const bool ok = SetClipboardImage(image);
            Post([=] {
                if (g_generation.load() == generation)
                    ShowCard(anchor, L"截图", L"", L"", ok ? L"截图已复制到剪贴板，直接粘贴即可。" : L"复制截图失败。",
                             L"Esc 关闭", {}, true);
            });
            return;
        }
        const auto task = config.at("task");
        json result;
        std::string error;
        try
        {
            result = Vision::RecognizeImage(image, task, cancelled);
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        if (cancelled())
            return;
        if (!error.empty())
        {
            Post([=] {
                if (g_generation.load() == generation)
                    ShowCard(anchor, L"截图", L"", L"", Wide(error), L"Esc 关闭", {}, true);
            });
            return;
        }
        const auto text = Wide(result.value("text", std::string()));
        if (kind == "ocr")
        {
            if (config.at("after_ocr_copy") == "true" && !text.empty())
                TextIo::SetClipboardText(text);
            Vision::AddHistory("ocr", Utf8(text), "");
            Post([=] {
                if (g_generation.load() != generation)
                    return;
                if (config.at("after_ocr_card") != "true")
                    return;
                const bool copied = config.at("after_ocr_copy") == "true" && !text.empty();
                ShowCard(
                    anchor, L"文字识别", Clip(Wide(result.value("warning", std::string())), 60), text,
                    text.empty() ? L"没有识别到文字。" : L"",
                    copied ? L"已复制 · 1 再复制 · Esc 关闭" : L"1 复制 · Esc 关闭",
                    [text] { TextIo::SetClipboardText(text); }, true);
            });
            return;
        }
        // Screenshot translation: translate each located block and paint it over the original.
        auto blocks = Vision::OverlayBlocksForImage(result, image.width, image.height);
        std::vector<std::string> sources;
        std::set<std::string> seen;
        for (const auto &block : blocks)
            if (seen.insert(block.source).second)
                sources.push_back(block.source);
        const auto source = config.at("source_language");
        const auto target = ChooseTarget(text);
        std::string combined;
        try
        {
            const auto translated = Vision::TranslateBlocks(sources, source, target, cancelled);
            std::map<std::string, std::string> byText;
            for (size_t i = 0; i < sources.size() && i < translated.size(); ++i)
                byText[sources[i]] = translated[i];
            for (auto &block : blocks)
            {
                block.text = byText[block.source];
                if (!block.text.empty())
                    combined += (combined.empty() ? "" : "\n") + block.text;
            }
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        if (cancelled())
            return;
        Vision::AddHistory("translate", Utf8(text), combined);
        Post([=] {
            if (g_generation.load() != generation)
                return;
            if (error.empty() && !blocks.empty())
                Vision::ShowTranslationOverlay(region, blocks);
            const auto body = Wide(combined);
            ShowCard(
                anchor, L"截图翻译", L"", body,
                !error.empty() ? Wide(error)
                : body.empty() ? L"没有识别到可翻译的文字。"
                               : L"",
                L"1 复制译文 · Esc 关闭", [body] { TextIo::SetClipboardText(body); }, true);
        });
    });
}

void TranslateSelection()
{
    CloseAll();
    const uint64_t generation = NextGeneration();
    const auto cancelled = CancelledFor(generation);
    const RECT anchor = AnchorNow();
    Work([=] {
        TextIo::WaitModifiersReleased();
        auto text = TextIo::ReadSelection();
        std::string error;
        std::wstring translated;
        if (text.empty())
            error = "没有选中文字。先选中要翻译的文字，再按快捷键。";
        else
        {
            try
            {
                translated = Wide(Vision::Translate(Utf8(text), "auto", ChooseTarget(text), cancelled));
                Vision::AddHistory("selection", Utf8(text), Utf8(translated));
            }
            catch (const std::exception &e)
            {
                error = e.what();
            }
        }
        if (cancelled())
            return;
        Post([=] {
            if (g_generation.load() != generation)
                return;
            ShowCard(anchor, L"划词翻译", Clip(text, 40), translated, Wide(error), L"1 复制译文 · Esc 关闭",
                     [translated] { TextIo::SetClipboardText(translated); });
        });
    });
}

// ----------------------------------------------------------------------------------- reply / rewrite

struct ReplyTone
{
    std::string id, name, prompt;
    bool custom = false;
};

struct ReplyState
{
    std::string task; // reply | rewrite
    uint64_t generation = 0;
    std::vector<ReplyTone> tones;
    int tab = 0, defaultTab = 0;
    std::map<std::string, std::vector<PanelItem>> done;
    std::map<std::string, std::wstring> failed;
    std::set<std::string> pending;
    std::string incoming, context, relationship, persona, kind, platform, length, contactKey;
    int count = 3;
    std::vector<std::wstring> intents;
    int intent = -1;
    bool hadSelection = false;
    std::wstring source;
    std::wstring note;
    RECT anchor{};
};

ReplyState g_reply;

const char *const kIntentTexts[] = {"答应", "拒绝", "再想想", "问清楚"};

std::string FormatContext(const json &messages, size_t limit)
{
    std::string out;
    const size_t begin = messages.size() > limit ? messages.size() - limit : 0;
    for (size_t i = begin; i < messages.size(); ++i)
    {
        const auto &m = messages[i];
        const std::string who = m.value("self", false)           ? "我"
                                : m.value("speaker", "").empty() ? "对方"
                                                                 : m.value("speaker", "");
        out += who + "：" + m.value("text", "") + "\n";
    }
    return out;
}

PanelModel BuildReplyModel()
{
    auto &s = g_reply;
    PanelModel model;
    model.title = s.task == "rewrite" ? L"帮我改" : L"帮我回";
    model.source = Clip(s.source, 48);
    if (s.task == "reply")
    {
        for (size_t i = 0; i < s.tones.size(); ++i)
            model.tabs.push_back({Wide(s.tones[i].name), static_cast<int>(i) == s.defaultTab});
        model.tab = s.tab;
        for (const auto &text : s.intents)
            model.intents.push_back(text);
        model.intent = s.intent;
    }
    model.canRefresh = true;
    const std::string key = s.task == "rewrite" ? "rewrite" : (s.tones.empty() ? "" : s.tones[s.tab].id);
    if (s.pending.count(key))
        model.loading = true;
    else if (s.failed.count(key))
        model.note = s.failed[key];
    else if (s.done.count(key))
    {
        model.items = s.done[key];
        if (model.items.empty())
            model.note = L"模型没有给出可用的回复，按 R 再试一次。";
    }
    model.hint = s.task == "rewrite" ? L"1-5 替换原文 · R 重新生成 · Esc 关闭"
                                     : L"1-9 填入 · Tab 换语气 · R 重新生成 · I 我的意思 · Esc 关闭";
    model.status = FeatureConfig::Get("reply", "fill_mode") == "copy" ? L"只复制" : L"直接填入";
    if (!s.note.empty())
        model.warn = s.note;
    return model;
}

void Generate(const std::string &key, bool force);

void FillText(const std::wstring &text)
{
    const bool copyOnly = FeatureConfig::Get("reply", "fill_mode") == "copy";
    const bool replaceAll = g_reply.task == "rewrite" && !g_reply.hadSelection;
    CloseAll();
    Work([=] {
        TextIo::WaitModifiersReleased();
        if (copyOnly)
            TextIo::SetClipboardText(text);
        else
            TextIo::PasteText(text, replaceAll);
    });
}

void SwitchTone(int index)
{
    if (index < 0 || index >= static_cast<int>(g_reply.tones.size()))
        return;
    g_reply.tab = index;
    const auto key = g_reply.tones[index].id;
    if (!g_reply.done.count(key) && !g_reply.pending.count(key))
        Generate(key, false);
    Panel().Update(BuildReplyModel());
}

PanelHandlers ReplyHandlers()
{
    PanelHandlers handlers;
    handlers.onPick = [](int index) {
        const std::string key = g_reply.task == "rewrite" ? "rewrite" : g_reply.tones[g_reply.tab].id;
        const auto found = g_reply.done.find(key);
        if (found != g_reply.done.end() && index >= 0 && index < static_cast<int>(found->second.size()))
            FillText(found->second[index].text);
    };
    handlers.onTab = [](int index) { SwitchTone(index); };
    handlers.onRefresh = [] {
        const std::string key = g_reply.task == "rewrite" ? "rewrite" : g_reply.tones[g_reply.tab].id;
        Generate(key, true);
        Panel().Update(BuildReplyModel());
    };
    handlers.onIntent = [](int index) {
        g_reply.intent = index;
        g_reply.done.clear();
        const auto key = g_reply.tones[g_reply.tab].id;
        Generate(key, true);
        Panel().Update(BuildReplyModel());
    };
    handlers.onClose = [] { CloseAll(); };
    return handlers;
}

void Generate(const std::string &key, bool force)
{
    auto &s = g_reply;
    if (s.pending.count(key))
        return;
    if (force)
    {
        s.done.erase(key);
        s.failed.erase(key);
    }
    json request = {{"action", "generate"},  {"requestId", "quick-" + std::to_string(GetTickCount64())},
                    {"task", s.task},        {"incoming", s.incoming},
                    {"context", s.context},  {"relationship", s.relationship},
                    {"persona", s.persona},  {"length", s.length},
                    {"count", s.count},      {"kind", s.kind.empty() ? "person" : s.kind},
                    {"platform", s.platform}};
    if (s.task == "rewrite")
        request["mode"] = "rewrite_styles";
    else
    {
        const auto &tone = s.tones[s.tab];
        if (tone.custom)
            request["tonePrompt"] = tone.prompt;
        else
            request["mode"] = tone.id;
        if (s.intent >= 0 && s.intent < static_cast<int>(std::size(kIntentTexts)))
            request["intent"] = std::string("我想表达：") + kIntentTexts[s.intent];
    }
    if (request["platform"].get<std::string>().empty())
        request.erase("platform");
    s.pending.insert(key);
    const uint64_t generation = s.generation;
    const auto cancelled = CancelledFor(generation);
    Work([=] {
        std::vector<PanelItem> items;
        std::wstring error;
        try
        {
            const auto result = ReplyStudio::RunRequest(request, cancelled);
            for (const auto &row : result.at("replies"))
                items.push_back({Wide(row.value("label", "")), Wide(row.value("text", "")), L"", false});
        }
        catch (const std::exception &e)
        {
            error = Wide(e.what());
        }
        if (cancelled())
            return;
        Post([=] {
            if (g_generation.load() != generation)
                return;
            g_reply.pending.erase(key);
            if (!error.empty())
                g_reply.failed[key] = error;
            else
                g_reply.done[key] = items;
            if (g_panel && g_panel->IsOpen())
                Panel().Update(BuildReplyModel());
        });
    });
}

// Reads the chat the user is in: the visible bubbles, who said what, and what we already know about them.
bool ReadChat(ReplyState &s, const std::function<bool()> &cancelled, HWND foreground)
{
    const auto captured = ReplyStudio::CaptureChatWindows(cancelled);
    const json &windows = captured.is_object() && captured.contains("windows") ? captured.at("windows") : captured;
    if (!windows.is_array())
        return false;
    const std::string suffix = "-" + std::to_string(reinterpret_cast<uintptr_t>(foreground));
    for (const auto &raw : windows)
    {
        const auto id = raw.value("window", "");
        if (id.size() < suffix.size() || id.compare(id.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        auto parsed = ReplyStudio::ParseChatWindow(raw);
        if (!parsed.value("readable", false) || !parsed.contains("messages") || parsed["messages"].empty())
            return false;
        const auto &messages = parsed["messages"];
        std::string incoming;
        for (size_t i = messages.size(); i-- > 0;)
        {
            if (messages[i].value("self", false))
            {
                if (!incoming.empty())
                    break;
                continue;
            }
            incoming = messages[i].value("text", "") + (incoming.empty() ? "" : "\n") + incoming;
            if (messages.size() - i >= 3)
                break;
        }
        if (incoming.empty())
            return false;
        const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        json contact;
        try
        {
            contact = ReplyStudio::ObserveChat(parsed, now);
        }
        catch (const std::exception &)
        {
        }
        s.incoming = incoming;
        s.kind = parsed.value("kind", "");
        s.platform = parsed.value("platform", "");
        const int contextLimit = FeatureConfig::GetInt("reply", "context");
        s.context = contextLimit > 0 ? FormatContext(messages, static_cast<size_t>(contextLimit)) : std::string();
        s.source = Wide(parsed.value("title", "")) + L"：" + Wide(incoming);
        if (contact.is_object())
        {
            s.relationship = ReplyStudio::RelationshipPrompt(contact);
            s.contactKey = contact.value("id", "");
            if (!contact.value("length", "").empty())
                s.length = contact.value("length", "");
            s.persona = ReplyStudio::PersonaPrompt(ReplyStudio::LoadPersona(), contact.value("address", ""));
            const auto preferred = contact.value("tone", "");
            for (size_t i = 0; i < s.tones.size(); ++i)
                if (s.tones[i].id == preferred)
                    s.defaultTab = s.tab = static_cast<int>(i);
        }
        return true;
    }
    return false;
}

void StartAssistant(const std::string &task)
{
    CloseAll();
    auto &s = g_reply;
    s = ReplyState();
    s.task = task;
    s.generation = NextGeneration();
    s.anchor = AnchorNow();
    s.length = FeatureConfig::Get("reply", "length");
    s.count = task == "rewrite" ? 5 : std::atoi(FeatureConfig::Get("reply", "count").c_str());
    if (s.count != 5)
        s.count = 3;
    s.persona = ReplyStudio::PersonaPrompt(ReplyStudio::LoadPersona(), "");
    if (task == "reply")
    {
        for (const auto &tone : ReplyStudio::LoadTones())
        {
            if (!tone.value("on", false))
                continue;
            s.tones.push_back({tone.value("id", ""), tone.value("name", ""),
                               tone.value("prompt", tone.value("desc", "")), tone.value("custom", false)});
        }
        if (s.tones.empty())
        {
            ShowCard(s.anchor, L"帮我回", L"", L"", L"没有启用的语气。请到设置里至少打开一种语气。", L"Esc 关闭");
            return;
        }
        for (const auto &text : kIntentTexts)
            s.intents.push_back(Wide(text));
    }
    const HWND foreground = GetForegroundWindow();
    const auto cancelled = CancelledFor(s.generation);
    const uint64_t generation = s.generation;
    PanelModel loading = BuildReplyModel();
    loading.loading = true;
    loading.source = task == "rewrite" ? L"正在读取输入框…" : L"正在读取对话…";
    Panel().Open(s.anchor, ReplyHandlers(), std::move(loading));
    Work([=, read = g_reply]() mutable {
        TextIo::WaitModifiersReleased();
        std::wstring problem;
        bool ok = false;
        if (task == "rewrite")
        {
            auto text = TextIo::ReadSelection();
            read.hadSelection = !text.empty();
            if (text.empty())
                text = TextIo::ReadWholeInput();
            if (!text.empty())
            {
                read.incoming = Utf8(text);
                read.source = text;
                ok = true;
            }
            else
                problem = L"没读到文字：先在输入框里写点内容，或选中要改的部分。";
        }
        else
        {
            const auto source = FeatureConfig::Get("reply", "source");
            const bool reader = FeatureConfig::GetBool("reply", "reader_enabled");
            if (source == "auto" && reader)
            {
                try
                {
                    ok = ReadChat(read, cancelled, foreground);
                }
                catch (const std::exception &)
                {
                    ok = false;
                }
            }
            if (!ok && source != "clipboard")
            {
                const auto text = TextIo::ReadSelection();
                if (!text.empty())
                {
                    read.incoming = Utf8(text);
                    read.source = text;
                    ok = true;
                }
            }
            if (!ok)
            {
                const auto text = TextIo::ClipboardText();
                if (!text.empty())
                {
                    read.incoming = Utf8(text.substr(0, 4000));
                    read.source = text;
                    ok = true;
                }
            }
            if (!ok)
                problem = reader ? L"没读到对方的话。选中那条消息，或先复制它，再按快捷键。"
                                 : L"没读到对方的话。选中那条消息，或先复制它，再按快捷键；"
                                   L"想自动读取聊天窗口，请到设置里打开“读取聊天窗口”。";
        }
        if (cancelled())
            return;
        Post([=]() mutable {
            if (g_generation.load() != generation)
                return;
            if (!ok)
            {
                ShowCard(g_reply.anchor, task == "rewrite" ? L"帮我改" : L"帮我回", L"", L"", problem, L"Esc 关闭");
                return;
            }
            read.pending.clear();
            read.done.clear();
            read.failed.clear();
            g_reply = std::move(read);
            const std::string key = g_reply.task == "rewrite" ? "rewrite" : g_reply.tones[g_reply.tab].id;
            Generate(key, false);
            Panel().Update(BuildReplyModel());
        });
    });
}

// ----------------------------------------------------------------------------------- clipboard panel

struct ClipState
{
    std::vector<ClipboardHistory::Entry> clips;
    std::vector<PersonalLibrary::Phrase> phrases;
    int tab = 0;
    std::wstring query;
    std::vector<std::wstring> values; // text behind each visible row
    RECT anchor{};
};
ClipState g_clip;

std::wstring Lower(std::wstring text)
{
    for (auto &c : text)
        c = static_cast<wchar_t>(towlower(c));
    return text;
}

std::wstring AgeText(int64_t at)
{
    if (!at)
        return {};
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto minutes = std::max<int64_t>(0, (now - at) / 60000);
    if (minutes < 1)
        return L"刚刚";
    if (minutes < 60)
        return std::to_wstring(minutes) + L" 分钟前";
    if (minutes < 1440)
        return std::to_wstring(minutes / 60) + L" 小时前";
    return std::to_wstring(minutes / 1440) + L" 天前";
}

PanelModel BuildClipModel()
{
    auto &s = g_clip;
    PanelModel model;
    model.title = L"剪贴板";
    model.tabs = {{L"剪贴历史", false}, {L"常用语", false}};
    model.tab = s.tab;
    model.searchable = true;
    model.query = s.query;
    model.width = 520;
    model.hint = L"数字键粘贴 · 直接打字搜索 · Tab 切换 · Esc 关闭";
    const auto needle = Lower(s.query);
    s.values.clear();
    if (s.tab == 0)
    {
        for (const auto &entry : s.clips)
        {
            if (!needle.empty() && Lower(entry.text).find(needle) == std::wstring::npos)
                continue;
            model.items.push_back(
                {L"", Clip(entry.text, 90), entry.app.empty() ? AgeText(entry.at) : entry.app, entry.pinned});
            s.values.push_back(entry.text);
            if (model.items.size() >= 9)
                break;
        }
    }
    else
    {
        for (const auto &phrase : s.phrases)
        {
            const auto text = Wide(phrase.text), name = Wide(phrase.name.empty() ? phrase.group : phrase.name);
            if (!needle.empty() && Lower(text).find(needle) == std::wstring::npos &&
                Lower(name).find(needle) == std::wstring::npos)
                continue;
            model.items.push_back({L"", Clip(text, 90), name, false});
            s.values.push_back(text);
            if (model.items.size() >= 9)
                break;
        }
    }
    if (model.items.empty())
        model.note = s.tab == 0 ? (s.query.empty() ? L"还没有剪贴历史。" : L"没有匹配的记录。")
                                : (s.query.empty() ? L"还没有常用语，可以在“资料库”里添加。" : L"没有匹配的常用语。");
    model.status = std::to_wstring(s.tab == 0 ? s.clips.size() : s.phrases.size()) + L" 条";
    return model;
}

void OpenClipboardPanel()
{
    CloseAll();
    auto &s = g_clip;
    s = ClipState();
    s.anchor = AnchorNow();
    s.clips = ClipboardHistory::LoadEntries();
    s.phrases = PersonalLibrary::Phrases(200);
    PanelHandlers handlers;
    handlers.onPick = [](int index) {
        if (index < 0 || index >= static_cast<int>(g_clip.values.size()))
            return;
        const auto text = g_clip.values[index];
        CloseAll();
        Work([text] {
            TextIo::WaitModifiersReleased();
            TextIo::PasteText(text, false);
        });
    };
    handlers.onTab = [](int index) {
        g_clip.tab = index;
        Panel().Update(BuildClipModel());
    };
    handlers.onQuery = [](const std::wstring &query) {
        g_clip.query = query;
        Panel().Update(BuildClipModel());
    };
    handlers.onClose = [] { CloseAll(); };
    Panel().Open(s.anchor, std::move(handlers), BuildClipModel());
}

bool Dispatch(const std::string &id)
{
    if (id == "shot" || id == "ocr" || id == "translate" || id == "live")
        PickAndRun(id);
    else if (id == "selection")
        TranslateSelection();
    else if (id == "reply")
        StartAssistant("reply");
    else if (id == "rewrite")
        StartAssistant("rewrite");
    else if (id == "clip")
        OpenClipboardPanel();
    else
        return false;
    return true;
}
} // namespace

void EnableHotkeys(bool enabled)
{
    g_hotkeys = enabled;
}

void OnStart()
{
    if (!g_hotkeys)
        return;
    RegisterAll();
    SetTimer(Controller(), kTimerHotkeys, 1000, nullptr);
}

void OnStop()
{
    for (const auto &[id, action] : g_registered)
        UnregisterHotKey(Controller(), id);
    g_registered.clear();
    KillTimer(Controller(), kTimerHotkeys);
    KillTimer(Controller(), kTimerLive);
    NextGeneration();
    g_panel.reset();
}

void OnHotkey(int id)
{
    const auto found = g_registered.find(id);
    if (found != g_registered.end())
        Dispatch(found->second);
}

void OnTimer(UINT_PTR id)
{
    if (id == kTimerHotkeys)
    {
        if (FeatureConfig::Revision() != g_registeredRevision)
            RegisterAll();
    }
    else if (id == kTimerLive)
    {
        if (!Vision::Busy())
        {
            KillTimer(Controller(), kTimerLive);
            if (g_panel)
                g_panel->Close();
        }
    }
}

bool Run(const std::string &id)
{
    static const std::set<std::string> known{"shot", "ocr",   "translate", "selection",
                                             "live", "reply", "rewrite",   "clip"};
    if (!known.count(id))
        return false;
    Start();
    std::thread([id] {
        if (!g_hotkeys)
            Sleep(350); // the settings window is hiding; let the previous app get focus back first
        Post([id] { Dispatch(id); });
    }).detach();
    return true;
}

json HotkeyStatus()
{
    try
    {
        std::ifstream in(StatusPath(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        auto parsed = json::parse(bytes, nullptr, false);
        if (parsed.is_object())
            return parsed;
    }
    catch (...)
    {
    }
    return json{{"hotkeys", json::object()}};
}
} // namespace Quick::Actions
