//! 输入会话：对应 `engine/core/input_session*.cpp`。拥有一个 `ImeSession`，在它之上做按键处理、选词上屏、
//! 光标编辑、本地模式、混排、固定位置、造词与调频，宿主（各平台前端）只需把平台按键翻译成这里的调用。
//!
//! 文件分工与 C++ 一致：`mod.rs` 是类型、构造与选项；`keys.rs` 是按键、命令、选词、本地模式与学习；
//! `composition.rs` 是预编辑与切分的读出、选词后的推进、云查询状态；`candidates.rs` 是混排与固定位置；
//! `editing.rs` 是光标编辑与单元边界。
//!
//! 和 C++ 不同的几处：
//! - 辅助码方案按名字找文件（`set_helpcode_schema`）由宿主做，这里直接收码表（`set_helpcode_keymap`）；
//! - 没给时钟时 C++ 读系统本地时间，这里退回 UTC（宿主应当用 `set_local_date_time_provider` 给本地时间）；
//! - 构造时由调用方建好 `ImeSession`（词库路径、日志路径、语言模型都在那里）。

mod candidates;
mod composition;
mod editing;
mod keys;

use crate::candidate_queries::CandidateQueries;
pub use crate::candidate_queries::{EnglishInputOptions, LocalInputMode, MixedExpressiveOptions};
use crate::datetime::DateTime;
use crate::ime_session::{ImeSession, WubiInputOptions};
use crate::lattice::{Item, Source};
use crate::punctuation::PunctuationPolicy;
use crate::quanpin_dictionary::{QuanpinDictionary, SentenceAssociation};
use crate::request::{QueryRequest, SchemeType};
use crate::runtime_paths::RuntimeRoots;
use crate::shuangpin::Profile;
use std::sync::atomic::{AtomicU64, Ordering};

/// 一次按键或选词的结果：`handled` 告诉前端要不要吞掉按键，`commit` 是要插进客户端的文字。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct KeyResult {
    pub handled: bool,
    pub commit: Option<String>,
    pub diagnostic: Option<String>,
}

impl KeyResult {
    pub(crate) fn handled() -> Self {
        Self { handled: true, commit: None, diagnostic: None }
    }

    pub(crate) fn with(handled: bool, commit: Option<String>, diagnostic: Option<String>) -> Self {
        Self { handled, commit, diagnostic }
    }
}

/// 作用在现有组合上的编辑意图。可打印字符走 `handle_character`。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Command {
    Backspace,
    CommitCandidate,
    CommitRaw,
    Cancel,
    MoveLeft,
    MoveRight,
    MoveHome,
    MoveEnd,
    DeleteForward,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CandidateEdge {
    FirstHan,
    LastHan,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum FrequencyAdjustmentMode {
    #[default]
    Disabled,
    Pin,
    Halve,
    Linear,
    Promote,
}

impl FrequencyAdjustmentMode {
    pub(crate) fn name(self) -> &'static str {
        match self {
            FrequencyAdjustmentMode::Disabled => "disabled",
            FrequencyAdjustmentMode::Pin => "pin",
            FrequencyAdjustmentMode::Halve => "halve",
            FrequencyAdjustmentMode::Linear => "linear",
            FrequencyAdjustmentMode::Promote => "promote",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrequencyAdjustmentOptions {
    pub mode: FrequencyAdjustmentMode,
    pub trigger_count: i32,
    pub linear_step: i32,
}

impl Default for FrequencyAdjustmentOptions {
    fn default() -> Self {
        Self { mode: FrequencyAdjustmentMode::Disabled, trigger_count: 1, linear_step: 1 }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LocalModeOptions {
    pub unicode: bool,
    pub date_time: bool,
    /// Shift+K 快捷短语模式，按编码前缀查。
    pub quick_phrase: bool,
    /// 全拼/双拼里把编码精确匹配的快捷短语混进候选；五笔不混。
    pub quick_phrase_candidates: bool,
    /// 快捷短语参与调频：选择会把整组前移，越过它选普通候选会把它后移。关掉时组固定在首位。
    pub quick_phrase_frequency: bool,
    pub emoji: bool,
    pub kaomoji: bool,
    pub super_jianpin: bool,
    pub temporary_english: bool,
    pub temporary_japanese: bool,
}

impl Default for LocalModeOptions {
    fn default() -> Self {
        Self {
            unicode: true,
            date_time: true,
            quick_phrase: true,
            quick_phrase_candidates: true,
            quick_phrase_frequency: true,
            emoji: true,
            kaomoji: true,
            super_jianpin: true,
            temporary_english: true,
            temporary_japanese: true,
        }
    }
}

/// 给异步提供者（云、AI）的组合快照；前端原样带回，会话按活动组合重新校验。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct OnlineQuery {
    pub scheme: SchemeType,
    pub generation: u64,
    pub identity: String,
    pub query_text: String,
    pub cache_key: String,
    pub pinyin_segments: Vec<String>,
    pub cloud_eligible: bool,
    pub ai_eligible: bool,
    pub session_id: u64,
}

/// 一次选词之后组合怎么走。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SelectionTransition {
    pub continues_composition: bool,
    /// 刚选中的候选由五笔码表产出：上屏推进与造词都按它决定，五笔一次消耗整串编码。
    pub wubi_native: bool,
    pub full_pure_pinyin: String,
    pub current_segmentation: String,
    pub current_segmentation_with_cases: String,
    pub selected_canonical_pinyin: String,
    /// 这次选词消耗掉的原串，按用户敲的原样；不消耗输入的来源（云、联想、整词上屏）留空。
    pub consumed_raw_input_with_cases: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CloudQueryState {
    pub should_query: bool,
    pub query_text: String,
    pub cache_key: String,
    pub committed_pinyin: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CreatingWordProgress {
    pub pinyin: String,
    pub word: String,
    pub preedit: String,
    pub completed: bool,
    pub can_store: bool,
}

/// 双拼会话的两种预编辑切分：`raw` 是按键原串的切分（ni'hc），`quanpin` 是转换后的全拼切分（ni'hao）。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ShuangpinPreeditForms {
    pub raw: String,
    pub quanpin: String,
}

static NEXT_SESSION_ID: AtomicU64 = AtomicU64::new(1);

/// 一次响应同时属于一个会话和一代组合。
struct OnlineRequestGuard {
    id: u64,
    generation: u64,
}

impl OnlineRequestGuard {
    fn new() -> Self {
        Self { id: NEXT_SESSION_ID.fetch_add(1, Ordering::Relaxed), generation: 0 }
    }

    fn invalidate(&mut self) {
        self.generation += 1;
    }

    fn stamp(&self, query: &mut OnlineQuery) {
        query.session_id = self.id;
        query.generation = self.generation;
    }

    fn matches(&self, current: &OnlineQuery, incoming: &OnlineQuery) -> bool {
        incoming.session_id == self.id
            && incoming.generation == self.generation
            && current.scheme == incoming.scheme
            && current.identity == incoming.identity
            && current.query_text == incoming.query_text
            && current.cache_key == incoming.cache_key
            && current.pinyin_segments == incoming.pinyin_segments
            && current.cloud_eligible == incoming.cloud_eligible
            && current.ai_eligible == incoming.ai_eligible
    }
}

/// 引擎没有页长的概念，快捷短语组最多退到默认页长 6 的末位。
const QUICK_PHRASE_MAX_SLOT: i32 = 5;

pub struct InputSession<'a> {
    roots: RuntimeRoots,
    candidate_queries: CandidateQueries,
    engine: ImeSession<'a>,
    profile: &'static Profile,

    caret: Option<usize>,
    prefix_candidates: Vec<Item>,
    /// 前缀候选的缓存键（小写）；也供混排的英文/表情联想跟着正在转换的那段走。
    prefix_query_input: String,
    /// 为真时 `candidates()` 和混排读 `prefix_candidates` 而不是引擎。
    prefix_candidates_active: bool,
    fixed_positions_enabled: bool,

    immediate_phrase_progress: CreatingWordProgress,
    shuangpin_preedit_uses_raw: bool,
    pending_pinyin_sequence: String,
    pending_pinyin_sequence_with_cases: String,
    has_pending_pinyin_sequence: bool,
    has_pending_pinyin_sequence_with_cases: bool,

    /// 位掩码（autocorrect 的位），不是 bool：bool 会把邻键位截断丢失。
    quanpin_autocorrect_types: u32,
    quanpin_helpcode_enabled: bool,
    shuangpin_helpcode_enabled: bool,
    mid_sentence_helpcode_enabled: bool,
    direct_helpcode_enabled: bool,
    direct_helpcode_slash_marker: bool,
    direct_helpcode_uppercase_marker: bool,
    mid_sentence_uppercase_trigger_enabled: bool,
    chinese_punctuation_enabled: bool,
    candidate_learning_enabled: bool,
    punctuation: PunctuationPolicy,
    frequency_adjustment: FrequencyAdjustmentOptions,
    frequency_adjustment_configured: bool,
    local_mode_options: LocalModeOptions,
    english_input_options: EnglishInputOptions,
    mixed_expressive_options: MixedExpressiveOptions,
    dedicated_english_mode: bool,
    dedicated_english_preedit: String,
    dedicated_english_candidates: Vec<Item>,
    mixed_candidates: Vec<Item>,
    local_input_mode: LocalInputMode,
    temporary_original_scheme: Option<SchemeType>,
    local_preedit: String,
    local_candidates: Vec<Item>,
    local_date_time_provider: Option<Box<dyn Fn() -> DateTime>>,
    online_requests: OnlineRequestGuard,
    /// 整句落库用的另一份全拼词库连接（C++ 的 `canonical_phrase_engine_`）：第一次用到才开，写库不动查询引擎的缓存。
    canonical_phrase_engine: Option<QuanpinDictionary<'a>>,
}

/// 协调线程：没人给时钟时退到 UTC（C++ 读系统本地时间，时区要宿主给）。
fn utc_now() -> DateTime {
    let seconds = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let days = (seconds / 86_400) as i64;
    let second_of_day = seconds % 86_400;
    // civil_from_days（Howard Hinnant）。
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let day_of_era = z.rem_euclid(146_097);
    let year_of_era = (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_index = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_index + 2) / 5 + 1;
    let month = if month_index < 10 { month_index + 3 } else { month_index - 9 };
    let year = year_of_era + era * 400 + i64::from(month <= 2);
    DateTime {
        year: year as u32,
        month: month as u32,
        day: day as u32,
        weekday: ((days + 4).rem_euclid(7)) as u32,
        hour: (second_of_day / 3_600) as u32,
        minute: (second_of_day % 3_600 / 60) as u32,
        second: (second_of_day % 60) as u32,
    }
}

impl<'a> InputSession<'a> {
    /// 辅助码默认开、中文标点默认开、学习默认开，和 C++ 的第二个构造函数一致。
    pub fn new(mut engine: ImeSession<'a>, roots: RuntimeRoots, profile: &'static Profile) -> Self {
        engine.set_quanpin_autocorrect_types(0);
        engine.set_quanpin_helpcode_enabled(true);
        engine.set_shuangpin_helpcode_enabled(true);
        InputSession {
            candidate_queries: CandidateQueries::new(roots.clone(), profile),
            roots,
            engine,
            profile,
            caret: None,
            prefix_candidates: Vec::new(),
            prefix_query_input: String::new(),
            prefix_candidates_active: false,
            fixed_positions_enabled: false,
            immediate_phrase_progress: CreatingWordProgress::default(),
            shuangpin_preedit_uses_raw: true,
            pending_pinyin_sequence: String::new(),
            pending_pinyin_sequence_with_cases: String::new(),
            has_pending_pinyin_sequence: false,
            has_pending_pinyin_sequence_with_cases: false,
            quanpin_autocorrect_types: 0,
            quanpin_helpcode_enabled: true,
            shuangpin_helpcode_enabled: true,
            mid_sentence_helpcode_enabled: false,
            direct_helpcode_enabled: false,
            direct_helpcode_slash_marker: true,
            direct_helpcode_uppercase_marker: false,
            mid_sentence_uppercase_trigger_enabled: false,
            chinese_punctuation_enabled: true,
            candidate_learning_enabled: true,
            punctuation: PunctuationPolicy::new(),
            frequency_adjustment: FrequencyAdjustmentOptions::default(),
            frequency_adjustment_configured: false,
            local_mode_options: LocalModeOptions::default(),
            english_input_options: EnglishInputOptions::default(),
            mixed_expressive_options: MixedExpressiveOptions::default(),
            dedicated_english_mode: false,
            dedicated_english_preedit: String::new(),
            dedicated_english_candidates: Vec::new(),
            mixed_candidates: Vec::new(),
            local_input_mode: LocalInputMode::None,
            temporary_original_scheme: None,
            local_preedit: String::new(),
            local_candidates: Vec::new(),
            local_date_time_provider: None,
            online_requests: OnlineRequestGuard::new(),
            canonical_phrase_engine: None,
        }
    }

    // ------------------------------------------------------------------------------------------
    // 选项
    // ------------------------------------------------------------------------------------------

    pub fn set_helpcode_keymap(&mut self, keymap: crate::helpcode::Keymap) {
        self.engine.set_helpcode_keymap(keymap);
        self.update_mixed_candidates();
    }

    pub fn set_shuangpin_helpcode_enabled(&mut self, enabled: bool) {
        if self.shuangpin_helpcode_enabled == enabled {
            return;
        }
        self.shuangpin_helpcode_enabled = enabled;
        self.engine.set_shuangpin_helpcode_enabled(enabled);
        self.update_mixed_candidates();
        self.online_requests.invalidate();
    }

    /// 句中辅助码（双拼）。默认关闭。
    pub fn set_mid_sentence_helpcode_enabled(&mut self, enabled: bool) {
        if self.mid_sentence_helpcode_enabled == enabled {
            return;
        }
        self.mid_sentence_helpcode_enabled = enabled;
        self.engine.set_mid_sentence_helpcode_enabled(enabled);
        self.update_mixed_candidates();
        self.online_requests.invalidate();
    }

    /// 双拼直接辅助码（万象式，不要引导键）。默认关闭；开着时末尾辅助码和反引号句中辅助码都让位给它。
    pub fn set_direct_helpcode_enabled(&mut self, enabled: bool) {
        if self.direct_helpcode_enabled == enabled {
            return;
        }
        self.direct_helpcode_enabled = enabled;
        self.engine.set_direct_helpcode_enabled(enabled);
        self.update_mixed_candidates();
        self.online_requests.invalidate();
    }

    pub fn direct_helpcode_enabled(&self) -> bool {
        self.direct_helpcode_enabled
    }

    /// 句中四码用什么结束：补 /（默认）、第二位辅码大写，可以都开。
    pub fn set_direct_helpcode_markers(&mut self, slash: bool, uppercase: bool) {
        if self.direct_helpcode_slash_marker == slash && self.direct_helpcode_uppercase_marker == uppercase {
            return;
        }
        self.direct_helpcode_slash_marker = slash;
        self.direct_helpcode_uppercase_marker = uppercase;
        self.engine.set_direct_helpcode_markers(slash, uppercase);
        self.update_mixed_candidates();
        self.online_requests.invalidate();
    }

    pub fn set_mid_sentence_uppercase_trigger_enabled(&mut self, enabled: bool) {
        if self.mid_sentence_uppercase_trigger_enabled == enabled {
            return;
        }
        self.mid_sentence_uppercase_trigger_enabled = enabled;
        self.engine.set_mid_sentence_uppercase_trigger_enabled(enabled);
        self.update_mixed_candidates();
        self.online_requests.invalidate();
    }

    pub fn set_quanpin_helpcode_enabled(&mut self, enabled: bool) {
        if self.quanpin_helpcode_enabled == enabled {
            return;
        }
        self.quanpin_helpcode_enabled = enabled;
        self.engine.set_quanpin_helpcode_enabled(enabled);
        self.update_mixed_candidates();
        self.online_requests.invalidate();
    }

    pub fn set_frequency_adjustment(&mut self, options: FrequencyAdjustmentOptions) -> bool {
        if options.trigger_count < 1 || options.trigger_count > 10 || options.linear_step < 1 || options.linear_step > 10 {
            return false;
        }
        self.frequency_adjustment = options;
        self.frequency_adjustment_configured = true;
        true
    }

    pub fn frequency_adjustment(&self) -> &FrequencyAdjustmentOptions {
        &self.frequency_adjustment
    }

    pub fn set_local_mode_options(&mut self, options: LocalModeOptions) {
        let quick_phrase_changed =
            self.local_mode_options.quick_phrase_candidates != options.quick_phrase_candidates || self.local_mode_options.quick_phrase_frequency != options.quick_phrase_frequency;
        self.local_mode_options = options;
        if quick_phrase_changed && self.local_input_mode == LocalInputMode::None && !self.dedicated_english_mode {
            self.update_mixed_candidates();
        }
        let options = &self.local_mode_options;
        let disabled = match self.local_input_mode {
            LocalInputMode::Unicode => !options.unicode,
            LocalInputMode::DateTime => !options.date_time,
            LocalInputMode::QuickPhrase => !options.quick_phrase,
            LocalInputMode::Emoji => !options.emoji,
            LocalInputMode::Kaomoji => !options.kaomoji,
            LocalInputMode::SuperJianpin => !options.super_jianpin,
            LocalInputMode::TemporaryEnglish => !options.temporary_english,
            LocalInputMode::TemporaryJapanese => !options.temporary_japanese,
            LocalInputMode::None => false,
        };
        if disabled {
            self.reset_composition();
        }
    }

    pub fn local_mode_options(&self) -> &LocalModeOptions {
        &self.local_mode_options
    }

    pub fn set_english_input_options(&mut self, options: EnglishInputOptions) -> bool {
        if options.minimum_prefix < 1 || options.minimum_prefix > 8 {
            return false;
        }
        self.english_input_options = options;
        self.update_mixed_candidates();
        true
    }

    pub fn english_input_options(&self) -> &EnglishInputOptions {
        &self.english_input_options
    }

    pub fn set_mixed_expressive_options(&mut self, options: MixedExpressiveOptions) {
        self.mixed_expressive_options = options;
        self.update_mixed_candidates();
    }

    pub fn mixed_expressive_options(&self) -> &MixedExpressiveOptions {
        &self.mixed_expressive_options
    }

    pub fn set_wubi_input_options(&mut self, options: WubiInputOptions) {
        // Server 每次击键都重读配置并调用这里；值没变就不能重建候选，否则五笔会话每键会多跑一轮码表查询。
        let current = self.engine.wubi_input_options();
        if current.mixed_pinyin == options.mixed_pinyin && current.z_wildcard == options.z_wildcard {
            return;
        }
        self.engine.set_wubi_input_options(options);
        // 设置决定哪些词库回答手里的编码，所以活动的组合要重新问一遍。
        if self.is_wubi() && !self.dedicated_english_mode && self.local_input_mode == LocalInputMode::None {
            self.recompute_candidates();
        }
    }

    pub fn wubi_input_options(&self) -> WubiInputOptions {
        self.engine.wubi_input_options()
    }

    pub fn set_dedicated_english_mode(&mut self, enabled: bool) {
        if self.dedicated_english_mode == enabled {
            return;
        }
        self.reset_composition();
        self.dedicated_english_mode = enabled;
    }

    pub fn dedicated_english_mode(&self) -> bool {
        self.dedicated_english_mode
    }

    pub fn local_input_mode(&self) -> LocalInputMode {
        self.local_input_mode
    }

    pub fn set_local_date_time_provider(&mut self, provider: Box<dyn Fn() -> DateTime>) {
        self.local_date_time_provider = Some(provider);
    }

    pub fn set_quanpin_autocorrect_types(&mut self, autocorrect_types: u32) {
        // Server 每键重新下发配置；值没变就不能重建（和复制）混排列表。
        if self.quanpin_autocorrect_types == autocorrect_types {
            return;
        }
        self.quanpin_autocorrect_types = autocorrect_types;
        self.engine.set_quanpin_autocorrect_types(autocorrect_types);
        self.update_mixed_candidates();
    }

    pub fn set_fuzzy_pinyin_options(&mut self, rules: u32) {
        self.engine.set_fuzzy_pinyin_options(rules);
    }

    pub fn set_sentence_association(&mut self, options: SentenceAssociation) {
        self.engine.set_sentence_association(options);
    }

    pub fn set_enabled_domain_dictionaries(&mut self, csv: &str) {
        self.engine.set_enabled_domain_dictionaries(csv);
        self.prefix_query_input.clear();
        self.prefix_candidates.clear();
        self.refresh_prefix_candidates();
    }

    pub fn set_rescoring_context(&mut self, context: &str) {
        self.engine.set_rescoring_context(context);
    }

    pub fn set_chinese_punctuation_enabled(&mut self, enabled: bool) {
        self.chinese_punctuation_enabled = enabled;
    }

    pub fn set_candidate_learning_enabled(&mut self, enabled: bool) {
        self.candidate_learning_enabled = enabled;
    }

    pub fn set_shuangpin_preedit_uses_raw(&mut self, enabled: bool) {
        self.shuangpin_preedit_uses_raw = enabled;
    }

    pub fn quanpin_autocorrect_types(&self) -> u32 {
        self.quanpin_autocorrect_types
    }

    pub fn helpcode_enabled(&self) -> bool {
        match self.scheme() {
            SchemeType::Quanpin => self.quanpin_helpcode_enabled,
            SchemeType::Shuangpin => self.shuangpin_helpcode_enabled,
            _ => false,
        }
    }

    pub fn chinese_punctuation_enabled(&self) -> bool {
        self.chinese_punctuation_enabled
    }

    pub fn candidate_learning_enabled(&self) -> bool {
        self.candidate_learning_enabled
    }

    // ------------------------------------------------------------------------------------------
    // 方案与组合状态
    // ------------------------------------------------------------------------------------------

    /// 切方案会丢掉当前组合；承诺保留已敲文字的前端要先上屏再调用。
    pub fn switch_scheme(&mut self, scheme_type: SchemeType) {
        self.reset_composition();
        self.engine.switch_scheme(scheme_type);
        self.update_mixed_candidates();
    }

    /// 临时日语模式里是进入模式前的方案。
    pub fn scheme(&self) -> SchemeType {
        self.temporary_original_scheme.unwrap_or_else(|| self.engine.current_scheme_type())
    }

    pub fn scheme_type(&self) -> SchemeType {
        self.scheme()
    }

    pub fn current_scheme_type(&self) -> SchemeType {
        self.engine.current_scheme_type()
    }

    pub fn has_composition(&self) -> bool {
        if self.dedicated_english_mode {
            return !self.dedicated_english_preedit.is_empty();
        }
        !self.preedit().is_empty()
    }

    pub fn preedit(&self) -> &str {
        if self.dedicated_english_mode {
            return &self.dedicated_english_preedit;
        }
        if self.local_input_mode != LocalInputMode::None {
            return &self.local_preedit;
        }
        self.engine.preedit()
    }

    pub fn raw_segmentation(&self) -> &str {
        if self.dedicated_english_mode {
            return &self.dedicated_english_preedit;
        }
        if self.local_input_mode != LocalInputMode::None {
            return &self.local_preedit;
        }
        &self.engine.request().raw_segmentation
    }

    pub fn normalized_segmentation(&self) -> &str {
        if self.dedicated_english_mode {
            return &self.dedicated_english_preedit;
        }
        if self.local_input_mode != LocalInputMode::None {
            return &self.local_preedit;
        }
        &self.engine.request().normalized_segmentation
    }

    pub fn candidates(&self) -> &[Item] {
        if self.dedicated_english_mode {
            return &self.dedicated_english_candidates;
        }
        if self.local_input_mode != LocalInputMode::None {
            return &self.local_candidates;
        }
        if self.mixed_candidates_active() {
            // `update_mixed_candidates` 在前缀解码激活时从前缀候选拼出这份。
            return &self.mixed_candidates;
        }
        if self.prefix_candidates_active {
            return &self.prefix_candidates;
        }
        self.engine.candidates()
    }

    pub fn get_candidates(&self) -> &[Item] {
        self.candidates()
    }

    pub fn expand_initial_candidates(&mut self) -> bool {
        self.engine.expand_initial_candidates()
    }

    pub fn find_candidate(&self, key: &str, value: &str) -> Option<Item> {
        self.engine.find_candidate(self.scheme(), key, value)
    }

    fn request(&self) -> &QueryRequest {
        self.engine.request()
    }

    fn is_shuangpin(&self) -> bool {
        self.current_scheme_type() == SchemeType::Shuangpin
    }

    fn is_wubi(&self) -> bool {
        self.current_scheme_type() == SchemeType::Wubi
    }

    fn is_japanese(&self) -> bool {
        self.current_scheme_type() == SchemeType::JapaneseRomaji
    }

    /// 候选是否由五笔码表产出。混输组合里五笔候选在前、拼音候选追加在后，调频、删除、固定位置
    /// 与上屏推进都按候选自己的方案走，不再按会话方案一刀切。
    fn is_wubi_native_candidate(item: &Item) -> bool {
        item.scheme == SchemeType::Wubi
    }

    /// 当前列表里五笔码表候选的条数；顶字与四码自动上屏只认它，追加的拼音候选不算。
    fn wubi_native_candidate_count(&self) -> usize {
        self.candidates().iter().filter(|item| Self::is_wubi_native_candidate(item)).count()
    }

    fn journal_path(&self) -> String {
        self.roots.user_data.join("msime_user.db").to_string_lossy().into_owned()
    }

    fn main_dictionary_path(&self) -> String {
        self.roots.dictionaries.join("msime.db").to_string_lossy().into_owned()
    }

    fn english_dictionary_path(&self) -> String {
        self.roots.dictionaries.join("english.db").to_string_lossy().into_owned()
    }

    /// 重置、清缓存。
    pub fn reset_state(&mut self) {
        self.clear_pending_sequence();
        self.reset_composition();
    }

    pub fn reset_cache(&mut self) {
        self.engine.reset_cache();
        if let Some(engine) = &self.canonical_phrase_engine {
            engine.reset_cache();
        }
        // 前缀候选是按文本缓存的，不跟着引擎缓存失效：只清缓存键，让下一次
        // refresh_prefix_candidates 按新权重/选项重查；保留当前列表，避免 caret 激活期间出现空候选窗。
        self.prefix_query_input.clear();
    }

    /// 神经重排结果回来后用：只失效整句排序，由调用方接着 `recompute_candidates`。
    pub fn reset_sentence_cache(&mut self) {
        self.engine.reset_sentence_cache();
        if let Some(engine) = &self.canonical_phrase_engine {
            engine.reset_sentence_cache();
        }
        self.prefix_query_input.clear();
    }

    fn reset_composition(&mut self) {
        self.caret = None;
        self.prefix_candidates.clear();
        self.prefix_query_input.clear();
        self.prefix_candidates_active = false;
        self.immediate_phrase_progress = CreatingWordProgress::default();
        self.clear_pending_sequence();
        self.online_requests.invalidate();
        let original_scheme = self.temporary_original_scheme;
        self.local_input_mode = LocalInputMode::None;
        self.temporary_original_scheme = None;
        self.local_preedit.clear();
        self.local_candidates.clear();
        self.dedicated_english_preedit.clear();
        self.dedicated_english_candidates.clear();
        self.mixed_candidates.clear();
        self.engine.reset();
        if let Some(original) = original_scheme {
            if self.engine.current_scheme_type() != original {
                self.engine.switch_scheme(original);
            }
        }
    }

    /// 跨多次分段选词拼起来的词只在组合还活着时才有意义。任何清空预编辑却没走 `reset_composition` 的路径
    /// （退格删光剩余拼音、继续选词后剩余输入没了）都等于放弃这个词，这里丢掉，免得接到下一个组合的选词上被存成没人打过的词。
    fn discard_abandoned_phrase_progress(&mut self) {
        if !self.has_composition() {
            self.immediate_phrase_progress = CreatingWordProgress::default();
        }
    }

    // ------------------------------------------------------------------------------------------
    // 在线（云 / AI）候选
    // ------------------------------------------------------------------------------------------

    pub fn online_query(&self) -> Option<OnlineQuery> {
        if self.dedicated_english_mode || self.local_input_mode != LocalInputMode::None || !self.has_composition() {
            return None;
        }
        let request = self.engine.request();
        let mut query = OnlineQuery { scheme: request.scheme, ..Default::default() };
        self.online_requests.stamp(&mut query);
        let input = if request.raw_input_with_cases.is_empty() { &request.raw_input } else { &request.raw_input_with_cases };
        query.identity = format!("{}:{}", scheme_number(request.scheme), input);

        let state = self.get_cloud_query_state();
        if !state.should_query {
            return None;
        }
        query.query_text = state.query_text;
        query.cache_key = state.cache_key;
        if request.scheme == SchemeType::JapaneseRomaji {
            query.cloud_eligible = true;
            return Some(query);
        }
        if query.query_text.is_empty() {
            return None;
        }
        let segmentation = if request.normalized_segmentation.is_empty() { &query.query_text } else { &request.normalized_segmentation };
        query.pinyin_segments = crate::quanpin_query::split_segments(segmentation);
        query.cloud_eligible = true;
        query.ai_eligible = !query.pinyin_segments.is_empty() && query.pinyin_segments.iter().all(|segment| crate::quanpin_query::is_complete_pinyin_input(segment));
        Some(query)
    }

    pub fn apply_online_candidate(&mut self, query: &OnlineQuery, candidate: &str, source: Source) -> bool {
        if candidate.is_empty() || (source != Source::CloudSuggestion && source != Source::AiSuggestion) {
            return false;
        }
        let Some(current) = self.online_query() else { return false };
        if !self.online_requests.matches(&current, query)
            || (source == Source::CloudSuggestion && !current.cloud_eligible)
            || (source == Source::AiSuggestion && !current.ai_eligible)
            || self.candidates().iter().any(|item| item.word == candidate)
        {
            return false;
        }
        if self.engine.apply_dynamic_candidate(candidate, source) != 0 {
            return false;
        }
        self.update_mixed_candidates();
        self.candidates().iter().any(|item| item.word == candidate && item.source == source)
    }

    pub fn cache_dynamic_candidate(&mut self, pinyin: &str, word: &str, source: Source) -> i32 {
        let cache_result = self.engine.cache_dynamic_candidate(pinyin, word, source);
        let _ = self.engine.cache_dynamic_candidate_for_current_request(word, source);
        cache_result
    }
}

fn scheme_number(scheme: SchemeType) -> u32 {
    match scheme {
        SchemeType::Quanpin => 0,
        SchemeType::Shuangpin => 1,
        SchemeType::Wubi => 2,
        SchemeType::JapaneseRomaji => 3,
    }
}
