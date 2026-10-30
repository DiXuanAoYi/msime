//! 平台入口 `Session`：对应 `engine/core/session.cpp` 与 `include/metasequoia/session.h`。
//!
//! 包一个 `InputSession`（拼音、五笔、日语的兼容会话）。
//!
//! 和 C++ 不同的几处：辅助码方案按名字找文件由宿主做，这里直接收已经建好的 `InputSession`
//! （词库路径、日志路径、语言模型、码表都在它里面）；`SessionOptions` 只管构造后要应用的那些开关。

use crate::ime_session::WubiInputOptions;
use crate::input_session::{
    CandidateEdge, Command, EnglishInputOptions, FrequencyAdjustmentOptions, InputSession, KeyResult, LocalInputMode, LocalModeOptions, MixedExpressiveOptions, OnlineQuery,
};
use crate::lattice::{Item, Source};
use crate::request::SchemeType;

#[derive(Debug, Clone)]
pub struct SessionOptions {
    pub autocorrect_types: u32,
    pub helpcode: bool,
    pub chinese_punctuation: bool,
    pub learning: bool,
    pub fuzzy_pinyin: u32,
    pub frequency: FrequencyAdjustmentOptions,
    pub local_modes: LocalModeOptions,
    pub english: EnglishInputOptions,
    pub expressive: MixedExpressiveOptions,
    pub wubi: WubiInputOptions,
}

#[derive(Debug, Clone)]
pub struct SessionSnapshot {
    pub scheme: SchemeType,
    pub local_mode: LocalInputMode,
    pub preedit: String,
    pub raw_segmentation: String,
    pub normalized_segmentation: String,
    pub candidates: Vec<Item>,
    pub dedicated_english: bool,
    pub editing_text: String,
    pub caret_position: usize,
}

pub struct Session<'a> {
    session: InputSession<'a>,
}

impl<'a> Session<'a> {
    /// 选项里的频率或英文参数被拒绝就返回错误（C++ 抛 `invalid_argument`）。
    pub fn new(mut session: InputSession<'a>, options: &SessionOptions) -> Result<Self, String> {
        session.set_quanpin_autocorrect_types(options.autocorrect_types);
        session.set_fuzzy_pinyin_options(options.fuzzy_pinyin);
        session.set_quanpin_helpcode_enabled(options.helpcode);
        session.set_shuangpin_helpcode_enabled(options.helpcode);
        if !session.set_frequency_adjustment(options.frequency) || !session.set_english_input_options(options.english) {
            return Err("Invalid session options".to_string());
        }
        session.set_local_mode_options(options.local_modes);
        session.set_mixed_expressive_options(options.expressive);
        session.set_wubi_input_options(options.wubi);
        session.enable_fixed_positions();
        session.set_chinese_punctuation_enabled(options.chinese_punctuation);
        session.set_candidate_learning_enabled(options.learning);
        Ok(Self { session })
    }

    /// 宿主要拿到底层会话时用（给时钟、码表这类构造后的设置）。
    pub fn input_session_mut(&mut self) -> &mut InputSession<'a> {
        &mut self.session
    }

    pub fn character(&mut self, value: char, shift_only: bool) -> KeyResult {
        self.session.handle_character(value, shift_only)
    }

    pub fn command(&mut self, value: Command) -> KeyResult {
        self.session.handle_command(value)
    }

    pub fn candidate_key(&mut self, value: char) -> KeyResult {
        self.session.handle_candidate_key(value)
    }

    pub fn punctuation(&mut self, value: char) -> KeyResult {
        self.session.handle_punctuation(value)
    }

    pub fn set_chinese_punctuation_enabled(&mut self, enabled: bool) {
        self.session.set_chinese_punctuation_enabled(enabled);
    }

    pub fn select(&mut self, index: usize) -> KeyResult {
        self.session.select_candidate(index)
    }

    pub fn select_edge(&mut self, index: usize, edge: CandidateEdge) -> KeyResult {
        self.session.select_candidate_edge(index, edge)
    }

    pub fn pin(&mut self, index: usize) -> KeyResult {
        self.session.pin_candidate(index)
    }

    pub fn remove(&mut self, index: usize) -> KeyResult {
        self.session.remove_candidate(index)
    }

    pub fn fix_position(&mut self, index: usize, position: i32) -> KeyResult {
        if !(1..=5).contains(&position) {
            return KeyResult::default();
        }
        self.session.set_candidate_position(index, position)
    }

    pub fn clear_position(&mut self, index: usize) -> KeyResult {
        self.session.set_candidate_position(index, 0)
    }

    pub fn finish(&mut self, first_index: usize) -> KeyResult {
        self.session.finish_composition(first_index)
    }

    pub fn switch_scheme(&mut self, scheme: SchemeType) {
        self.session.switch_scheme(scheme);
    }

    pub fn set_helpcode_enabled(&mut self, enabled: bool) {
        self.session.set_quanpin_helpcode_enabled(enabled);
        self.session.set_shuangpin_helpcode_enabled(enabled);
    }

    pub fn set_dedicated_english(&mut self, enabled: bool) {
        self.session.set_dedicated_english_mode(enabled);
    }

    /// 只改混输这一个开关：整份重建 `WubiInputOptions` 会把运行期已生效的 z 通配一起复位。
    pub fn set_wubi_mixed_pinyin(&mut self, enabled: bool) {
        let mut options = self.session.wubi_input_options();
        options.mixed_pinyin = enabled;
        self.session.set_wubi_input_options(options);
    }

    pub fn snapshot(&self) -> SessionSnapshot {
        SessionSnapshot {
            scheme: self.session.scheme(),
            local_mode: self.session.local_input_mode(),
            preedit: self.session.preedit().to_string(),
            raw_segmentation: self.session.raw_segmentation().to_string(),
            normalized_segmentation: self.session.normalized_segmentation().to_string(),
            candidates: self.session.candidates().to_vec(),
            dedicated_english: self.session.dedicated_english_mode(),
            editing_text: self.session.editing_text(),
            caret_position: self.session.caret_position(),
        }
    }

    pub fn online_query(&self) -> Option<OnlineQuery> {
        self.session.online_query()
    }

    pub fn apply_online_candidate(&mut self, query: &OnlineQuery, candidate: &str, source: Source) -> bool {
        self.session.apply_online_candidate(query, candidate, source)
    }
}
