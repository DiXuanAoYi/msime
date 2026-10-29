//! 一次候选查询的请求，对应 `engine/core/query_request.h`、`scheme_type.h`、`key_event.h`。
//!
//! 方案（`scheme.rs`）把按键攒成请求，引擎（`pinyin_engine.rs` 等）读请求出候选。

use crate::shuangpin_dictionary::SentenceAssociation;
use crate::shuangpin_query::SyllableHelpcode;

/// 虚拟键码，数值与 Windows 的 `VK_*` 一致。
pub mod key {
    pub const BACKSPACE: u32 = 0x08;
    pub const TAB: u32 = 0x09;
    pub const RETURN: u32 = 0x0D;
    pub const SHIFT: u32 = 0x10;
    pub const ESCAPE: u32 = 0x1B;
    pub const SPACE: u32 = 0x20;
    pub const SEMICOLON: u32 = 0xBA;
    pub const MINUS: u32 = 0xBD;
    pub const SLASH: u32 = 0xBF;
    pub const APOSTROPHE: u32 = 0xDE;
    pub const BACKQUOTE: u32 = 0xC0;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SchemeType {
    #[default]
    Quanpin,
    Shuangpin,
    Wubi,
    JapaneseRomaji,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct KeyStroke {
    pub vk: u32,
    pub modifiers_down: u32,
    pub wch: u16,
}

#[derive(Debug, Clone, PartialEq)]
pub struct QueryRequest {
    pub scheme: SchemeType,
    pub raw_input: String,
    pub raw_input_with_cases: String,
    pub normalized_input: String,
    pub raw_segmentation: String,
    pub normalized_segmentation: String,
    pub segmentation: String,
    pub enable_shuangpin_helpcode: bool,
    pub enable_quanpin_helpcode: bool,
    /// 句中辅助码：输入串里带反引号段时，`raw_input` 等都是把每段换成一个 `'` 之后的结果，原串放在
    /// `raw_input_with_syllable_helpcodes`。`syllable_helpcodes` 不受开关影响，开关决定是否参与筛选。
    pub enable_mid_sentence_helpcode: bool,
    pub raw_input_with_syllable_helpcodes: String,
    pub syllable_helpcodes: Vec<SyllableHelpcode>,
    /// 去段串到原串的下标映射（末尾多一项等于原串长度）和每段原样的辅码文本挂在哪个音节后面。
    pub has_syllable_helpcode_layout: bool,
    pub syllable_helpcode_source_index: Vec<usize>,
    pub syllable_helpcode_decorations: Vec<(usize, String)>,
    /// 会话开着直接辅助码（不论这次请求有没有被改写）。
    pub enable_direct_helpcode: bool,
    pub direct_helpcode_slash_marker: bool,
    pub direct_helpcode_uppercase_marker: bool,
    pub enable_quanpin_autocorrect_transposition: bool,
    pub enable_quanpin_autocorrect_neighbor: bool,
    pub enable_quanpin_autocorrect_deletion: bool,
    pub enable_quanpin_autocorrect_insertion: bool,
    pub key_strokes: Vec<KeyStroke>,
    pub fuzzy_pinyin: u32,
    pub sentence_association: SentenceAssociation,
    pub enabled_domain_dictionaries: String,
    /// 给神经重排看的前文：本会话最近上屏的文本。
    pub rescoring_context: String,
    /// 五笔 z 通配：编码里有 z 时按模式匹配而不是前缀范围。
    pub wubi_z_wildcard: bool,
    pub valid: bool,
}

impl Default for QueryRequest {
    fn default() -> Self {
        QueryRequest {
            scheme: SchemeType::Quanpin,
            raw_input: String::new(),
            raw_input_with_cases: String::new(),
            normalized_input: String::new(),
            raw_segmentation: String::new(),
            normalized_segmentation: String::new(),
            segmentation: String::new(),
            enable_shuangpin_helpcode: false,
            enable_quanpin_helpcode: false,
            enable_mid_sentence_helpcode: false,
            raw_input_with_syllable_helpcodes: String::new(),
            syllable_helpcodes: Vec::new(),
            has_syllable_helpcode_layout: false,
            syllable_helpcode_source_index: Vec::new(),
            syllable_helpcode_decorations: Vec::new(),
            enable_direct_helpcode: false,
            direct_helpcode_slash_marker: true,
            direct_helpcode_uppercase_marker: false,
            enable_quanpin_autocorrect_transposition: false,
            enable_quanpin_autocorrect_neighbor: false,
            enable_quanpin_autocorrect_deletion: false,
            enable_quanpin_autocorrect_insertion: false,
            key_strokes: Vec::new(),
            fuzzy_pinyin: 0,
            sentence_association: SentenceAssociation::default(),
            enabled_domain_dictionaries: String::new(),
            rescoring_context: String::new(),
            wubi_z_wildcard: false,
            valid: false,
        }
    }
}
