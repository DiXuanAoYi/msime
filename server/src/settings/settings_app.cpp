#include "settings/vision_service.h"
#include "english/speech_service.h"
#include "english/voice_studio.h"
#include "settings/personal_library.h"
#include "quick/quick_actions.h"
#include "settings/feature_config.h"
#include "settings/vision_config.h"
#include "settings/local_ocr_models.h"
#include "settings/translation_library.h"
#include "settings/local_asr_models.h"
#include "settings/local_tts_models.h"
#include "assistant/service.h"
#include "assistant/profile_store.h"
#include "english/vocabulary_service.h"
#include "engine/contracts/webview/validator.h"
#include "config/ime_config.h"
#include "engine/core/data_path.h"
#include "global/globals.h"
#include "resource/resource.h"
#include "settings/settings_launcher.h"
#include "settings/config_update_dispatch.h"
#include "settings/settings_environment.h"
#include "settings/ai_workbench.h"
#include "settings/neural_models.h"
#include "settings/webview_reply.h"
#include "settings/default_input_method.h"
#include "settings/api_credential_test.h"
#include "settings/collocation_model.h"
#include "settings/settings_splash.h"
#include "settings/settings_webview_error.h"
#include "settings/dictionary_manager.h"
#include "settings/serial_task_queue.h"
#include "settings/settings_task_diagnostics.h"
#include "statistics/stats_overview.h"
#include "statistics/stats_store.h"
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include "skin/candidate_skin_catalog.h"
#include "utils/common_utils.h"
#include "utils/single_instance.h"
#include "voice-input/voice_providers.h"

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <WebView2EnvironmentOptions.h>
#include <boost/json.hpp>
#include <dcomp.h>
#include <dwmapi.h>
#include <nlohmann/json.hpp>
#include <wil/com.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wrl.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
namespace json = boost::json;

namespace
{
constexpr wchar_t kWindowClass[] = L"MetasequoiaImeSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"知语输入法 · 设置";
constexpr wchar_t kTestWindowTitle[] = L"知语输入法 · 临时测试配置";
bool g_temporary_configuration = false;
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MetasequoiaImeSettings.SingleInstance";
constexpr UINT kActivateExistingWindow = WM_APP + 1;
constexpr UINT kOpenAboutSection = WM_APP + 2;
constexpr UINT kOpenReplyStudioSection = WM_APP + 6;
constexpr UINT kOpenPersonalSection = WM_APP + 7;
constexpr UINT kScanSkinCatalog = WM_APP + 3;
constexpr UINT kWorkerCompleted = WM_APP + 4;
constexpr UINT kQuitSettings = WM_APP + 5;
constexpr UINT_PTR kConfigReloadTimer = 1;
constexpr UINT_PTR kLingerTimer = 2;

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2CompositionController> g_composition_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<ICoreWebView2_3> g_webview3;
ComPtr<ICoreWebView2Controller2> g_controller2;
ComPtr<IDCompositionDevice> g_dcomp_device;
ComPtr<IDCompositionTarget> g_dcomp_target;
ComPtr<IDCompositionVisual> g_dcomp_root;
RECT g_maximize_button_rect{};
bool g_has_maximize_button_rect = false;
bool g_maximize_button_hover = false;
bool g_window_active = true;
bool g_window_minimized = false;
bool g_open_about_on_ready = false;
bool g_open_reply_on_ready = false;
bool g_open_personal_on_ready = false;
bool g_webview_content_started = false;
HWND g_settings_hwnd = nullptr;
std::optional<CandidateSkinCatalog::ScanResult> g_candidate_skin_catalog;
uint64_t g_candidate_skin_catalog_revision = 0;
std::unique_ptr<SerialTaskQueue> g_worker;
std::unique_ptr<ReplyStudio::Host> g_reply_studio;
std::unique_ptr<ReplyStudio::Host> g_chat_model;
std::unique_ptr<ReplyStudio::Host> g_chat_reader;
bool g_closing = false;
bool g_lingering = false;
bool g_reload_pending = false;
bool g_settings_light = false;
// appearance.settings_window_linger 的 UI 线程副本，由 ConfigCompletion 更新。
std::string g_settings_window_linger = "off";
std::wstring g_last_config_message;
bool g_worker_com_initialized = false; // worker-only
void CloseSettings(HWND hwnd);
void HideSettings(HWND hwnd);

// 冷启动打点：设置 METASEQUOIA_IME_SETTINGS_TRACE=1 后，各阶段距进程启动的毫秒数追加到
// %TEMP%\metasequoia-settings-startup.log。UI 线程和 worker 都会调用。
void TraceStartup(const char *stage, bool force_diagnostic = false)
{
    static const bool enabled = [] {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"METASEQUOIA_IME_SETTINGS_TRACE", value, 8) > 0 && value[0] == L'1';
    }();
    if (!enabled && !force_diagnostic)
        return;
    static const LONGLONG start = [] {
        FILETIME creation{}, exit{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
        return static_cast<LONGLONG>((static_cast<ULONGLONG>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime);
    }();
    FILETIME now_ft{};
    GetSystemTimePreciseAsFileTime(&now_ft);
    const LONGLONG now =
        static_cast<LONGLONG>((static_cast<ULONGLONG>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime);
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path path = std::filesystem::path(temp) / L"metasequoia-settings-startup.log";
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") == 0 && file)
    {
        fprintf(file, "%lu %8.1f ms  %s\n", GetCurrentProcessId(), static_cast<double>(now - start) / 10000.0, stage);
        fclose(file);
    }
}

void TraceSettingsTask(const SettingsTaskDiagnostics::Context &context, const char *stage,
                       const SettingsTaskDiagnostics::Failure *failure)
{
    std::string text = "operation=" + std::string(SettingsTaskDiagnostics::Operation(context.operation)) +
                       " action=" + SettingsTaskDiagnostics::Action(context.action) + " stage=" + stage;
    if (failure)
        text += " exception=" + std::string(failure->category) + " json_id=" + std::to_string(failure->json_id);
    TraceStartup(text.c_str(), failure != nullptr);
}
void SubmitSettingsTask(SettingsTaskDiagnostics::Context context, SerialTaskQueue::Task task);

nlohmann::json CandidateColorsToJson(const CandidateSkinCatalog::CandidateColors &colors)
{
    nlohmann::json json = nlohmann::json::object();
    if (!colors.accent.empty())
        json["accent"] = colors.accent;
    if (!colors.selected.empty())
        json["selected"] = colors.selected;
    if (!colors.hover.empty())
        json["hover"] = colors.hover;
    if (!colors.surface.empty())
        json["surface"] = colors.surface;
    if (!colors.border.empty())
        json["border"] = colors.border;
    if (!colors.text.empty())
        json["text"] = colors.text;
    if (!colors.number.empty())
        json["number"] = colors.number;
    if (!colors.translation.empty())
        json["translation"] = colors.translation;
    if (colors.showSelectedBar.has_value())
        json["showSelectedBar"] = *colors.showSelectedBar;
    const std::pair<const char *, const std::string *> optional[] = {
        {"candidateText", &colors.candidateText},
        {"preeditText", &colors.preeditText},
        {"preeditCaret", &colors.preeditCaret},
        {"selectedText", &colors.selectedText},
        {"selectedNumber", &colors.selectedNumber},
        {"selectedTranslation", &colors.selectedTranslation},
        {"translationPos", &colors.translationPos},
        {"translationSeparator", &colors.translationSeparator},
        {"selectedTranslationPos", &colors.selectedTranslationPos},
        {"selectedTranslationSeparator", &colors.selectedTranslationSeparator},
        {"selectedBar", &colors.selectedBar},
        {"preeditBackground", &colors.preeditBackground},
        {"preeditDivider", &colors.preeditDivider},
    };
    for (const auto &[key, value] : optional)
    {
        if (!value->empty())
            json[key] = *value;
    }
    nlohmann::json menu = nlohmann::json::object();
    if (!colors.menuBackground.empty())
        menu["background"] = colors.menuBackground;
    if (!colors.menuBorder.empty())
        menu["border"] = colors.menuBorder;
    if (!colors.menuText.empty())
        menu["text"] = colors.menuText;
    if (!colors.menuHover.empty())
        menu["hover"] = colors.menuHover;
    if (!menu.empty())
        json["menu"] = std::move(menu);
    return json;
}

nlohmann::json ToolbarColorsToJson(const CandidateSkinCatalog::ToolbarColors &colors)
{
    nlohmann::json json = nlohmann::json::object();
    if (!colors.background.empty())
        json["background"] = colors.background;
    if (!colors.border.empty())
        json["border"] = colors.border;
    if (!colors.handle.empty())
        json["handle"] = colors.handle;
    if (!colors.divider.empty())
        json["divider"] = colors.divider;
    if (!colors.icon.empty())
        json["icon"] = colors.icon;
    if (!colors.hover.empty())
        json["hover"] = colors.hover;
    return json;
}

void ShowAboutSection()
{
    if (!g_webview)
        return;
    g_webview->ExecuteScript(LR"JS((() => {
        const selectAbout = () => {
            const item = document.querySelector('.sidebar .item[data-target="about-settings"]');
            if (!item) return false;
            if (!item.classList.contains('active')) item.click();
            const section = document.getElementById('about-settings');
            if (!item.classList.contains('active') || !section || getComputedStyle(section).display === 'none')
                return false;
            item.scrollIntoView({ block: 'nearest' });
            return true;
        };
        if (selectAbout()) return;
        const retry = setInterval(() => {
            if (selectAbout()) clearInterval(retry);
        }, 50);
        setTimeout(() => clearInterval(retry), 10000);
    })())JS",
                             nullptr);
}

void ShowReplyStudioSection()
{
    if (!g_webview)
        return;
    g_webview->ExecuteScript(LR"JS((() => {
        const selectAbout = () => {
            const item = document.querySelector('.sidebar .item[data-target="reply-studio"]');
            if (!item) return false;
            if (!item.classList.contains('active')) item.click();
            const section = document.getElementById('reply-studio-settings');
            if (!item.classList.contains('active') || !section || getComputedStyle(section).display === 'none')
                return false;
            item.scrollIntoView({ block: 'nearest' });
            return true;
        };
        if (selectAbout()) return;
        const retry = setInterval(() => {
            if (selectAbout()) clearInterval(retry);
        }, 50);
        setTimeout(() => clearInterval(retry), 10000);
    })())JS",
                             nullptr);
}

void ShowPersonalSection()
{
    if (!g_webview)
        return;
    g_webview->ExecuteScript(LR"JS((() => {
        const selectAbout = () => {
            const item = document.querySelector('.sidebar .item[data-target="personal-library"]');
            if (!item) return false;
            if (!item.classList.contains('active')) item.click();
            const section = document.getElementById('personal-library');
            if (!item.classList.contains('active') || !section || getComputedStyle(section).display === 'none')
                return false;
            item.scrollIntoView({ block: 'nearest' });
            return true;
        };
        if (selectAbout()) return;
        const retry = setInterval(() => {
            if (selectAbout()) clearInterval(retry);
        }, 50);
        setTimeout(() => clearInterval(retry), 10000);
    })())JS",
                             nullptr);
}

void ApplyWindowActivationAppearance()
{
    if (!g_webview)
        return;

    // Keep this effect in the native host so it follows the real top-level
    // activation state rather than DOM focus. Only title-bar foreground
    // elements are muted; the page and title-bar backgrounds remain intact.
    const wchar_t *script = g_window_active ? LR"JS((() => {
            document.getElementById('metasequoia-window-inactive-overlay')?.remove();
            const id = 'metasequoia-window-activation-style';
            if (!document.getElementById(id)) {
                const style = document.createElement('style');
                style.id = id;
                style.textContent = `
                    .titlebar-left, .window-button {
                        transition: opacity 140ms ease-out;
                    }
                    html.metasequoia-window-inactive .titlebar-left,
                    html.metasequoia-window-inactive .window-button {
                        opacity: 0.52 !important;
                    }
                    html.metasequoia-window-inactive .window-button:hover,
                    html.metasequoia-window-inactive .window-button.host-hover {
                        opacity: 1 !important;
                    }
                `;
                document.head.appendChild(style);
            }
            document.documentElement.classList.remove('metasequoia-window-inactive');
        })())JS"
                                            : LR"JS((() => {
            document.getElementById('metasequoia-window-inactive-overlay')?.remove();
            const id = 'metasequoia-window-activation-style';
            if (!document.getElementById(id)) {
                const style = document.createElement('style');
                style.id = id;
                style.textContent = `
                    .titlebar-left, .window-button {
                        transition: opacity 140ms ease-out;
                    }
                    html.metasequoia-window-inactive .titlebar-left,
                    html.metasequoia-window-inactive .window-button {
                        opacity: 0.52 !important;
                    }
                    html.metasequoia-window-inactive .window-button:hover,
                    html.metasequoia-window-inactive .window-button.host-hover {
                        opacity: 1 !important;
                    }
                `;
                document.head.appendChild(style);
            }
            document.documentElement.classList.add('metasequoia-window-inactive');
        })())JS";
    g_webview->ExecuteScript(script, nullptr);
}

void ApplySettingsChromeTheme(HWND hwnd = nullptr)
{
    const HWND target = hwnd ? hwnd : g_settings_hwnd;
    const bool light = g_settings_light;
    if (target)
    {
        BOOL dark = light ? FALSE : TRUE;
        DwmSetWindowAttribute(target, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    if (g_controller2)
    {
        COREWEBVIEW2_COLOR background =
            light ? COREWEBVIEW2_COLOR{255, 243, 243, 243} : COREWEBVIEW2_COLOR{255, 32, 32, 32};
        g_controller2->put_DefaultBackgroundColor(background);
    }
}

void ResetTitlebarHoverAfterVisibilityChange()
{
    if (!g_webview)
        return;

    g_webview->ExecuteScript(LR"JS((() => {
        const root = document.documentElement;
        const suppressClass = 'metasequoia-suppress-titlebar-tooltip';
        const styleId = 'metasequoia-titlebar-tooltip-reset-style';
        if (!document.getElementById(styleId)) {
            const style = document.createElement('style');
            style.id = styleId;
            style.textContent = `
                html.${suppressClass} .window-button[data-tooltip]::after,
                html.${suppressClass} .window-button[data-tooltip]:hover::after {
                    opacity: 0 !important;
                    transform: translateX(-50%) !important;
                    transition-delay: 0s !important;
                }
            `;
            document.head.appendChild(style);
        }

        root.classList.add(suppressClass);
        document.querySelector('.window-controls')
            ?.classList.add('window-controls-click-reset');
        document.querySelectorAll('.window-button').forEach((button) => {
            button.classList.remove('host-hover', 'host-active');
            if (button instanceof HTMLElement) button.blur();
        });

        const previousRelease = window.__metasequoiaReleaseTooltipReset;
        if (typeof previousRelease === 'function') {
            window.removeEventListener('pointermove', previousRelease, true);
            window.removeEventListener('pointerdown', previousRelease, true);
        }
        const release = () => {
            root.classList.remove(suppressClass);
            window.removeEventListener('pointermove', release, true);
            window.removeEventListener('pointerdown', release, true);
            delete window.__metasequoiaReleaseTooltipReset;
        };
        window.__metasequoiaReleaseTooltipReset = release;
        window.addEventListener('pointermove', release, {capture: true, once: true});
        window.addEventListener('pointerdown', release, {capture: true, once: true});
    })())JS",
                             nullptr);
}

static nlohmann::json CustomHelpcodeSchemasJson()
{
    nlohmann::json schemas = nlohmann::json::array();
    for (const auto &schema : GetCustomHelpcodeSchemas())
        schemas.push_back({{"id", schema.schema}, {"name", schema.name}, {"name_en", schema.name_en}});
    return schemas;
}

static nlohmann::json CustomShuangpinSchemasJson()
{
    nlohmann::json schemas = nlohmann::json::array();
    for (const auto &schema : GetCustomShuangpinSchemas())
    {
        schemas.push_back(
            {{"id", schema.schema}, {"name", schema.name}, {"name_en", schema.name_en}, {"error", schema.error}});
    }
    return schemas;
}

std::wstring BuildConfigMessage(bool refresh_skin_catalog)
{

    const VoiceInputConfig &voice = GetConfiguredVoiceInput();
    const AiAssistantConfig ai = GetConfiguredAiAssistantSnapshot();
    const TencentTmtConfig &tencent_tmt = GetConfiguredTencentTmt();
    const CustomTranslationConfig &custom_translation = GetConfiguredCustomTranslation();
    const NiuTransConfig &niutrans = GetConfiguredNiuTrans();
    const NetworkProxyConfig network_proxy = GetConfiguredNetworkProxy();
    const FrequencyAdjustmentConfig &frequency = GetConfiguredFrequencyAdjustment();
    const FloatingToolbarItemsConfig &toolbar = GetConfiguredFloatingToolbarItems();
    const std::filesystem::path skins_root = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
    if (refresh_skin_catalog)
    {
        g_candidate_skin_catalog = CandidateSkinCatalog::Scan(skins_root);
        ++g_candidate_skin_catalog_revision;
    }
    const std::string skin_layout = GetConfiguredCandidateWindowLayout();
    const std::string skin_theme = ResolveConfiguredTheme(GetConfiguredThemeCand());
    nlohmann::json external_skins = nlohmann::json::array();
    if (g_candidate_skin_catalog)
    {
        for (const auto &skin : g_candidate_skin_catalog->packages)
        {
            external_skins.push_back(
                {{"id", skin.id},
                 {"name", skin.name},
                 {"version", skin.version},
                 {"author", skin.author},
                 {"description", skin.description},
                 {"base", skin.base},
                 {"layouts", skin.layouts},
                 {"themes", skin.themes},
                 {"minWidthDip", skin.minWidthDip},
                 {"decorationTopDip", skin.decorationTopDip},
                 {"decorationWidthDip", skin.decorationWidthDip},
                 {"decorationImage", skin.decorationImage},
                 {"decorationAlign", skin.decorationAlign},
                 {"cornerRadiusDip", skin.cornerRadiusDip ? nlohmann::json(*skin.cornerRadiusDip) : nlohmann::json()},
                 {"borderWidthDip", skin.borderWidthDip ? nlohmann::json(*skin.borderWidthDip) : nlohmann::json()},
                 {"itemCornerRadiusDip",
                  skin.itemCornerRadiusDip ? nlohmann::json(*skin.itemCornerRadiusDip) : nlohmann::json()},
                 {"shadow", skin.shadow},
                 {"fontFamily", skin.fontFamily},
                 {"backgroundImage", skin.backgroundImage},
                 {"backgroundFit", skin.backgroundFit},
                 {"backgroundOpacity", skin.backgroundOpacity},
                 {"candidate",
                  {{"dark", CandidateColorsToJson(skin.dark)}, {"light", CandidateColorsToJson(skin.light)}}},
                 {"toolbar",
                  {{"dark", ToolbarColorsToJson(skin.toolbarDark)}, {"light", ToolbarColorsToJson(skin.toolbarLight)}}},
                 {"toolbarCornerRadiusDip",
                  skin.toolbarCornerRadiusDip ? nlohmann::json(*skin.toolbarCornerRadiusDip) : nlohmann::json()},
                 // 已按 base 的默认皮肤清单解析好，页面预览直接用。
                 {"pageArrows", CandidateSkinCatalog::ResolvePageArrows(skins_root, skin.id, &skin)},
                 {"fitContent", CandidateSkinCatalog::ResolveFitContent(skins_root, skin.id, &skin)},
                 {"compatible", CandidateSkinCatalog::Supports(skin, skin_layout, skin_theme)}});
        }
    }
    // 内置皮肤的翻页箭头开关每次都现读 skins/default，用户改完清单刷新设置页即可在预览里看到。
    nlohmann::json builtin_page_arrows = nlohmann::json::object();
    for (const auto &id : CandidateSkinCatalog::BuiltInIds())
        builtin_page_arrows[id] = CandidateSkinCatalog::ResolvePageArrows(skins_root, id, nullptr);
    nlohmann::json builtin_fit_content = nlohmann::json::object();
    for (const auto &id : CandidateSkinCatalog::BuiltInIds())
        builtin_fit_content[id] = CandidateSkinCatalog::ResolveFitContent(skins_root, id, nullptr);
    nlohmann::json skin_issues = nlohmann::json::array();
    if (g_candidate_skin_catalog)
    {
        for (const auto &issue : g_candidate_skin_catalog->issues)
            skin_issues.push_back({{"folder", issue.folder}, {"reason", issue.reason}});
    }
    nlohmann::json polish_presets = nlohmann::json::array();
    for (const auto &preset : VoiceInput::BuiltinPolishPromptPresets())
    {
        polish_presets.push_back({{"id", std::string(preset.id)},
                                  {"name", std::string(preset.name)},
                                  {"prompt", std::string(preset.prompt)}});
    }
    // 规则位取不门控视图：总开关关闭时复选框仍要展示用户已存的勾选；
    // 总开关本身单独下发。
    const metasequoia::FuzzyPinyinOptions fuzzy_rules = GetConfiguredFuzzyPinyinRuleStates();
    nlohmann::json payload = {
        {"type", "configSnapshot"},
        {"data",
         {{"input",
           {{"mode", GetConfiguredInputMode()},
            {"schema", GetConfiguredInputSchemeName()},
            {"japanese_schema", GetConfiguredJapaneseSchema()},
            {"character_set", GetConfiguredCharacterSet()},
            {"default_ime_mode", GetConfiguredDefaultImeMode()},
            {"ime_mode_scope", GetConfiguredImeModeScope()},
            {"shuangpin_schema", GetConfiguredShuangpinSchema()},
            {"wubi_schema", GetConfiguredWubiSchema()},
            {"wubi_mixed_pinyin", GetConfiguredWubiMixedPinyin()},
            {"english_candidates", GetConfiguredEnglishCandidates()},
            {"english_candidate_app_rules", *GetConfiguredEnglishCandidateAppRules()},
            {"wubi_four_code_auto_commit", GetConfiguredWubiFourCodeAutoCommit()},
            {"wubi_fifth_code_top_commit", GetConfiguredWubiFifthCodeTopCommit()},
            {"vim_mode_apps", *GetConfiguredVimModeApps()},
            {"enabled_domain_dictionaries", *GetConfiguredEnabledDomainDictionaries()},

            {"escape_keeps_selected_word", GetConfiguredEscapeKeepsSelectedWord()},
            {"enter_learns_english_word", GetConfiguredEnterLearnsEnglishWord()},
            {"wubi_z_mode", GetConfiguredWubiZMode()},
            {"word_to_character", GetConfiguredWordToCharacterEnabled()},
            {"word_to_character_keys", GetConfiguredWordToCharacterKeys()},
            {"smart_punctuation", GetConfiguredSmartPunctuationEnabled()},
            {"smart_punctuation_space_convert", GetConfiguredSmartPunctuationSpaceConvertEnabled()},
            {"smart_punctuation_direct_digit", GetConfiguredSmartPunctuationDirectDigitEnabled()},
            {"smart_punctuation_direct_letter", GetConfiguredSmartPunctuationDirectLetterEnabled()},
            {"smart_punctuation_repeat_to_chinese", GetConfiguredSmartPunctuationRepeatToChineseEnabled()},
            {"paired_punctuation", GetConfiguredPairedPunctuationEnabled()},
            {"punctuation_lock", GetConfiguredPunctuationLock()},
            {"fuzzy_pinyin", GetConfiguredFuzzyPinyinEnabled()},
            {"fuzzy_z_zh", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::Z_ZH)},
            {"fuzzy_c_ch", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::C_CH)},
            {"fuzzy_s_sh", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::S_SH)},
            {"fuzzy_n_l", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::N_L)},
            {"fuzzy_f_h", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::F_H)},
            {"fuzzy_r_l", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::R_L)},
            {"fuzzy_an_ang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::AN_ANG)},
            {"fuzzy_en_eng", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::EN_ENG)},
            {"fuzzy_in_ing", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::IN_ING)},
            {"fuzzy_ian_iang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::IAN_IANG)},
            {"fuzzy_uan_uang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::UAN_UANG)}}},
          {"general",
           {{"diagnostic_log", GetConfiguredDiagnosticLogEnabled()},
            {"candidate_window_diagnostic_log", GetConfiguredDiagnosticLogEnabled()},
            {"tsf_diagnostic_log", GetConfiguredTsfDiagnosticLogEnabled()},
            {"floating_toolbar", GetConfiguredFloatingToolbarEnabled()},
            {"floating_toolbar_shadow", GetConfiguredFloatingToolbarShadow()},
            {"caret_state_indicator", GetConfiguredCaretStateIndicatorEnabled()},
            {"caret_state_indicator_on_focus", GetConfiguredCaretStateIndicatorOnFocus()},
            {"caret_state_indicator_position", GetConfiguredCaretStateIndicatorPosition()},
            {"floating_toolbar_fullwidth", toolbar.fullwidth},
            {"floating_toolbar_punctuation", toolbar.punctuation},
            {"floating_toolbar_character_set", toolbar.character_set},
            {"floating_toolbar_emoji", toolbar.emoji},
            {"floating_toolbar_screen_keyboard", toolbar.screen_keyboard},
            {"floating_toolbar_voice", toolbar.voice},
            {"floating_toolbar_reply", toolbar.reply},
            {"floating_toolbar_clipboard", toolbar.clipboard},
            {"floating_toolbar_settings", toolbar.settings},
            {"floating_toolbar_scale", GetConfiguredFloatingToolbarScale()},
            {"floating_toolbar_font_size", GetConfiguredFloatingToolbarFontSize()},
            {"floating_toolbar_auto_hide", GetConfiguredFloatingToolbarAutoHide()},
            {"floating_toolbar_auto_hide_delay", GetConfiguredFloatingToolbarAutoHideDelay()},
            {"cn_en_mixed_input", GetConfiguredEnglishCandidatesEnabled()},
            {"candidate_translations", GetConfiguredCandidateTranslationsEnabled()},
            {"candidate_translation_cloud", GetConfiguredCandidateTranslationCloudEnabled()},
            {"personal_translation_glossary", GetConfiguredPersonalTranslationGlossary()->Source()},
            {"disabled_translation_dictionaries", *GetConfiguredDisabledTranslationDictionaries()},
            {"cn_en_mixed_input_min_chars", GetConfiguredEnglishMixedInputMinChars()},
            {"emoji_mixed_input", GetConfiguredEmojiMixedInputEnabled()},
            {"kaomoji_mixed_input", GetConfiguredKaomojiMixedInputEnabled()},
            {"cloud_candidates", GetConfiguredCloudCandidatesEnabled()},
            {"paging_minus_equal", GetConfiguredPagingMinusEqualEnabled()},
            {"paging_comma_period", GetConfiguredPagingCommaPeriodEnabled()},
            {"paging_brackets", GetConfiguredPagingBracketsEnabled()},
            {"paging_tab", GetConfiguredPagingTabEnabled()},
            {"paging_page_up_down", GetConfiguredPagingPageUpDownEnabled()},
            {"paging_mouse_wheel", GetConfiguredPagingMouseWheelEnabled()},
            {"candidate_arrow_navigation", GetConfiguredCandidateArrowNavigationEnabled()}}},
          {"association",
           {{"sentence_wordlattice", GetConfiguredAssocSentenceWordLattice()},
            {"sentence_google", GetConfiguredAssocSentenceGoogle()},
            {"sentence_neural_desktop", GetConfiguredAssocSentenceNeuralDesktop()},
            {"sentence_neural_keyboard", GetConfiguredAssocSentenceNeuralKeyboard()},
            {"sentence_show_next_on_duplicate", GetConfiguredAssocSentenceShowNextOnDuplicate()},
            {"sentence_source_badge", GetConfiguredAssocSentenceSourceBadge()},
            {"sentence_collocation_enabled", GetConfiguredAssocSentenceCollocationEnabled()},
            {"sentence_collocation_model_status", [] {
                 nlohmann::json statuses = nlohmann::json::object();
                 for (const auto &[model_id, status] : collocation::GetModelStatuses())
                     statuses[model_id] = nlohmann::json{{"state", status.state}, {"progress", status.progress},
                                                         {"error", status.error}};
                 return statuses;
             }()},
            {"sentence_collocation_model", GetConfiguredAssocSentenceCollocationModel()},
            {"sentence_collocation_catalog", [] {
                 // 页面不自持目录副本：id 与下载 URL 留在 Server 侧，只下发展示字段。
                 nlohmann::json catalog = nlohmann::json::array();
                 for (const auto &entry : collocation::Catalog())
                 {
                     catalog.push_back(nlohmann::json{{"id", entry.id},
                                                      {"displayName", entry.display_name},
                                                      {"sizeHint", entry.size_hint},
                                                      {"license", entry.license},
                                                      {"url", collocation::SourceUrl(entry)}});
                 }
                 return catalog;
             }()}}},
          {"keybindings",
           {{"trilingual_cycle", GetConfiguredTrilingualCycleEnabled()},
            {"switch_language_shift", GetConfiguredSwitchLanguageShiftEnabled()},
            {"switch_language_ctrl", GetConfiguredSwitchLanguageCtrlEnabled()},
            {"switch_language_ctrl_alt_space", GetConfiguredSwitchLanguageCtrlAltSpaceEnabled()},
            {"toggle_character_set_ctrl_shift_f", GetConfiguredCharacterSetShortcutEnabled()}}},
          {"frequency_adjustment",
           {{"mode", frequency.mode},
            {"trigger_count", frequency.trigger_count},
            {"linear_step", frequency.linear_step}}},
          {"utility",
           {{"unicode_mode", GetConfiguredUnicodeModeEnabled()},
            {"quick_phrase", GetConfiguredQuickPhraseEnabled()},
            {"quick_phrase_candidates", GetConfiguredQuickPhraseCandidatesEnabled()},
            {"quick_phrase_frequency", GetConfiguredQuickPhraseFrequencyEnabled()},
            {"mixed_candidates", GetConfiguredMixedCandidatesEnabled()},
            {"date_time_candidates", GetConfiguredDateTimeCandidatesEnabled()},
            {"date_time_menu", GetConfiguredDateTimeMenuEnabled()},
            {"date_time_mode", GetConfiguredDateTimeModeEnabled()},
            {"emoji_mode", GetConfiguredEmojiModeEnabled()},
            {"kaomoji_mode", GetConfiguredKaomojiModeEnabled()},
            {"jianpin_mode", GetConfiguredJianpinModeEnabled()},
            {"y_mode", GetConfiguredYModeEnabled()},
            {"r_mode", GetConfiguredRModeEnabled()},
            {"v_mode", GetConfiguredVModeEnabled()},
            {"clipboard_history", GetConfiguredClipboardHistoryEnabled()}}},
          {"appearance",
           {{"ui_backend", GetConfiguredUiBackend()},
            {"candidate_fit_content", GetConfiguredCandidateFitContentMode()},
            {"settings_window_linger", GetConfiguredSettingsWindowLinger()},
            {"candidate_window_layout", GetConfiguredCandidateWindowLayout()},
            {"candidate_window_follow_cursor", GetConfiguredCandidateWindowFollowCursor()},
            {"candidate_skin", GetConfiguredCandidateSkin()},
            {"candidate_window_preedit_style", GetConfiguredCandidateWindowPreeditStyle()},
            {"candidate_window_preedit_shuangpin_quanpin", GetConfiguredCandidateWindowPreeditShuangpinQuanpin()},
            {"candidate_show_pinyin", GetConfiguredCandidateShowPinyin()},
            {"candidate_fixed_badge", GetConfiguredCandidateFixedBadge()},
            {"candidate_fixed_badge_style", GetConfiguredCandidateFixedBadgeStyle()},
            {"tsf_preedit_style", GetConfiguredTsfPreeditStyle()},
            {"tsf_preedit_shuangpin_quanpin", GetConfiguredTsfPreeditShuangpinQuanpin()},
            {"theme_mode", GetConfiguredThemeMode()},
            {"theme_settings", GetConfiguredThemeSettings()},
            {"theme_cand", GetConfiguredThemeCand()},
            {"theme_ftb", GetConfiguredThemeFtb()},
            {"theme_menu", GetConfiguredThemeMenu()},
            {"theme_emoji", GetConfiguredThemeEmoji()},
            {"theme_screen_keyboard", GetConfiguredThemeScreenKeyboard()},
            {"theme_handwriting", GetConfiguredThemeHandwriting()},
            {"theme_voice", GetConfiguredThemeVoice()},
            {"page_size", GetConfiguredCandidatePageSize()},
            {"font", GetConfiguredCandidateFont()},
            {"font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateFont())},
            {"fallback_fonts", GetConfiguredCandidateFallbackFonts()},
            {"fallback_font_css_families", GetConfiguredCandidateFallbackFontFamilies()},
            {"english_font", GetConfiguredCandidateEnglishFont()},
            {"english_font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont())},
            {"default_font", GetConfiguredCandidateDefaultFont()},
            {"default_font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateDefaultFont())},
            {"font_size", GetConfiguredCandidateFontSize()},
            {"candidate_window_preedit_font_size", GetConfiguredCandidateWindowPreeditFontSize()},
            {"cand_text_color", GetConfiguredCandidateTextColor()},
            {"system_fonts", GetSystemFontFamilies()},
            {"builtin_skin_page_arrows", std::move(builtin_page_arrows)},
            {"builtin_skin_fit_content", std::move(builtin_fit_content)},
            {"external_candidate_skins", std::move(external_skins)},
            {"candidate_skin_scan_issues", std::move(skin_issues)},
            {"candidate_skin_catalog_scanned", g_candidate_skin_catalog.has_value()},
            {"candidate_skin_catalog_revision", g_candidate_skin_catalog_revision},
            {"candidate_skin_directory", skins_root.u8string()}}},
          {"voice_input",
           {{"enabled", voice.enabled},
            {"hotkey_ralt", voice.hotkey_ralt},
            {"hotkey_ctrl_f9", voice.hotkey_ctrl_f9},
            {"hotkey_ctrl_win", voice.hotkey_ctrl_win},
            {"hotkey_rctrl_ralt", voice.hotkey_rctrl_ralt},
            {"hotkey_hold_space_lock", voice.hotkey_hold_space_lock},
            {"asr_provider", voice.asr_provider},
            {"doubao_auth_mode", voice.doubao_auth_mode},
            {"asr_app_key", voice.asr_app_key},
            {"tencent_secret_id", voice.tencent_secret_id},
            {"asr_token", voice.asr_token},
            {"asr_tokens", voice.asr_tokens},
            {"asr_endpoint", voice.asr_endpoint},
            {"asr_resource_id", voice.asr_resource_id},
            {"doubao_enable_itn", voice.doubao_enable_itn},
            {"doubao_enable_punc", voice.doubao_enable_punc},
            {"doubao_enable_ddc", voice.doubao_enable_ddc},
            {"doubao_boosting_table_id", voice.doubao_boosting_table_id},
            {"asr_model", voice.asr_model},
            {"polish_provider", voice.polish_provider},
            {"polish_token", voice.polish_token},
            {"polish_tokens", voice.polish_tokens},
            {"polish_endpoint", voice.polish_endpoint},
            {"polish_model", voice.polish_model},{"polish_thinking",voice.polish_thinking},
            {"polish_prompt_id", voice.polish_prompt_id},
            {"polish_prompt", voice.polish_prompt},
            {"polish_prompt_custom_1", voice.polish_prompt_custom_1},
            {"polish_prompt_custom_2", voice.polish_prompt_custom_2},
            {"polish_prompt_custom_3", voice.polish_prompt_custom_3},
            {"prompt_library", voice.prompt_library},
            {"language", voice.language},
            {"start_sound", voice.start_sound},
            {"end_sound", voice.end_sound},
            {"mute_system_audio", voice.mute_system_audio},
            {"notification_sound", voice.start_sound && voice.end_sound},
            {"polish_text", voice.polish_text},
            {"stream_inline_preedit", voice.stream_inline_preedit},
            {"commit_mode", voice.commit_mode}}},
          {"ai_assistant",
           {{"enabled", ai.enabled},
            {"provider", ai.provider},
            {"thinking", ai.thinking},{"task", ai.task},
            {"completion_enabled",ai.completion_enabled},{"question_enabled",ai.question_enabled},
            {"token", ai.token},
            {"tokens", ai.tokens},
            {"endpoint", ai.endpoint},
            {"model", ai.model},
            {"endpoints", ai.endpoints},
            {"models", ai.models},
            {"candidate_limit", ai.candidate_limit},
            {"prompt", ai.prompt},
            {"prompt_id", ai.prompt_id},
            {"prompt_custom_1", ai.prompt_custom_1},
            {"prompt_custom_2", ai.prompt_custom_2},
            {"prompt_custom_3", ai.prompt_custom_3}, {"prompt_library", ai.prompt_library}}},
          {"tencent_tmt",
           {{"secret_id", tencent_tmt.secret_id},
            {"secret_key", tencent_tmt.secret_key},
            {"region", tencent_tmt.region},
            {"target_language", tencent_tmt.target_language}}},
          {"custom_translation",
           {{"enabled", custom_translation.enabled},
            {"endpoint", custom_translation.endpoint},
            {"api_key", custom_translation.api_key}}},
          {"niutrans", {{"enabled", niutrans.enabled}, {"app_id", niutrans.app_id}, {"apikey", niutrans.apikey}}},
          {"network", {{"proxy_mode", network_proxy.mode}, {"proxy_server", network_proxy.server}}},
          {"helpcode",
           {{"shuangpin_helpcode", GetConfiguredShuangpinHelpcodeEnabled()},
            {"shuangpin_mid_sentence_helpcode", GetConfiguredShuangpinMidSentenceHelpcodeEnabled()},
            {"shuangpin_mid_sentence_helpcode_backtick", GetConfiguredShuangpinMidSentenceHelpcodeBacktick()},
            {"shuangpin_mid_sentence_helpcode_semicolon", GetConfiguredShuangpinMidSentenceHelpcodeSemicolon()},
            {"shuangpin_mid_sentence_helpcode_uppercase", GetConfiguredShuangpinMidSentenceHelpcodeUppercase()},
            {"shuangpin_direct_helpcode", GetConfiguredShuangpinDirectHelpcodeEnabled()},
            {"shuangpin_direct_helpcode_slash", GetConfiguredShuangpinDirectHelpcodeSlash()},
            {"shuangpin_direct_helpcode_uppercase", GetConfiguredShuangpinDirectHelpcodeUppercase()},
            {"shuangpin_helpcode_schema", GetConfiguredShuangpinHelpcodeSchema()},
            {"quanpin_helpcode", GetConfiguredQuanpinHelpcodeEnabled()},
            {"quanpin_helpcode_schema", GetConfiguredQuanpinHelpcodeSchema()},
            {"show_sp_helpcode_in_candidate_window", GetConfiguredShowShuangpinHelpcodeInCandidateWindow()},
            {"show_qp_helpcode_in_candidate_window", GetConfiguredShowQuanpinHelpcodeInCandidateWindow()}}},
          {"quanpin",
           {{"autocorrect_transposition", GetConfiguredQuanpinAutocorrectTransposition()},
            {"autocorrect_neighbor", GetConfiguredQuanpinAutocorrectNeighbor()},
            {"autocorrect_marker", GetConfiguredQuanpinAutocorrectMarker()}}},
          {"vocabulary", {{"enabled", GetConfiguredVocabularyEnabled()}, {"retention_days", GetConfiguredVocabularyRetentionDays()}}},
          {"statistics",
           {{"enabled", GetConfiguredStatisticsEnabled()}, {"retention", GetConfiguredStatisticsRetention()}}}}}};
    const auto candidate_translation = GetConfiguredCandidateTranslationSnapshot();
    payload["data"]["candidate_translation"] = {
        {"provider", candidate_translation.provider},   {"ai_provider", candidate_translation.ai_provider},
        {"endpoint", candidate_translation.endpoint},   {"model", candidate_translation.model},
        {"token", candidate_translation.token},         {"tokens", candidate_translation.tokens},
        {"endpoints", candidate_translation.endpoints}, {"models", candidate_translation.models}};
    payload["data"]["speech"] = Speech::GetConfig();
    payload["data"]["vision"] = VisionConfig::Get();
    for (const char *section : {"hotkeys", "reply", "clipboard"})
        payload["data"][section] = FeatureConfig::Get(section);
    payload["data"]["voice_input"]["polish_presets"] = std::move(polish_presets);
    payload["data"]["helpcode"]["custom_schemas"] = CustomHelpcodeSchemasJson();
    payload["data"]["helpcode"]["custom_directory"] = GetCustomHelpcodeDirectory();
    payload["data"]["input"]["custom_shuangpin_schemas"] = CustomShuangpinSchemasJson();
    payload["data"]["input"]["custom_shuangpin_directory"] = GetCustomShuangpinDirectory();
    payload["data"]["environment"] = {{"temporary_configuration", g_temporary_configuration}};
    payload["protocolVersion"] = metasequoia::webview::Version;
    const std::string serialized = payload.dump();
    if (!metasequoia::webview::Validate(json::parse(serialized), "server"))
        throw std::runtime_error("Invalid config snapshot");
    return string_to_wstring(serialized);
}

SerialTaskQueue::Completion ConfigCompletion(bool refresh_skin_catalog = false)
{
    TraceStartup(refresh_skin_catalog ? "worker: BuildConfigMessage(skins) begin" : "worker: BuildConfigMessage begin");
    auto message = BuildConfigMessage(refresh_skin_catalog);
    TraceStartup("worker: BuildConfigMessage end");
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light";
    return [message = std::move(message), light, linger = GetConfiguredSettingsWindowLinger()] {
        g_settings_light = light;
        g_settings_window_linger = linger;
        ApplySettingsChromeTheme();
        SettingsSplash::SetTheme(light);
        if (g_webview && message != g_last_config_message)
        {
            if (SUCCEEDED(g_webview->PostWebMessageAsJson(message.c_str())))
            {
                g_last_config_message = message;
                TraceStartup("ui: config posted to page");
            }
        }
        else if (!g_webview)
        {
            TraceStartup("ui: config dropped (webview not ready)");
        }
    };
}

void PostConfig(bool refresh_skin_catalog = false)
{
    if (!g_worker || g_closing)
        return;
    SubmitSettingsTask({refresh_skin_catalog ? "skinCatalogRequest" : "configRequest", true},
                       [refresh_skin_catalog] { return ConfigCompletion(refresh_skin_catalog); });
}

void PostValidatedServerMessage(json::object payload)
{
    if (!g_webview)
        return;
    payload["protocolVersion"] = metasequoia::webview::Version;
    if (!metasequoia::webview::Validate(payload, "server"))
        return;
    const std::wstring message = string_to_wstring(json::serialize(payload));
    g_webview->PostWebMessageAsJson(message.c_str());
}

void ReportSettingsTaskError(const SettingsTaskDiagnostics::Context &context, const SettingsTaskDiagnostics::Failure &)
{
    if (std::string_view(context.operation) == "initialConfig" && !g_webview)
    {
        MessageBoxW(g_settings_hwnd, L"设置加载失败，请重试。诊断已记录，输入法仍可使用。", L"知语输入法",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    // A native Save As dialog runs a nested message loop. Background status failures must not open another modal.
    PostValidatedServerMessage(
        {{"type", "settingsOperationError"},
         {"automatic", context.automatic},
         {"message", context.automatic ? "后台状态刷新失败，请点击相应刷新按钮重试。当前编辑内容保留。"
                                       : "本次设置操作失败，请重试。当前编辑内容保留。"}});
}
void SubmitSettingsTask(SettingsTaskDiagnostics::Context context, SerialTaskQueue::Task task)
{
    if (g_worker)
        g_worker->Submit(
            SettingsTaskDiagnostics::Wrap(context, std::move(task), TraceSettingsTask, ReportSettingsTaskError));
}
SettingsTaskDiagnostics::Context SettingsMessageContext(const std::string &type, const json::value &message)
{
    const char *action = "-";
    if (const auto *data = message.as_object().if_contains("data"); data && data->is_object())
        if (const auto *value = data->as_object().if_contains("action"); value && value->is_string())
            action =
                SettingsTaskDiagnostics::Action(std::string_view(value->as_string().data(), value->as_string().size()));
    const bool automatic = type == "configRequest" || type == "skinCatalogRequest" ||
                           type == "collocationModelStatusRequest" || std::string_view(action) == "status" ||
                           std::string_view(action) == "overview" || std::string_view(action) == "catalog" ||
                           std::string_view(action) == "domainCatalog";
    return {SettingsTaskDiagnostics::Operation(type), automatic, action};
}

void PostWindowState(HWND hwnd)
{
    PostValidatedServerMessage({
        {"type", "windowState"},
        {"data", {{"isMaximized", IsZoomed(hwnd) != FALSE}}},
    });
}

void PostMaximizeButtonEvent(const char *event_name)
{
    PostValidatedServerMessage({
        {"type", "maxButtonEvent"},
        {"data", {{"event", event_name}}},
    });
}

int HitTestName(const std::string &name)
{
    if (name == "left")
        return HTLEFT;
    if (name == "right")
        return HTRIGHT;
    if (name == "top")
        return HTTOP;
    if (name == "bottom")
        return HTBOTTOM;
    if (name == "left-top")
        return HTTOPLEFT;
    if (name == "right-top")
        return HTTOPRIGHT;
    if (name == "left-bottom")
        return HTBOTTOMLEFT;
    if (name == "right-bottom")
        return HTBOTTOMRIGHT;
    return HTCLIENT;
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring &text)
{
    if (!OpenClipboard(hwnd))
        return false;
    EmptyClipboard();

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
    {
        CloseClipboard();
        return false;
    }

    void *destination = GlobalLock(memory);
    if (!destination)
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

std::filesystem::path StatisticsDatabasePath()
{
    return std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"stats.db";
}

// The local calendar day "today" is bucketed into. GetLocalTime resolves the
// current timezone and DST, matching the Server-side aggregator's resolver.
int LocalTodayDayKey()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return MsimeStats::DayKeyOf(now.wYear, now.wMonth, now.wDay);
}

json::object OverviewToJson(const MsimeStats::Overview &overview)
{
    json::object categories;
    categories["cjk"] = overview.categories.cjk;
    categories["latin"] = overview.categories.latin;
    categories["digit"] = overview.categories.digit;
    categories["punct"] = overview.categories.punct;
    categories["other"] = overview.categories.other;

    json::array hourly;
    hourly.reserve(overview.today_hourly.size());
    for (const int64_t chars : overview.today_hourly)
        hourly.push_back(json::value(chars));

    json::array daily;
    daily.reserve(overview.daily.size());
    for (const MsimeStats::DailyRow &row : overview.daily)
    {
        json::object entry;
        entry["dayKey"] = row.day_key;
        entry["cjk"] = row.cjk;
        entry["latin"] = row.latin;
        entry["digit"] = row.digit;
        entry["punct"] = row.punct;
        entry["other"] = row.other;
        entry["activeMs"] = row.active_ms;
        daily.push_back(std::move(entry));
    }

    json::object result;
    result["hasData"] = overview.has_data;
    result["firstDayKey"] = overview.first_day_key;
    result["lastDayKey"] = overview.last_day_key;
    result["days"] = overview.days;
    result["totalChars"] = overview.total_chars;
    result["totalActiveMs"] = overview.total_active_ms;
    result["todayDayKey"] = overview.today_day_key;
    result["todayChars"] = overview.today_chars;
    result["todayActiveMs"] = overview.today_active_ms;
    result["averagePerDay"] = overview.average_per_day;
    result["currentStreak"] = overview.current_streak;
    result["longestStreak"] = overview.longest_streak;
    result["bestDayKey"] = overview.best_day_key;
    result["bestDayChars"] = overview.best_day_chars;
    result["todaySpeed"] = overview.today_speed;
    result["averageSpeed"] = overview.average_speed;
    result["fastestSpeed"] = overview.fastest_speed;
    result["fastestDayKey"] = overview.fastest_day_key;
    result["categories"] = std::move(categories);
    result["todayHourly"] = std::move(hourly);
    result["daily"] = std::move(daily);
    return result;
}

// Handles one statistics request on the worker thread. Each request opens the
// store, uses it and closes it: the settings process can exit at any time, the
// store carries no cross-request state, and all requests are serialised by the
// single serial task queue, so no in-process lock is needed. WAL plus the
// store's busy timeout arbitrate with the Server process that writes the same
// file.
json::object HandleStatsRequest(const json::object &data, HWND hwnd)
{
    const std::string request_id = json::value_to<std::string>(data.at("requestId"));
    const std::string action = json::value_to<std::string>(data.at("action"));
    json::object response;
    response["requestId"] = request_id;
    response["action"] = action;
    response["ok"] = true;
    response["message"] = "成功";

    if (action == "openDirectory")
    {
        const std::filesystem::path directory = StatisticsDatabasePath().parent_path();
        std::error_code error_code;
        std::filesystem::create_directories(directory, error_code);
        ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        response["message"] = "已打开数据目录";
        return response;
    }
    if (action != "overview" && action != "clearAll")
    {
        response["ok"] = false;
        response["message"] = "未知的统计操作。";
        return response;
    }

    // The missing-file check is the observable half of "recording is off by
    // default": Store::Open always opens with SQLITE_OPEN_CREATE, so merely
    // browsing statistics must not create stats.db.
    std::error_code exists_error;
    const std::filesystem::path database_path = StatisticsDatabasePath();
    const bool database_exists = std::filesystem::exists(database_path, exists_error) && !exists_error;
    std::unique_ptr<MsimeStats::Store> store;
    if (database_exists)
    {
        std::string error;
        store = MsimeStats::Store::Open(database_path, error);
        if (!store)
        {
            response["ok"] = false;
            response["message"] = "无法打开统计数据库：" + error;
            return response;
        }
    }

    const int today = LocalTodayDayKey();
    if (action == "clearAll")
    {
        int64_t removed_days = 0;
        if (store)
        {
            std::string error;
            if (!store->ClearAll(removed_days, error))
            {
                response["ok"] = false;
                response["message"] = "清空统计数据失败：" + error;
                return response;
            }
        }
        response["removedDays"] = removed_days;
        response["message"] =
            removed_days > 0 ? "已清空 " + std::to_string(removed_days) + " 天数据" : "没有需要清空的数据";
    }

    MsimeStats::Overview overview;
    if (store)
    {
        std::vector<MsimeStats::DailyRow> daily;
        std::vector<MsimeStats::HourlyRow> hourly;
        std::string error;
        if (!store->DailyRows(daily, error) || !store->HourlyRows(today, hourly, error))
        {
            response["ok"] = false;
            response["message"] = "读取统计数据失败：" + error;
            return response;
        }
        overview = MsimeStats::ComputeOverview(daily, hourly, today);
    }
    else
    {
        // Derive the empty overview through the same path so the JSON shape is
        // identical (24 zeroed hour buckets, zeroed categories).
        overview = MsimeStats::ComputeOverview({}, {}, today);
    }
    response["overview"] = OverviewToJson(overview);
    if (action == "overview")
        response["message"] = "已加载统计数据";
    return response;
}

void HandleWebMessage(HWND hwnd, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    if (g_closing || !g_worker)
        return;
    wil::unique_cotaskmem_string raw;
    if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw)
        return;

    SettingsTaskDiagnostics::Context diagnostic{"unknown", true, "-"};
    try
    {
        json::value value = json::parse(wstring_to_string(raw.get()));
        if (!metasequoia::webview::Validate(value, "client", "settings"))
            return;
        const std::string type = json::value_to<std::string>(value.at("type"));
        diagnostic = SettingsMessageContext(type, value);
        TraceSettingsTask(diagnostic, "dispatch.begin", nullptr);
        if (type == "dragStart")
        {
            ReleaseCapture();
            PostMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        else if (type == "resizeHitTest" || type == "resizeStart")
        {
            const int hit_test = HitTestName(json::value_to<std::string>(value.at("data")));
            if (hit_test != HTCLIENT)
            {
                ReleaseCapture();
                PostMessageW(hwnd, WM_NCLBUTTONDOWN, hit_test, 0);
            }
        }
        else if (type == "focus")
        {
            SetFocus(hwnd);
        }
        else if (type == "windowControl")
        {
            const std::string command = json::value_to<std::string>(value.at("data"));
            if (command == "minimize")
                ShowWindow(hwnd, SW_MINIMIZE);
            else if (command == "maximize")
                ShowWindow(hwnd, SW_MAXIMIZE);
            else if (command == "restore")
                ShowWindow(hwnd, SW_RESTORE);
            else if (command == "close")
                HideSettings(hwnd);
        }
        else if (type == "maximizeButtonRect")
        {
            const auto &data = value.at("data").as_object();
            const double x = data.if_contains("x") ? json::value_to<double>(*data.if_contains("x")) : 0.0;
            const double y = data.if_contains("y") ? json::value_to<double>(*data.if_contains("y")) : 0.0;
            const double width = data.if_contains("width") ? json::value_to<double>(*data.if_contains("width")) : 0.0;
            const double height =
                data.if_contains("height") ? json::value_to<double>(*data.if_contains("height")) : 0.0;
            double scale = data.if_contains("dpr") ? json::value_to<double>(*data.if_contains("dpr")) : 0.0;
            if (scale <= 0.0)
                scale = static_cast<double>(GetDpiForWindow(hwnd)) / 96.0;
            g_maximize_button_rect = {static_cast<LONG>(std::lround(x * scale)),
                                      static_cast<LONG>(std::lround(y * scale)),
                                      static_cast<LONG>(std::lround((x + width) * scale)),
                                      static_cast<LONG>(std::lround((y + height) * scale))};
            g_has_maximize_button_rect = true;
        }
        else if (type == "configRequest")
        {
            TraceStartup("ui: page configRequest (DOMContentLoaded)");
            // A snapshot sent before the new document installed its listener may be lost.
            g_last_config_message.clear();
            PostConfig(false);
            PostMessageW(hwnd, kScanSkinCatalog, 0, 0);
        }
        else if (type == "skinCatalogRequest")
        {
            SubmitSettingsTask(diagnostic, [] {
                auto completion = ConfigCompletion(true);
                NotifyImeServerCandidateSkinRefresh();
                return completion;
            });
        }
        else if (type == "openSkinDirectory")
        {
            const std::filesystem::path skins = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
            std::error_code ec;
            std::filesystem::create_directories(skins, ec);
            if (!ec)
                ShellExecuteW(hwnd, L"open", skins.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "openHelpcodeDirectory")
        {
            const std::filesystem::path directory = metasequoia::path_from_utf8(GetCustomHelpcodeDirectory().c_str());
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (!ec)
                ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "openShuangpinDirectory")
        {
            const std::filesystem::path directory = metasequoia::path_from_utf8(GetCustomShuangpinDirectory().c_str());
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (!ec)
                ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "collocationModelDownload")
        {
            // 后台线程下载，进度随下一次配置快照回给页面；页面在下载态轮询 configRequest。
            // 忙碌（其他模型在下载）时 StartDownload 拒绝，页面在下载态禁用其余下载按钮。
            collocation::StartDownload(json::value_to<std::string>(value.at("data").at("modelId")));
            PostConfig(false);
        }
        else if (type == "collocationModelDelete")
        {
            // 下载中与当前生效解析的 id 在 DeleteModel 里拒绝；无论成败都回快照刷新列表。
            collocation::DeleteModel(json::value_to<std::string>(value.at("data").at("modelId")));
            PostConfig(false);
        }
        else if (type == "collocationModelImport")
        {
            // 应用内下载太慢时，用户可以用浏览器下好再导入；选择对话框在 UI 线程模态弹出，
            // 复制与格式校验在后台线程，进度同样随快照回放（页面在 importing 态轮询）。
            const std::string model_id = json::value_to<std::string>(value.at("data").at("modelId"));
            const std::filesystem::path source = collocation::PromptForModelFile(hwnd);
            if (!source.empty())
                collocation::ImportModel(model_id, source);
            PostConfig(false);
        }
        else if (type == "collocationModelStatusRequest")
        {
            PostConfig(false);
        }
        else if (type == "configUpdate")
        {
            const auto data = value.at("data").as_object();
            SubmitSettingsTask(diagnostic, [data] {
                TraceStartup("worker: configUpdate begin");
                const bool saved = SettingsConfig::ApplyConfigUpdate(data);
                TraceStartup(saved ? "worker: configUpdate saved" : "worker: configUpdate failed");
                auto completion = ConfigCompletion();
                return [saved, completion = std::move(completion)] {
                    completion();
                    if (!saved)
                        MessageBoxW(g_settings_hwnd, L"设置保存失败，请重试。", L"知语输入法", MB_OK | MB_ICONWARNING);
                };
            });
        }
        else if (type == "defaultImeRequest")
        {
            const auto &data = value.at("data").as_object();
            const std::string requestId = json::value_to<std::string>(data.at("requestId"));
            const std::string action = json::value_to<std::string>(data.at("action"));
            if (action == "openSystemSettings")
                default_input_method::OpenSystemSettings(hwnd);
            SubmitSettingsTask(diagnostic, [requestId, action]() -> SerialTaskQueue::Completion {
                bool ok = true;
                std::string message;
                if (action == "setDefault")
                {
                    ok = default_input_method::SetAsDefault(message);
                    if (ok)
                        message = "知语输入法已排在中文（简体）键盘的第一位；其他语言顺序保留。";
                }
                const auto status = default_input_method::QueryStatus();
                json::object response = {{"type", "defaultImeResponse"},
                                         {"requestId", requestId},
                                         {"action", action},
                                         {"ok", ok},
                                         {"message", message},
                                         {"enabled", status.enabled},
                                         {"isDefault", status.is_default},
                                         {"protocolVersion", metasequoia::webview::Version}};
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid default IME response");
                const auto text = string_to_wstring(json::serialize(response));
                return [text] {
                    if (g_webview)
                        g_webview->PostWebMessageAsJson(text.c_str());
                };
            });
        }
        else if (type == "neuralModelsRequest")
        {
            auto request = nlohmann::json::parse(json::serialize(value.at("data")));
            const auto source = request.value("action", "") == "import"
                                    ? NeuralModels::PromptForModelFile(hwnd, request.value("id", ""))
                                    : std::filesystem::path();
            SubmitSettingsTask(diagnostic, [request = std::move(request), source]() -> SerialTaskQueue::Completion {
                const auto response = NeuralModels::Handle(request, source);
                const auto message = string_to_wstring(SettingsWebView::SerializeReply(response));
                auto config = ConfigCompletion();
                return [message, config = std::move(config)] {
                    config();
                    if (g_webview)
                        g_webview->PostWebMessageAsJson(message.c_str());
                };
            });
        }
        else if (type == "apiModelList" || type == "aiTaskRequest")
        {
            auto request = nlohmann::json::parse(json::serialize(value.at("data")));
            if (type == "aiTaskRequest" && request.value("action", "") == "cancel")
                AiWorkbench::Cancel();
            SubmitSettingsTask(diagnostic, [type, request = std::move(request)]() -> SerialTaskQueue::Completion {
                auto response =
                    type == "apiModelList" ? AiWorkbench::HandleModelList(request) : AiWorkbench::Handle(request);
                const auto message = string_to_wstring(SettingsWebView::SerializeReply(response));
                return [message] {
                    if (g_webview)
                        g_webview->PostWebMessageAsJson(message.c_str());
                };
            });
        }
        else if (type == "apiCredentialTest")
        {
            const auto &data = value.at("data").as_object();
            const std::string request_id = json::value_to<std::string>(data.at("requestId"));
            ApiCredentialTest::Request request;
            request.service = json::value_to<std::string>(data.at("service"));
            for (const auto &[key, item] : data.at("config").as_object())
            {
                if (item.is_string())
                    request.config.emplace(std::string(key), json::value_to<std::string>(item));
            }
            SubmitSettingsTask(diagnostic, [request_id, request = std::move(request)]() -> SerialTaskQueue::Completion {
                ApiCredentialTest::Result result;
                try
                {
                    result = ApiCredentialTest::Run(request);
                }
                catch (...)
                {
                    result = {false, "测试失败：内部错误。"};
                }
                json::value response = {{"type", "apiCredentialTestResult"},
                                        {"requestId", request_id},
                                        {"ok", result.ok},
                                        {"message", result.message},
                                        {"protocolVersion", metasequoia::webview::Version}};
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid API credential test response");
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] {
                    if (g_webview)
                        g_webview->PostWebMessageAsJson(message.c_str());
                };
            });
        }
        else if (type == "openKeyboardPanel")
        {
            OpenKeyboardPanelApplication();
        }
        else if (type == "openHandwritingPanel")
        {
            OpenHandwritingPanelApplication();
        }
        else if (type == "restartServer")
        {
            if (!NotifyImeServerRestart())
            {
                MessageBoxW(g_settings_hwnd, L"未能重启输入法进程，请稍后重试。", L"知语输入法",
                            MB_OK | MB_ICONWARNING);
            }
        }
        else if (type == "openExternalUrl")
        {
            const std::string url = json::value_to<std::string>(value.at("data"));
            if (url.rfind("https://", 0) == 0)
                ShellExecuteW(hwnd, L"open", string_to_wstring(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "copyText")
        {
            CopyTextToClipboard(hwnd, string_to_wstring(json::value_to<std::string>(value.at("data"))));
        }
        else if (type == "dictionaryRequest")
        {
            const auto data = value.at("data").as_object();
            SubmitSettingsTask(diagnostic, [data]() -> SerialTaskQueue::Completion {
                json::value response;
                try
                {
                    response = SettingsDictionary::HandleRequest(data);
                }
                catch (...)
                {
                    response =
                        json::object{{"ok", false}, {"message", "词库操作失败，请重试"}, {"rows", json::array{}}};
                }
                auto &object = response.as_object();
                object["type"] = "dictionaryResponse";
                object["requestId"] = data.at("requestId");
                object["action"] = data.at("action");
                object["dictionary"] = data.at("dictionary");
                object["protocolVersion"] = metasequoia::webview::Version;
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid dictionary response");
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] {
                    if (g_webview)
                        g_webview->PostWebMessageAsJson(message.c_str());
                };
            });
        }
        else if (type == "speechRequest")
        {
            auto response = Speech::Handle(nlohmann::json::parse(json::serialize(value.at("data"))));
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "personalLibraryRequest")
        {
            auto response = PersonalLibrary::Handle(nlohmann::json::parse(json::serialize(value.at("data"))), hwnd);
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "voiceStudioRequest")
        {
            auto response = VoiceStudio::Handle(nlohmann::json::parse(json::serialize(value.at("data"))), hwnd);
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "visionRequest")
        {
            auto response = Vision::Handle(nlohmann::json::parse(json::serialize(value.at("data"))), hwnd);
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "localOcrRequest")
        {
            auto response = LocalOcr::Handle(nlohmann::json::parse(json::serialize(value.at("data"))));
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "localTtsRequest")
        {
            auto response = LocalTts::Handle(nlohmann::json::parse(json::serialize(value.at("data"))));
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "localAsrRequest")
        {
            auto response = LocalAsr::Handle(nlohmann::json::parse(json::serialize(value.at("data"))));
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "chatRequest")
        {
            const auto action = json::value_to<std::string>(value.at("data").at("action"));
            if ((action == "scan" || action == "poll") && !IsWindowVisible(hwnd))
                return;
            if (!g_chat_reader)
                g_chat_reader =
                    std::make_unique<ReplyStudio::Host>([hwnd] { PostMessageW(hwnd, kWorkerCompleted, 0, 0); }, true);
            g_chat_reader->Submit(nlohmann::json::parse(json::serialize(value.at("data"))),
                                  [](const nlohmann::json &response) {
                                      if (g_webview)
                                          g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
                                  });
        }
        else if (type == "assistantRequest")
        {
            auto request = nlohmann::json::parse(json::serialize(value.at("data")));
            if (request.value("origin", "") == "chat" && request.value("action", "") != "cancel" &&
                !IsWindowVisible(hwnd))
                return;
            auto &host = request.value("origin", "") == "chat" ? g_chat_model : g_reply_studio;
            if (!host)
                host = std::make_unique<ReplyStudio::Host>([hwnd] { PostMessageW(hwnd, kWorkerCompleted, 0, 0); });
            host->Submit(std::move(request), [](const nlohmann::json &response) {
                if (g_webview)
                    g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
            });
        }
        else if (type == "replyProfileRequest")
        {
            auto response = ReplyStudio::HandleProfileRequest(nlohmann::json::parse(json::serialize(value.at("data"))));
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "quickActionRequest")
        {
            const auto request = nlohmann::json::parse(json::serialize(value.at("data")));
            const auto action = request.value("action", "");
            nlohmann::json response = {{"type", "quickActionResponse"},
                                       {"requestId", request.value("requestId", "")},
                                       {"action", action},
                                       {"ok", true},
                                       {"message", ""}};
            if (action == "run")
            {
                // The settings window steps aside so the action sees the app the user was working in.
                if (Quick::Actions::Run(request.value("id", "")))
                    ShowWindow(hwnd, SW_MINIMIZE);
                else
                {
                    response["ok"] = false;
                    response["message"] = "未知的快捷操作";
                }
            }
            response["hotkeys"] = Quick::Actions::HotkeyStatus().value("hotkeys", nlohmann::json::object());
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "dictionaryLibraryRequest")
        {
            const auto response =
                TranslationLibrary::Handle(nlohmann::json::parse(json::serialize(value.at("data"))), hwnd);
            if (g_webview)
                g_webview->PostWebMessageAsJson(string_to_wstring(response.dump()).c_str());
        }
        else if (type == "vocabularyRequest")
        {
            const auto data = value.at("data").as_object();
            SubmitSettingsTask(diagnostic, [data]() -> SerialTaskQueue::Completion {
                const auto response = Vocabulary::HandleRequest(data);
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] {
                    if (g_webview)
                        g_webview->PostWebMessageAsJson(message.c_str());
                };
            });
        }
        else if (type == "statsRequest")
        {
            const auto data = value.at("data").as_object();
            SubmitSettingsTask(diagnostic, [data, hwnd]() -> SerialTaskQueue::Completion {
                json::object response;
                try
                {
                    response = HandleStatsRequest(data, hwnd);
                }
                catch (...)
                {
                    response["ok"] = false;
                    response["message"] = "统计操作失败，请重试";
                }
                response["requestId"] = data.at("requestId");
                response["action"] = data.at("action");
                response["type"] = "statsResponse";
                response["protocolVersion"] = metasequoia::webview::Version;
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid statistics response");
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] {
                    if (g_webview)
                        g_webview->PostWebMessageAsJson(message.c_str());
                };
            });
        }
        TraceSettingsTask(diagnostic, "dispatch.end", nullptr);
    }
    catch (const nlohmann::json::exception &error)
    {
        const SettingsTaskDiagnostics::Failure failure{"json", error.id};
        TraceSettingsTask(diagnostic, "dispatch.failed", &failure);
        ReportSettingsTaskError(diagnostic, failure);
    }
    catch (const std::exception &)
    {
        const SettingsTaskDiagnostics::Failure failure{"standard", 0};
        TraceSettingsTask(diagnostic, "dispatch.failed", &failure);
        ReportSettingsTaskError(diagnostic, failure);
    }
}

HRESULT EnsureCompositionTree(HWND hwnd)
{
    HRESULT hr = DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice),
                                          reinterpret_cast<void **>(g_dcomp_device.GetAddressOf()));
    if (FAILED(hr))
        return hr;
    hr = g_dcomp_device->CreateTargetForHwnd(hwnd, TRUE, &g_dcomp_target);
    if (FAILED(hr))
        return hr;
    hr = g_dcomp_device->CreateVisual(&g_dcomp_root);
    if (FAILED(hr))
        return hr;
    hr = g_dcomp_target->SetRoot(g_dcomp_root.Get());
    if (FAILED(hr))
        return hr;
    return g_dcomp_device->Commit();
}

bool g_webview_failed = false;
HRESULT FailWebView(HWND hwnd, const wchar_t *stage, HRESULT result)
{
    SettingsSplash::Dismiss();
    if (!g_webview_failed && !g_closing)
    {
        g_webview_failed = true;
        const auto message = SettingsWebView::FailureMessage(stage, FAILED(result) ? result : E_FAIL);
        MessageBoxW(hwnd, message.c_str(), L"知语输入法 · 设置页面", MB_OK | MB_ICONERROR);
        ShowWindow(hwnd, SW_HIDE);
        PostMessageW(hwnd, kQuitSettings, 0, 0);
    }
    return FAILED(result) ? result : E_FAIL;
}
HRESULT OnControllerCreated(HWND hwnd, HRESULT result, ICoreWebView2CompositionController *controller)
{
    TraceStartup("ui: controller created");
    if (g_closing || !IsWindow(hwnd))
        return E_ABORT;
    if (FAILED(result) || !controller)
    {
        return FailWebView(hwnd, L"创建 WebView2 控制器（异步）", result);
    }
    g_composition_controller = controller;
    HRESULT query = g_composition_controller.As(&g_controller);
    if (FAILED(query))
        return FailWebView(hwnd, L"获取 WebView2 控制器接口", query);
    query = g_controller->get_CoreWebView2(&g_webview);
    if (FAILED(query) || !g_webview)
        return FailWebView(hwnd, L"获取 WebView2 页面对象", query);

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_webview->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(TRUE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
    }
    g_controller->put_ZoomFactor(1.0);

    query = g_webview.As(&g_webview3);
    if (FAILED(query))
        return FailWebView(hwnd, L"获取本机页面映射接口", query);
    if (g_webview3)
    {
        // 虚拟主机名必须带保留 TLD `.example`。不带点的单标签主机名会先走一轮网络名称解析，
        // 实测导航前要空等约 2 秒才开始加载内容。
        const std::filesystem::path assets =
            std::filesystem::path(CommonUtils::get_ime_data_path_w()) / "html/webview2/settings/ime-settings/dist";
        query = g_webview3->SetVirtualHostNameToFolderMapping(L"imesettings.example", assets.c_str(),
                                                              COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        if (FAILED(query))
            return FailWebView(hwnd, L"映射本机设置页面", query);
        const std::filesystem::path skins = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
        std::error_code ec;
        std::filesystem::create_directories(skins, ec);
        g_webview3->SetVirtualHostNameToFolderMapping(L"candidate-skins.example", skins.c_str(),
                                                      COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }
    if (SUCCEEDED(g_controller.As(&g_controller2)))
    {
        ApplySettingsChromeTheme(hwnd);
    }

    HRESULT hr = EnsureCompositionTree(hwnd);
    if (FAILED(hr))
        return FailWebView(hwnd, L"创建 DirectComposition 视觉树", hr);
    hr = g_composition_controller->put_RootVisualTarget(g_dcomp_root.Get());
    if (FAILED(hr))
        return FailWebView(hwnd, L"连接 WebView2 视觉树", hr);
    g_dcomp_device->Commit();

    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    g_controller->put_Bounds(bounds);

    EventRegistrationToken token{};
    g_webview->add_ContentLoading(
        Callback<ICoreWebView2ContentLoadingEventHandler>([](ICoreWebView2 *,
                                                             ICoreWebView2ContentLoadingEventArgs *) -> HRESULT {
            TraceStartup("ui: ContentLoading");
            g_last_config_message.clear();
            g_webview_content_started = true;
            SettingsSplash::Dismiss();
            return S_OK;
        }).Get(),
        &token);
    g_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>([hwnd](ICoreWebView2 *,
                                                                      ICoreWebView2NavigationCompletedEventArgs *args)
                                                                   -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            TraceStartup("ui: NavigationCompleted");
            SettingsSplash::Dismiss();
            if (!success)
                return FailWebView(hwnd, L"加载本机设置页面", HRESULT_FROM_WIN32(ERROR_BAD_NET_RESP));
            if (success)
            {
                PostWindowState(hwnd);
                PostConfig();
                ApplyWindowActivationAppearance();
                SetFocus(hwnd);
                if (g_open_personal_on_ready)
                {
                    ShowPersonalSection();
                    g_open_personal_on_ready = false;
                }
                if (g_open_reply_on_ready)
                {
                    ShowReplyStudioSection();
                    g_open_reply_on_ready = false;
                }
                if (g_open_about_on_ready)
                {
                    ShowAboutSection();
                    g_open_about_on_ready = false;
                }
            }
            return S_OK;
        }).Get(),
        &token);
    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>([hwnd](ICoreWebView2 *,
                                                                     ICoreWebView2WebMessageReceivedEventArgs *args)
                                                                  -> HRESULT {
            HandleWebMessage(hwnd, args);
            return S_OK;
        }).Get(),
        &token);
    hr = g_webview->Navigate(L"https://imesettings.example/index.html");
    return FAILED(hr) ? FailWebView(hwnd, L"开始导航设置页面", hr) : hr;
}

void InitWebView(HWND hwnd)
{
    TraceStartup("ui: InitWebView begin");
    std::filesystem::path user_data = CommonUtils::get_webview2_user_data_path(L"webview2-settings");
    TraceStartup("ui: get_webview2_user_data_path end");
    std::error_code ec;
    std::filesystem::create_directories(user_data, ec);
    if (ec)
    {
        FailWebView(hwnd, L"创建设置缓存目录", HRESULT_FROM_WIN32(ec.value()));
        return;
    }
    auto options = Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--disable-features=TranslateUI "
                                            L"--disable-background-networking "
                                            L"--disable-default-apps "
                                            L"--disable-sync "
                                            L"--disable-prompt-on-repost "
                                            L"--no-first-run");
    const HRESULT start = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                TraceStartup("ui: environment created");
                if (g_closing || !IsWindow(hwnd))
                    return E_ABORT;
                if (FAILED(result) || !environment)
                {
                    return FailWebView(hwnd, L"创建 WebView2 环境（异步）", result);
                }
                ComPtr<ICoreWebView2Environment3> environment3;
                const HRESULT query = environment->QueryInterface(IID_PPV_ARGS(&environment3));
                if (FAILED(query))
                    return FailWebView(hwnd, L"获取 WebView2 环境接口", query);
                const HRESULT create = environment3->CreateCoreWebView2CompositionController(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                        [hwnd](HRESULT controller_result, ICoreWebView2CompositionController *controller) -> HRESULT {
                            return OnControllerCreated(hwnd, controller_result, controller);
                        })
                        .Get());
                return FAILED(create) ? FailWebView(hwnd, L"开始创建 WebView2 控制器（同步）", create) : create;
            })
            .Get());
    if (FAILED(start))
        FailWebView(hwnd, L"开始创建 WebView2 环境（同步）", start);
}

UINT32 MouseKeys(WPARAM w_param)
{
    UINT32 keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
    if (w_param & MK_LBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
    if (w_param & MK_RBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
    if (w_param & MK_MBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
    if (w_param & MK_SHIFT)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_SHIFT;
    if (w_param & MK_CONTROL)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_CONTROL;
    if (w_param & MK_XBUTTON1)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON1;
    if (w_param & MK_XBUTTON2)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON2;
    return keys;
}

bool ForwardMouse(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    if (!g_composition_controller)
        return false;
    COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
    UINT32 data = 0;
    switch (message)
    {
    case WM_MOUSEMOVE:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
        break;
    case WM_LBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
        SetFocus(hwnd);
        break;
    case WM_LBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
        break;
    case WM_LBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK;
        break;
    case WM_RBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
        break;
    case WM_RBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
        break;
    case WM_RBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOUBLE_CLICK;
        break;
    case WM_MBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
        break;
    case WM_MBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
        break;
    case WM_MBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOUBLE_CLICK;
        break;
    case WM_XBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN;
        data = GET_XBUTTON_WPARAM(w_param);
        break;
    case WM_XBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP;
        data = GET_XBUTTON_WPARAM(w_param);
        break;
    case WM_XBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOUBLE_CLICK;
        data = GET_XBUTTON_WPARAM(w_param);
        break;
    case WM_MOUSEWHEEL:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
        data = GET_WHEEL_DELTA_WPARAM(w_param);
        break;
    case WM_MOUSEHWHEEL:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL;
        data = GET_WHEEL_DELTA_WPARAM(w_param);
        break;
    case WM_MOUSELEAVE:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE;
        break;
    default:
        return false;
    }
    POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
        ScreenToClient(hwnd, &point);
    if (message == WM_MOUSEMOVE)
    {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
    }
    g_composition_controller->SendMouseInput(
        kind, static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(MouseKeys(w_param)), data, point);
    return true;
}

bool IsMaximizeButton(HWND hwnd, POINT screen_point)
{
    if (!g_has_maximize_button_rect)
        return false;
    ScreenToClient(hwnd, &screen_point);
    return PtInRect(&g_maximize_button_rect, screen_point) != FALSE;
}

int TopNonClientInset(HWND hwnd)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT caption{}, no_caption{};
    AdjustWindowRectExForDpi(&caption, style, FALSE, ex_style, dpi);
    AdjustWindowRectExForDpi(&no_caption, style & ~WS_CAPTION, FALSE, ex_style, dpi);
    return no_caption.top - caption.top + GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// 首帧之前隐藏 WebView2 会卡住 raster 初始化，预热阶段不要碰可见性。
void SetWebViewVisible(bool visible)
{
    if (g_controller && g_webview_content_started)
        g_controller->put_IsVisible(visible ? TRUE : FALSE);
}

// 驻留期间让 WebView2 按低内存目标释放缓存和 GPU 资源，重新露面时恢复正常。
void SetWebViewMemoryTarget(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL level)
{
    ComPtr<ICoreWebView2_19> webview19;
    if (g_webview && SUCCEEDED(g_webview.As(&webview19)))
        webview19->put_MemoryUsageTargetLevel(level);
}

// 重新露面：取消延迟退出，恢复渲染与配置轮询。
void CancelLinger(HWND hwnd)
{
    if (!g_lingering)
        return;
    g_lingering = false;
    KillTimer(hwnd, kLingerTimer);
    SetWebViewMemoryTarget(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
    if (g_webview3)
        g_webview3->Resume();
    SetWebViewVisible(true);
    SetTimer(hwnd, kConfigReloadTimer, 300, nullptr);
}

void ActivateWindow(HWND hwnd)
{
    if (g_closing)
        return;
    CancelLinger(hwnd);
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    else
        ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

// 驻留时长（毫秒）：0 表示关闭即退出，std::nullopt 表示一直驻留。
std::optional<UINT> SettingsLingerTimeoutMs(const std::string &linger)
{
    if (linger == "forever")
        return std::nullopt;
    if (linger == "off")
        return 0u;
    unsigned minutes = 10;
    if (linger == "1m")
        minutes = 1;
    else if (linger == "5m")
        minutes = 5;
    else if (linger == "30m")
        minutes = 30;
    else if (linger == "60m")
        minutes = 60;
    return minutes * 60u * 1000u;
}

// 用户关闭窗口时走这里：只隐藏，进程继续持有导航完成的 WebView2，驻留到
// appearance.settings_window_linger 指定的时长才真正退出，驻留期间重新打开不用再冷启动。
// 隐藏期间停掉配置轮询、让 controller 不可见并冻结页面，避免白拿 CPU 和内存。
// 首帧还没出来就被关掉，说明用户不想等这次冷启动，直接退出，别留一个半初始化的进程。
void HideSettings(HWND hwnd)
{
    Vision::Stop();
    VoiceStudio::Stop();
    if (g_closing)
        return;
    if (g_chat_reader)
        g_chat_reader->Submit({{"action", "cancel"}}, [](const nlohmann::json &) {});
    if (g_chat_model)
        g_chat_model->Submit({{"action", "cancel"}}, [](const nlohmann::json &) {});
    if (g_webview)
        g_webview->ExecuteScript(L"window.dispatchEvent(new Event('msime-chat-hidden'));", nullptr);
    const std::optional<UINT> linger_ms = SettingsLingerTimeoutMs(g_settings_window_linger);
    if (!g_webview_content_started || linger_ms == 0u)
    {
        CloseSettings(hwnd);
        return;
    }
    if (g_lingering)
    {
        ShowWindow(hwnd, SW_HIDE);
        return;
    }
    g_lingering = true;
    // 窗口隐藏时保留的那一帧会在下次打开时先显示出来，带着高亮的标题栏按钮就会闪一下。
    // 页面自己的关闭按钮已经在点击时清过，这里覆盖 Alt+F4、启动图的关闭按钮等不经过页面的路径。
    ResetTitlebarHoverAfterVisibilityChange();
    // 隐藏的窗口收不到 WM_NCMOUSELEAVE，本地这份悬停状态要手动清掉，
    // 否则下次真的移进去时不会再发 enter。
    g_maximize_button_hover = false;
    KillTimer(hwnd, kConfigReloadTimer);
    SettingsSplash::Dismiss();
    ShowWindow(hwnd, SW_HIDE);
    SetWebViewVisible(false);
    // TrySuspend 要求 controller 已不可见；冻结页面脚本和定时器，重开时由 CancelLinger 的 Resume 恢复。
    SetWebViewMemoryTarget(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    if (g_webview3)
        g_webview3->TrySuspend(
            Callback<ICoreWebView2TrySuspendCompletedHandler>([](HRESULT, BOOL) -> HRESULT { return S_OK; }).Get());
    if (linger_ms)
        SetTimer(hwnd, kLingerTimer, *linger_ms, nullptr);
}

void CloseSettings(HWND hwnd)
{
    Vision::Stop();
    VoiceStudio::Stop();
    if (g_closing)
        return;
    g_closing = true;
    g_lingering = false;
    KillTimer(hwnd, kLingerTimer);
    KillTimer(hwnd, kConfigReloadTimer);
    SettingsSplash::Dismiss();
    ShowWindow(hwnd, SW_HIDE);
    if (g_reply_studio)
        g_reply_studio->Stop();
    if (g_chat_model)
        g_chat_model->Stop();
    if (g_chat_reader)
        g_chat_reader->Stop();
    if (g_worker)
        g_worker->Stop();
    else
        DestroyWindow(hwnd);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        // Keep native chrome aligned with the page's system-theme media query.
        // Do not rewrite any global or independent theme preference.
        PostConfig(false);
        break;
    case kActivateExistingWindow:
        ActivateWindow(hwnd);
        return 0;
    case kOpenAboutSection:
        ActivateWindow(hwnd);
        ShowAboutSection();
        return 0;
    case kOpenPersonalSection:
        ActivateWindow(hwnd);
        ShowPersonalSection();
        return 0;
    case kOpenReplyStudioSection:
        ActivateWindow(hwnd);
        ShowReplyStudioSection();
        return 0;
    case kScanSkinCatalog:
        PostConfig(true);
        return 0;
    case WM_NCCALCSIZE:
        if (w_param)
        {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(l_param);
            const LRESULT result = DefWindowProcW(hwnd, message, w_param, l_param);
            params->rgrc[0].top -= IsZoomed(hwnd) ? GetSystemMetrics(SM_CYCAPTION) : TopNonClientInset(hwnd);
            return result;
        }
        break;
    case WM_NCHITTEST: {
        const LRESULT result = DefWindowProcW(hwnd, message, w_param, l_param);
        POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        return result == HTCLIENT && IsMaximizeButton(hwnd, point) ? HTMAXBUTTON : result;
    }
    case WM_MOVE:
    case WM_MOVING:
        if (g_controller)
            g_controller->NotifyParentWindowPositionChanged();
        if (SettingsSplash::IsVisible())
            SettingsSplash::SyncToOwner();
        break;
    case WM_SETFOCUS:
        if (g_controller)
            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        break;
    case WM_ACTIVATE: {
        const bool active = LOWORD(w_param) != WA_INACTIVE;
        if (g_window_active != active)
        {
            g_window_active = active;
            ApplyWindowActivationAppearance();
        }
        if (active)
        {
            if (g_webview3)
                g_webview3->Resume();
            if (g_controller)
                g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        break;
    }
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (ForwardMouse(hwnd, message, w_param, l_param))
            return (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK) ? TRUE : 0;
        break;
    case WM_SETCURSOR:
        if (g_composition_controller && LOWORD(l_param) == HTCLIENT)
        {
            UINT32 cursor = 0;
            if (SUCCEEDED(g_composition_controller->get_SystemCursorId(&cursor)) && cursor)
            {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(cursor)));
                return TRUE;
            }
        }
        break;
    case WM_NCMOUSEMOVE: {
        POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        if (IsMaximizeButton(hwnd, point))
        {
            if (!g_maximize_button_hover)
            {
                g_maximize_button_hover = true;
                PostMaximizeButtonEvent("enter");
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        break;
    }
    case WM_NCMOUSELEAVE:
        if (g_maximize_button_hover)
        {
            g_maximize_button_hover = false;
            PostMaximizeButtonEvent("leave");
        }
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP: {
        POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        if (IsMaximizeButton(hwnd, point))
        {
            PostMaximizeButtonEvent(message == WM_NCLBUTTONDOWN ? "down" : "up");
            return 0;
        }
        break;
    }
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(hwnd, &client);
        const bool light = g_settings_light;
        HBRUSH brush = CreateSolidBrush(light ? RGB(243, 243, 243) : RGB(32, 32, 32));
        FillRect(reinterpret_cast<HDC>(w_param), &client, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(l_param);
        const UINT dpi = GetDpiForWindow(hwnd);
        info->ptMinTrackSize = {MulDiv(600, dpi, 96), MulDiv(400, dpi, 96)};
        return 0;
    }
    case WM_SIZE: {
        const bool minimized = w_param == SIZE_MINIMIZED;
        if (minimized || g_window_minimized)
            ResetTitlebarHoverAfterVisibilityChange();
        g_window_minimized = minimized;
        if (g_controller)
        {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            g_controller->put_Bounds(bounds);
            PostWindowState(hwnd);
        }
        if (SettingsSplash::IsVisible())
            SettingsSplash::SyncToOwner();
        break;
    }
    case WM_TIMER:
        if (w_param == kLingerTimer)
        {
            CloseSettings(hwnd);
            return 0;
        }
        if (w_param == kConfigReloadTimer && g_worker && !g_closing && !g_reload_pending)
        {
            g_reload_pending = true;
            SubmitSettingsTask({"configReload", true}, [] {
                SerialTaskQueue::Completion completion;
                try
                {
                    if (ReloadImeConfigIfChanged())
                        completion = ConfigCompletion();
                }
                catch (const nlohmann::json::exception &error)
                {
                    const SettingsTaskDiagnostics::Failure failure{"json", error.id};
                    TraceSettingsTask({"configReload", true}, "reload.retry", &failure);
                }
                catch (...)
                {
                    const SettingsTaskDiagnostics::Failure failure{"standard", 0};
                    TraceSettingsTask({"configReload", true}, "reload.retry", &failure);
                }
                return [completion = std::move(completion)] {
                    g_reload_pending = false;
                    if (completion)
                        completion();
                };
            });
        }
        return 0;
    case kWorkerCompleted:
        if (g_worker)
            g_worker->Drain(!g_closing);
        if (g_reply_studio)
            g_reply_studio->Drain(!g_closing);
        if (g_chat_model)
            g_chat_model->Drain(!g_closing);
        if (g_chat_reader)
            g_chat_reader->Drain(!g_closing);
        if (g_closing && (!g_worker || g_worker->Stopped()) && (!g_reply_studio || g_reply_studio->Stopped()) &&
            (!g_chat_reader || g_chat_reader->Stopped()) && (!g_chat_model || g_chat_model->Stopped()))
            DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        Vision::Stop();
        HideSettings(hwnd);
        return 0;
    case kQuitSettings:
        CloseSettings(hwnd);
        return 0;
    case WM_DESTROY:
        AiWorkbench::Cancel();
        Vision::Stop();
        if (g_reply_studio)
            g_reply_studio->Stop();
        if (g_chat_model)
            g_chat_model->Stop();
        if (g_chat_reader)
            g_chat_reader->Stop();
        g_closing = true;
        g_lingering = false;
        if (g_worker)
            g_worker->Stop();
        KillTimer(hwnd, kLingerTimer);
        KillTimer(hwnd, kConfigReloadTimer);
        SettingsSplash::Dismiss();
        g_webview.Reset();
        g_controller2.Reset();
        g_controller.Reset();
        g_composition_controller.Reset();
        g_dcomp_root.Reset();
        g_dcomp_target.Reset();
        g_dcomp_device.Reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w_param, l_param);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command)
{
    g_temporary_configuration = SettingsEnvironment::HasTemporaryConfiguration();
    TraceStartup("wWinMain");
    g_open_personal_on_ready =
        command_line && std::wstring(command_line).find(L"--personal-library") != std::wstring::npos;
    g_open_reply_on_ready = command_line && std::wstring(command_line).find(L"--reply-studio") != std::wstring::npos;
    g_open_about_on_ready = command_line && std::wstring(command_line).find(L"--about") != std::wstring::npos;
    TraceStartup("ui: DPI begin");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    TraceStartup("ui: DPI end; COM begin");
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    TraceStartup(SUCCEEDED(com_result) ? "ui: COM complete" : "ui: COM failed");
    if (FAILED(com_result))
        return 1;

    TraceStartup("ui: single-instance mutex begin");
    CommonUtils::SingleInstanceGuard single_instance(kSingleInstanceMutex);
    TraceStartup("ui: single-instance mutex complete");
    if (!single_instance.is_valid())
        return 1;
    if (single_instance.already_running())
    {
        TraceStartup("ui: existing Settings instance detected");
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing)
        {
            PostMessageW(existing,
                         g_open_personal_on_ready ? kOpenPersonalSection
                         : g_open_reply_on_ready
                             ? kOpenReplyStudioSection
                             : (g_open_about_on_ready ? kOpenAboutSection : kActivateExistingWindow),
                         0, 0);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    TraceStartup("ui: window class begin");
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_IME_ICON));
    window_class.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SETTINGS_ICON));
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class))
        return 1;
    TraceStartup("ui: window class complete");

    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(900, dpi, 96);
    const int height = MulDiv(710, dpi, 96);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    TraceStartup("ui: CreateWindow begin");
    HWND hwnd = CreateWindowExW(0, kWindowClass, g_temporary_configuration ? kTestWindowTitle : kWindowTitle,
                                WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, instance, nullptr);
    TraceStartup(hwnd ? "ui: CreateWindow complete" : "ui: CreateWindow failed");
    if (!hwnd)
        return 1;
    g_settings_hwnd = hwnd;
    g_worker = std::make_unique<SerialTaskQueue>([hwnd] { PostMessageW(hwnd, kWorkerCompleted, 0, 0); },
                                                 [] {
                                                     const SettingsTaskDiagnostics::Context context{"unknown", true};
                                                     const SettingsTaskDiagnostics::Failure failure;
                                                     TraceSettingsTask(context, "queue.unwrappedFailure", &failure);
                                                     ReportSettingsTaskError(context, failure);
                                                 },
                                                 [] {
                                                     if (g_worker_com_initialized)
                                                         CoUninitialize();
                                                 });
    auto initial_theme = std::make_shared<std::promise<bool>>();
    std::future<bool> initial_light = initial_theme->get_future();
    SubmitSettingsTask({"initialConfig", false}, [initial_theme] {
        g_worker_com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        TraceStartup("worker: InitImeConfig begin");
        InitImeConfig();
        TraceStartup("worker: InitImeConfig end");
        initial_theme->set_value(ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light");
        return ConfigCompletion();
    });
    // 配置加载通常只要几毫秒，先短暂等它拿到主题再画窗口框架和 splash，免得浅色主题先闪一下深色。
    // 等不到就按深色先亮出来，之后由 ConfigCompletion 切换。
    if (initial_light.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready)
    {
        try
        {
            g_settings_light = initial_light.get();
        }
        catch (...)
        {
        }
    }

    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    ApplySettingsChromeTheme(hwnd);
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    MARGINS margins{};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // CreateWindow sends the initial WM_NCCALCSIZE before the DWM/custom-frame
    // attributes above are installed. Force a second frame calculation now;
    // otherwise the native caption remains until the first minimize/restore.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    SetTimer(hwnd, kConfigReloadTimer, 300, nullptr);
    // 先把窗口和 splash 亮出来，再做 WebView2 用户数据目录的准备和 environment 创建。
    ShowWindow(hwnd, show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
    UpdateWindow(hwnd);
    SettingsSplash::Show(hwnd, g_settings_light);
    TraceStartup("ui: window + splash shown");
    InitWebView(hwnd);
    TraceStartup("ui: InitWebView returned");
    if (g_webview_content_started)
        SettingsSplash::Dismiss();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_worker.reset(); // Drain accepted writes even if the window was destroyed externally.
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
