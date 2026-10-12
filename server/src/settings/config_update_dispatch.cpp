// Configuration JSON dispatch shared with the Settings host and its native regression tests.
#include "settings/config_update_dispatch.h"
#include "config/ime_config.h"
#include "english/speech_service.h"
#include "settings/vision_config.h"
#include <string>
#include <string_view>
#include <vector>

namespace json = boost::json;
namespace SettingsConfig
{
std::optional<bool> ApplyCandidateAnnotationUpdate(const json::object &data)
{
    const auto *path = data.if_contains("path");
    if (!path || !path->is_string() || path->as_string() != "appearance.candidate_show_pinyin")
        return std::nullopt;
    const auto *value = data.if_contains("value");
    return value && value->is_bool() && SetConfiguredCandidateShowPinyin(value->as_bool());
}

bool ApplyConfigUpdate(const json::object &data)
{
    if (const auto annotation = ApplyCandidateAnnotationUpdate(data); annotation.has_value())
        return *annotation;
    const std::string path = json::value_to<std::string>(data.at("path"));
    if (path == "input.english_candidates")
        return SetConfiguredEnglishCandidates(json::value_to<bool>(data.at("value")));
    if (path == "input.english_candidate_app_rules")
        return SetConfiguredEnglishCandidateAppRules(json::value_to<std::string>(data.at("value")));
    if (path == "input.wubi_four_code_auto_commit")
        return SetConfiguredWubiFourCodeAutoCommit(json::value_to<bool>(data.at("value")));
    if (path == "input.wubi_fifth_code_top_commit")
        return SetConfiguredWubiFifthCodeTopCommit(json::value_to<bool>(data.at("value")));
    if (path == "input.vim_mode_apps")
        return SetConfiguredVimModeApps(json::value_to<std::string>(data.at("value")));
    if (path == "input.enabled_domain_dictionaries")
        return SetConfiguredEnabledDomainDictionaries(json::value_to<std::string>(data.at("value")));
    if (path == "keybindings.trilingual_cycle")
        return SetConfiguredTrilingualCycleEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.floating_toolbar_shadow")
        return SetConfiguredFloatingToolbarShadow(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_fit_content")
        return SetConfiguredCandidateFitContentMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.mode")
        return SetConfiguredInputMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.schema")
        return SetConfiguredInputScheme(json::value_to<std::string>(data.at("value")));
    if (path == "input.japanese_schema")
        return SetConfiguredJapaneseSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.character_set")
        return SetConfiguredCharacterSet(json::value_to<std::string>(data.at("value")));
    if (path == "input.default_ime_mode")
        return SetConfiguredDefaultImeMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.ime_mode_scope")
        return SetConfiguredImeModeScope(json::value_to<std::string>(data.at("value")));
    if (path == "input.shuangpin_schema")
        return SetConfiguredShuangpinSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.wubi_schema")
        return SetConfiguredWubiSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.wubi_mixed_pinyin")
        return SetConfiguredWubiMixedPinyin(json::value_to<bool>(data.at("value")));
    if (path == "input.escape_keeps_selected_word")
        return SetConfiguredEscapeKeepsSelectedWord(json::value_to<bool>(data.at("value")));
    if (path == "input.enter_learns_english_word")
        return SetConfiguredEnterLearnsEnglishWord(json::value_to<bool>(data.at("value")));
    if (path == "input.wubi_z_mode")
        return SetConfiguredWubiZMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.word_to_character")
        return SetConfiguredWordToCharacterEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.word_to_character_keys")
        return SetConfiguredWordToCharacterKeys(json::value_to<std::string>(data.at("value")));
    if (path == "input.smart_punctuation")
        return SetConfiguredSmartPunctuationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_space_convert")
        return SetConfiguredSmartPunctuationSpaceConvertEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_direct_digit")
        return SetConfiguredSmartPunctuationDirectDigitEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_direct_letter")
        return SetConfiguredSmartPunctuationDirectLetterEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_repeat_to_chinese")
        return SetConfiguredSmartPunctuationRepeatToChineseEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.paired_punctuation")
        return SetConfiguredPairedPunctuationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.punctuation_lock")
        return SetConfiguredPunctuationLock(json::value_to<std::string>(data.at("value")));
    constexpr std::string_view input_section_prefix = "input.";
    constexpr std::string_view fuzzy_prefix = "input.fuzzy_";
    // 精确分支必须在前：总开关键同样命中 fuzzy_ 前缀，不能靠 prefix setter 拒绝它。
    if (path == "input.fuzzy_pinyin")
        return SetConfiguredFuzzyPinyinEnabled(json::value_to<bool>(data.at("value")));
    if (path.rfind(fuzzy_prefix, 0) == 0)
        return SetConfiguredFuzzyPinyinRule(path.substr(input_section_prefix.size()),
                                            json::value_to<bool>(data.at("value")));
    if (path == "appearance.tsf_preedit_style")
        return SetConfiguredTsfPreeditStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.tsf_preedit_shuangpin_quanpin")
        return SetConfiguredTsfPreeditShuangpinQuanpin(json::value_to<bool>(data.at("value")));
    if (path == "appearance.ui_backend")
        return SetConfiguredUiBackend(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.settings_window_linger")
        return SetConfiguredSettingsWindowLinger(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_layout")
        return SetConfiguredCandidateWindowLayout(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_follow_cursor")
        return SetConfiguredCandidateWindowFollowCursor(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_skin")
        return SetConfiguredCandidateSkin(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_preedit_style")
        return SetConfiguredCandidateWindowPreeditStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_preedit_shuangpin_quanpin")
        return SetConfiguredCandidateWindowPreeditShuangpinQuanpin(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_fixed_badge")
        return SetConfiguredCandidateFixedBadge(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_fixed_badge_style")
        return SetConfiguredCandidateFixedBadgeStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.page_size")
        return SetConfiguredCandidatePageSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.font")
        return SetConfiguredCandidateFont(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.fallback_fonts")
        return SetConfiguredCandidateFallbackFonts(
            json::value_to<std::vector<std::string>>(json::parse(json::value_to<std::string>(data.at("value")))));
    if (path == "appearance.english_font")
        return SetConfiguredCandidateEnglishFont(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.font_size")
        return SetConfiguredCandidateFontSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.candidate_window_preedit_font_size")
        return SetConfiguredCandidateWindowPreeditFontSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.cand_text_color")
        return SetConfiguredCandidateTextColor(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_mode")
        return SetConfiguredThemeMode(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_settings")
        return SetConfiguredThemeSettings(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_cand")
        return SetConfiguredThemeCand(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_ftb")
        return SetConfiguredThemeFtb(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_menu")
        return SetConfiguredThemeMenu(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_emoji")
        return SetConfiguredThemeEmoji(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_screen_keyboard")
        return SetConfiguredThemeScreenKeyboard(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_handwriting")
        return SetConfiguredThemeHandwriting(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_voice")
        return SetConfiguredThemeVoice(json::value_to<std::string>(data.at("value")));
    if (path == "general.floating_toolbar")
        return SetConfiguredFloatingToolbarEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.caret_state_indicator")
        return SetConfiguredCaretStateIndicatorEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.caret_state_indicator_on_focus")
        return SetConfiguredCaretStateIndicatorOnFocus(json::value_to<bool>(data.at("value")));
    if (path == "general.caret_state_indicator_position")
        return SetConfiguredCaretStateIndicatorPosition(json::value_to<std::string>(data.at("value")));
    if (path == "general.diagnostic_log" || path == "general.candidate_window_diagnostic_log")
        return SetConfiguredDiagnosticLogEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.tsf_diagnostic_log")
        return SetConfiguredTsfDiagnosticLogEnabled(json::value_to<bool>(data.at("value")));
    if (path == "vocabulary.enabled")
        return SetConfiguredVocabularyEnabled(json::value_to<bool>(data.at("value")));
    if (path == "vocabulary.retention_days")
        return SetConfiguredVocabularyRetentionDays(json::value_to<int>(data.at("value")));
    if (path == "statistics.enabled")
        return SetConfiguredStatisticsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "statistics.retention")
        return SetConfiguredStatisticsRetention(json::value_to<std::string>(data.at("value")));
    if (path == "general.floating_toolbar_scale")
    {
        const json::value &value = data.at("value");
        const double scale = value.is_double() ? value.as_double() : static_cast<double>(value.as_int64());
        return SetConfiguredFloatingToolbarScale(scale);
    }
    if (path == "general.floating_toolbar_font_size")
        return SetConfiguredFloatingToolbarFontSize(static_cast<int>(data.at("value").as_int64()));
    // Must precede the item prefix below, which would reject these as unknown items.
    if (path == "general.floating_toolbar_auto_hide")
        return SetConfiguredFloatingToolbarAutoHide(json::value_to<bool>(data.at("value")));
    if (path == "general.floating_toolbar_auto_hide_delay")
        return SetConfiguredFloatingToolbarAutoHideDelay(static_cast<int>(data.at("value").as_int64()));
    constexpr std::string_view toolbar_prefix = "general.floating_toolbar_";
    if (path.rfind(toolbar_prefix, 0) == 0)
        return SetConfiguredFloatingToolbarItemEnabled(path.substr(toolbar_prefix.size()),
                                                       json::value_to<bool>(data.at("value")));
    if (path == "general.cn_en_mixed_input")
        return SetConfiguredEnglishCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_translation_cloud")
        return SetConfiguredCandidateTranslationCloudEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.disabled_translation_dictionaries")
        return SetConfiguredDisabledTranslationDictionaries(json::value_to<std::string>(data.at("value")));
    if (path == "general.personal_translation_glossary")
        return SetConfiguredPersonalTranslationGlossary(json::value_to<std::string>(data.at("value")));
    if (path == "general.candidate_translations")
        return SetConfiguredCandidateTranslationsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.cn_en_mixed_input_min_chars")
        return SetConfiguredEnglishMixedInputMinChars(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.emoji_mixed_input")
        return SetConfiguredEmojiMixedInputEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.kaomoji_mixed_input")
        return SetConfiguredKaomojiMixedInputEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.cloud_candidates")
        return SetConfiguredCloudCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_wordlattice")
        return SetConfiguredAssocSentenceWordLattice(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_google")
        return SetConfiguredAssocSentenceGoogle(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_neural_desktop")
        return SetConfiguredAssocSentenceNeuralDesktop(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_neural_keyboard")
        return SetConfiguredAssocSentenceNeuralKeyboard(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_show_next_on_duplicate")
        return SetConfiguredAssocSentenceShowNextOnDuplicate(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_source_badge")
        return SetConfiguredAssocSentenceSourceBadge(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_collocation_enabled")
        return SetConfiguredAssocSentenceCollocationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_collocation_model")
        return SetConfiguredAssocSentenceCollocationModel(json::value_to<std::string>(data.at("value")));
    if (path == "utility.unicode_mode")
        return SetConfiguredUnicodeModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.quick_phrase")
        return SetConfiguredQuickPhraseEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.quick_phrase_candidates")
        return SetConfiguredQuickPhraseCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.quick_phrase_frequency")
        return SetConfiguredQuickPhraseFrequencyEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.mixed_candidates")
        return SetConfiguredMixedCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.date_time_candidates")
        return SetConfiguredDateTimeCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.date_time_menu")
        return SetConfiguredDateTimeMenuEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.date_time_mode")
        return SetConfiguredDateTimeModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.emoji_mode")
        return SetConfiguredEmojiModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.kaomoji_mode")
        return SetConfiguredKaomojiModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.jianpin_mode")
        return SetConfiguredJianpinModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.y_mode")
        return SetConfiguredYModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.r_mode")
        return SetConfiguredRModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.v_mode")
        return SetConfiguredVModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.clipboard_history")
        return SetConfiguredClipboardHistoryEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_minus_equal")
        return SetConfiguredPagingMinusEqualEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_tab")
        return SetConfiguredPagingTabEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_comma_period")
        return SetConfiguredPagingCommaPeriodEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_brackets")
        return SetConfiguredPagingBracketsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_page_up_down")
        return SetConfiguredPagingPageUpDownEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_mouse_wheel")
        return SetConfiguredPagingMouseWheelEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_arrow_navigation")
        return SetConfiguredCandidateArrowNavigationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_shift")
        return SetConfiguredSwitchLanguageShiftEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_ctrl")
        return SetConfiguredSwitchLanguageCtrlEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_ctrl_alt_space")
        return SetConfiguredSwitchLanguageCtrlAltSpaceEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.toggle_character_set_ctrl_shift_f")
        return SetConfiguredCharacterSetShortcutEnabled(json::value_to<bool>(data.at("value")));
    if (path.rfind("frequency_adjustment.", 0) == 0)
    {
        const std::string key = path.substr(21);
        const json::value &value = data.at("value");
        if (value.is_string())
            return SetConfiguredFrequencyAdjustmentString(key, json::value_to<std::string>(value));
        if (value.is_int64())
            return SetConfiguredFrequencyAdjustmentInt(key, static_cast<int>(value.as_int64()));
        return false;
    }
    if (path.rfind("speech.", 0) == 0)
        return data.at("value").is_string() &&
               Speech::SetConfig(path.substr(7), json::value_to<std::string>(data.at("value")));
    if (path.rfind("voice_input.", 0) == 0)
    {
        const std::string key = path.substr(12);
        const json::value &value = data.at("value");
        if (value.is_bool())
        {
            return SetConfiguredVoiceInputBool(key, json::value_to<bool>(value));
        }
        if (value.is_string())
            return SetConfiguredVoiceInputString(key, json::value_to<std::string>(value));
        return false;
    }
    if (path.rfind("vision.", 0) == 0)
        return data.at("value").is_string() &&
               VisionConfig::Set(path.substr(7), json::value_to<std::string>(data.at("value")));
    if (path.rfind("ai_assistant.", 0) == 0)
    {
        const std::string key = path.substr(13);
        const json::value &value = data.at("value");
        if (value.is_bool())
            return SetConfiguredAiAssistantBool(key, json::value_to<bool>(value));
        if (value.is_string())
            return SetConfiguredAiAssistantString(key, json::value_to<std::string>(value));
        if (value.is_int64())
            return SetConfiguredAiAssistantInt(key, static_cast<int>(value.as_int64()));
        return false;
    }
    if (path.rfind("candidate_translation.", 0) == 0)
    {
        const auto &value = data.at("value");
        return value.is_string() &&
               SetConfiguredCandidateTranslationString(path.substr(22), json::value_to<std::string>(value));
    }
    if (path.rfind("tencent_tmt.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredTencentTmtString(path.substr(std::string("tencent_tmt.").size()),
                                                                  json::value_to<std::string>(value));
    }
    if (path == "custom_translation.enabled")
        return SetConfiguredCustomTranslationBool("enabled", json::value_to<bool>(data.at("value")));
    if (path.rfind("custom_translation.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() &&
               SetConfiguredCustomTranslationString(path.substr(std::string("custom_translation.").size()),
                                                    json::value_to<std::string>(value));
    }
    if (path == "niutrans.enabled")
        return SetConfiguredNiuTransBool("enabled", json::value_to<bool>(data.at("value")));
    if (path.rfind("niutrans.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredNiuTransString(path.substr(std::string("niutrans.").size()),
                                                                json::value_to<std::string>(value));
    }
    if (path.rfind("network.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredNetworkString(path.substr(std::string("network.").size()),
                                                               json::value_to<std::string>(value));
    }
    if (path == "helpcode.show_sp_helpcode_in_candidate_window")
        return SetConfiguredShowShuangpinHelpcodeInCandidateWindow(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode")
        return SetConfiguredShuangpinHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_mid_sentence_helpcode")
        return SetConfiguredShuangpinMidSentenceHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_mid_sentence_helpcode_backtick")
        return SetConfiguredShuangpinMidSentenceHelpcodeBacktick(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_mid_sentence_helpcode_semicolon")
        return SetConfiguredShuangpinMidSentenceHelpcodeSemicolon(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_mid_sentence_helpcode_uppercase")
        return SetConfiguredShuangpinMidSentenceHelpcodeUppercase(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_direct_helpcode")
        return SetConfiguredShuangpinDirectHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_direct_helpcode_slash")
        return SetConfiguredShuangpinDirectHelpcodeSlash(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_direct_helpcode_uppercase")
        return SetConfiguredShuangpinDirectHelpcodeUppercase(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode_schema")
        return SetConfiguredShuangpinHelpcodeSchema(json::value_to<std::string>(data.at("value")));
    if (path == "helpcode.quanpin_helpcode")
        return SetConfiguredQuanpinHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.quanpin_helpcode_schema")
        return SetConfiguredQuanpinHelpcodeSchema(json::value_to<std::string>(data.at("value")));
    if (path == "helpcode.show_qp_helpcode_in_candidate_window")
        return SetConfiguredShowQuanpinHelpcodeInCandidateWindow(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_transposition")
        return SetConfiguredQuanpinAutocorrectTransposition(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_neighbor")
        return SetConfiguredQuanpinAutocorrectNeighbor(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_marker")
        return SetConfiguredQuanpinAutocorrectMarker(json::value_to<bool>(data.at("value")));
    return false;
}
} // namespace SettingsConfig
