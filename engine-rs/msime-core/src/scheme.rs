//! 输入方案：把按键攒成原始输入串，再生成 `QueryRequest`。对应 `engine/schemes/` 下的四个方案。
//!
//! 方案只管“键 → 串 → 请求”，不碰词库；开关（直接辅助码、大写触发、五笔的扩展长度/混输/通配）由会话设。

use crate::autocorrect::correction_aliases_enabled;
use crate::mid_sentence;
use crate::quanpin_query::{cut_pinyin_by_mode, join_segments, CutMode};
use crate::quanpin_utils::{detect_active_helpcode_length, strip_active_helpcodes};
use crate::request::{key, KeyStroke, QueryRequest, SchemeType};
use crate::romaji;
use crate::shuangpin::Profile;
use crate::shuangpin_query as sq;

pub trait InputScheme {
    fn reset(&mut self);
    fn handle_key(&mut self, vk: u32, modifiers_down: u32, wch: u16);
    /// 宿主整串替换（中间编辑、光标前缀解码）。
    fn set_raw_input(&mut self, raw_input: &str, raw_input_with_cases: &str);
    fn build_request(&self) -> QueryRequest;
    fn preedit(&self) -> String;
    fn scheme_type(&self) -> SchemeType;
}

fn is_alpha_vk(vk: u32) -> bool {
    (u32::from(b'A')..=u32::from(b'Z')).contains(&vk)
}

fn is_ascii_letter_char(wch: u16) -> bool {
    (u16::from(b'a')..=u16::from(b'z')).contains(&wch) || (u16::from(b'A')..=u16::from(b'Z')).contains(&wch)
}

/// 退格弹一个字符一个按键；退出、回车清空。三个方案都一样。
fn common_edit(raw: &mut String, strokes: &mut Vec<KeyStroke>, vk: u32) -> Option<bool> {
    if vk == key::BACKSPACE {
        raw.pop();
        strokes.pop();
        return Some(false);
    }
    if vk == key::ESCAPE || vk == key::RETURN {
        return Some(true);
    }
    None
}

fn letter_from(vk: u32, wch: u16) -> char {
    if is_ascii_letter_char(wch) {
        wch as u8 as char
    } else {
        (vk as u8 + (b'a' - b'A')) as char
    }
}

// ---------------------------------------------------------------------------------------------
// 全拼
// ---------------------------------------------------------------------------------------------

#[derive(Default)]
pub struct QuanpinScheme {
    raw_input: String,
    key_strokes: Vec<KeyStroke>,
    /// 手误别名层要不要参与切分（由会话的纠错掩码决定，默认关）：关掉纠错就不再通过别名改写切分。
    include_correction_aliases: bool,
}

impl QuanpinScheme {
    /// 纠错掩码由会话层在绑定方案时下发，方案只关心手误别名层开不开。
    pub fn set_autocorrect_types(&mut self, autocorrect_types: u32) {
        self.include_correction_aliases = correction_aliases_enabled(autocorrect_types);
    }
}

impl InputScheme for QuanpinScheme {
    fn reset(&mut self) {
        self.raw_input.clear();
        self.key_strokes.clear();
    }

    fn set_raw_input(&mut self, raw_input: &str, raw_input_with_cases: &str) {
        self.raw_input = if raw_input_with_cases.is_empty() { raw_input } else { raw_input_with_cases }.to_string();
        self.key_strokes.clear();
    }

    fn handle_key(&mut self, vk: u32, modifiers_down: u32, wch: u16) {
        match common_edit(&mut self.raw_input, &mut self.key_strokes, vk) {
            Some(true) => return self.reset(),
            Some(false) => return,
            None => {}
        }
        if vk == key::APOSTROPHE {
            if !self.raw_input.ends_with('\'') {
                self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
                self.raw_input.push('\'');
            }
            return;
        }
        if !is_alpha_vk(vk) {
            return;
        }
        self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
        self.raw_input.push(letter_from(vk, wch));
    }

    fn build_request(&self) -> QueryRequest {
        let mut request = QueryRequest { scheme: SchemeType::Quanpin, raw_input_with_cases: self.raw_input.clone(), ..Default::default() };
        request.raw_input = self.raw_input.chars().map(|c| if c == '\'' { c } else { c.to_ascii_lowercase() }).collect();
        request.key_strokes = self.key_strokes.clone();

        let helpcode_length = detect_active_helpcode_length(&request.raw_input, &request.raw_input_with_cases);
        let normalized_source = strip_active_helpcodes(&request.raw_input, &request.raw_input_with_cases);
        request.normalized_input = normalized_source.chars().filter(|&c| c != '\'').collect();
        if !request.normalized_input.is_empty() {
            if let Some(first) = cut_pinyin_by_mode(&normalized_source, CutMode::Correction, self.include_correction_aliases).first() {
                request.normalized_segmentation = join_segments(first);
            }
        }
        if request.normalized_segmentation.is_empty() {
            request.normalized_segmentation = normalized_source;
        }

        let mut cased_source = request.raw_input_with_cases.clone();
        if helpcode_length > 0 && cased_source.len() >= helpcode_length {
            cased_source.truncate(cased_source.len() - helpcode_length);
        }
        request.raw_segmentation = sq::apply_segmentation_cases(&request.normalized_segmentation, &cased_source);
        if cased_source.ends_with('\'') && !request.raw_segmentation.ends_with('\'') {
            request.raw_segmentation.push('\'');
        }
        if helpcode_length > 0 {
            request.raw_segmentation.push('\'');
            request.raw_segmentation.push_str(&request.raw_input_with_cases[request.raw_input_with_cases.len() - helpcode_length..]);
        }
        request.segmentation = request.normalized_segmentation.clone();
        request.valid = !request.normalized_input.is_empty();
        request
    }

    fn preedit(&self) -> String {
        self.raw_input.clone()
    }

    fn scheme_type(&self) -> SchemeType {
        SchemeType::Quanpin
    }
}

// ---------------------------------------------------------------------------------------------
// 双拼
// ---------------------------------------------------------------------------------------------

pub struct ShuangpinScheme {
    profile: &'static Profile,
    direct_helpcode: bool,
    mid_sentence_uppercase_trigger: bool,
    raw_input: String,
    key_strokes: Vec<KeyStroke>,
}

impl ShuangpinScheme {
    pub fn new(profile: &'static Profile) -> Self {
        ShuangpinScheme { profile, direct_helpcode: false, mid_sentence_uppercase_trigger: false, raw_input: String::new(), key_strokes: Vec::new() }
    }

    /// 直接辅助码开着时收 `/`（四码后的终止键），`;` 韵母也不再要求所在一节是奇数键：辅码会打乱奇偶。
    pub fn set_direct_helpcode(&mut self, enabled: bool) {
        self.direct_helpcode = enabled;
    }

    /// 句中辅助码的大写触发：完整音节后的大写字母开一段。
    pub fn set_mid_sentence_uppercase_trigger(&mut self, enabled: bool) {
        self.mid_sentence_uppercase_trigger = enabled;
    }
}

impl InputScheme for ShuangpinScheme {
    fn reset(&mut self) {
        self.raw_input.clear();
        self.key_strokes.clear();
    }

    fn set_raw_input(&mut self, raw_input: &str, raw_input_with_cases: &str) {
        self.raw_input = if raw_input_with_cases.is_empty() { raw_input } else { raw_input_with_cases }.to_string();
        self.key_strokes.clear();
    }

    fn handle_key(&mut self, vk: u32, modifiers_down: u32, wch: u16) {
        match common_edit(&mut self.raw_input, &mut self.key_strokes, vk) {
            Some(true) => return self.reset(),
            Some(false) => return,
            None => {}
        }
        if vk == key::APOSTROPHE {
            if !self.raw_input.ends_with('\'') {
                self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
                self.raw_input.push('\'');
            }
            return;
        }
        // 句中辅助码的反引号。开关与“前面是不是一节完整的偶数键”由调用方判断，这里只负责记下。
        if vk == key::BACKQUOTE && wch == u16::from(b'`') {
            self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
            self.raw_input.push(mid_sentence::MARKER as char);
            return;
        }
        if self.direct_helpcode && wch == u16::from(b'/') {
            self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
            self.raw_input.push('/');
            return;
        }

        let semicolon = wch == u16::from(b';');
        let microsoft_ing_key = if self.direct_helpcode {
            self.profile.uses_semicolon_final() && semicolon && self.raw_input.as_bytes().last().is_some_and(u8::is_ascii_alphabetic)
        } else {
            self.profile.uses_semicolon_final()
                && vk == key::SEMICOLON
                && semicolon
                && mid_sentence::accepts_semicolon_final_at(self.raw_input.as_bytes(), self.raw_input.len(), self.mid_sentence_uppercase_trigger)
        };
        if !is_alpha_vk(vk) && !microsoft_ing_key {
            return;
        }
        self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
        if is_ascii_letter_char(wch) {
            self.raw_input.push(wch as u8 as char);
        } else if microsoft_ing_key {
            self.raw_input.push(';');
        } else {
            self.raw_input.push(letter_from(vk, wch));
        }
    }

    fn build_request(&self) -> QueryRequest {
        let mut request = QueryRequest { scheme: SchemeType::Shuangpin, raw_input_with_cases: self.raw_input.clone(), ..Default::default() };
        if sq::has_mid_sentence_helpcode(&self.raw_input, self.mid_sentence_uppercase_trigger) {
            // 下游只认 ' 分隔的双拼：每段（反引号段或大写段）换成分隔符，约束单独带着，原串留给宿主回读。
            let parsed = sq::parse_mid_sentence_helpcodes(&self.raw_input, self.profile, self.mid_sentence_uppercase_trigger);
            request.raw_input_with_cases = parsed.input;
            request.raw_input_with_syllable_helpcodes = self.raw_input.clone();
            request.syllable_helpcodes = parsed.helpcodes;
            request.has_syllable_helpcode_layout = true;
            request.syllable_helpcode_source_index = parsed.source_index;
            request.syllable_helpcode_decorations = parsed.decorations;
        }
        request.raw_input = request.raw_input_with_cases.chars().map(|c| if c == '\'' { c } else { c.to_ascii_lowercase() }).collect();
        request.key_strokes = self.key_strokes.clone();
        request.valid = sq::effective_input_length(&request.raw_input) > 0;
        if !request.valid {
            return request;
        }
        let raw_segmentation = sq::segment_input(&request.raw_input, self.profile);
        request.raw_segmentation = sq::apply_segmentation_cases(&raw_segmentation, &request.raw_input_with_cases);
        request.normalized_segmentation = sq::to_quanpin_segmentation(&raw_segmentation, self.profile);
        request.segmentation = request.normalized_segmentation.clone();
        request.normalized_input = sq::normalize_input(&request.raw_input, self.profile);
        request
    }

    fn preedit(&self) -> String {
        self.raw_input.clone()
    }

    fn scheme_type(&self) -> SchemeType {
        SchemeType::Shuangpin
    }
}

// ---------------------------------------------------------------------------------------------
// 五笔
// ---------------------------------------------------------------------------------------------

/// 五笔编码四码，第五码默认拒绝。
const WUBI_MAX_CODE_LENGTH: usize = 4;
/// 混输时放宽的长度：够拼任何拼音，又不至于成了一整段。
const WUBI_MAX_MIXED_CODE_LENGTH: usize = 32;

#[derive(Default)]
pub struct WubiScheme {
    raw_input: String,
    key_strokes: Vec<KeyStroke>,
    extended_length_allowed: bool,
    mixed_pinyin_allowed: bool,
    z_wildcard: bool,
}

impl WubiScheme {
    /// 混输只在表里查不到手上的码之后才放开长度，这样 nihao、zhongguo 这类拼写才打得完；表里有答案的码保持四码。
    pub fn set_extended_length_allowed(&mut self, allowed: bool) {
        self.extended_length_allowed = allowed;
    }

    /// 五笔码里没有 z，平时丢掉；混输要收它，因为 z 是拼音字母，丢了会悄悄变成另一个拼写。跟着设置走。
    pub fn set_mixed_pinyin_allowed(&mut self, allowed: bool) {
        self.mixed_pinyin_allowed = allowed;
    }

    /// 通配模式要同一个键，理由相反：z 代表不知道的那一码。
    pub fn set_z_wildcard(&mut self, enabled: bool) {
        self.z_wildcard = enabled;
    }

    /// 完整编码是固定的四码，不随扩展长度变。
    pub fn has_complete_code(&self) -> bool {
        self.raw_input.len() == WUBI_MAX_CODE_LENGTH
    }

    fn max_code_length(&self) -> usize {
        if self.extended_length_allowed {
            WUBI_MAX_MIXED_CODE_LENGTH
        } else {
            WUBI_MAX_CODE_LENGTH
        }
    }

    fn is_wubi_letter(&self, lower: u8) -> bool {
        (b'a'..=b'y').contains(&lower) || (lower == b'z' && (self.mixed_pinyin_allowed || self.z_wildcard))
    }
}

impl InputScheme for WubiScheme {
    fn reset(&mut self) {
        self.raw_input.clear();
        self.key_strokes.clear();
        self.extended_length_allowed = false;
    }

    fn handle_key(&mut self, vk: u32, modifiers_down: u32, wch: u16) {
        match common_edit(&mut self.raw_input, &mut self.key_strokes, vk) {
            Some(true) => return self.reset(),
            Some(false) => return,
            None => {}
        }
        let lower = (vk as u8).wrapping_add(b'a' - b'A');
        if !is_alpha_vk(vk) || !self.is_wubi_letter(lower) || self.raw_input.len() >= self.max_code_length() {
            return;
        }
        self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
        self.raw_input.push(lower as char);
    }

    fn set_raw_input(&mut self, raw_input: &str, raw_input_with_cases: &str) {
        let source = if raw_input_with_cases.is_empty() { raw_input } else { raw_input_with_cases };
        let max_length = self.max_code_length();
        let mut normalized = String::new();
        for byte in source.bytes() {
            let lower = byte.to_ascii_lowercase();
            if !self.is_wubi_letter(lower) {
                continue;
            }
            normalized.push(lower as char);
            if normalized.len() == max_length {
                break;
            }
        }
        self.raw_input = normalized;
        self.key_strokes.clear();
    }

    fn build_request(&self) -> QueryRequest {
        QueryRequest {
            scheme: SchemeType::Wubi,
            raw_input: self.raw_input.clone(),
            raw_input_with_cases: self.raw_input.clone(),
            normalized_input: self.raw_input.clone(),
            raw_segmentation: self.raw_input.clone(),
            normalized_segmentation: self.raw_input.clone(),
            segmentation: self.raw_input.clone(),
            key_strokes: self.key_strokes.clone(),
            // 通配只在编码里真有 z 时才走模式匹配；只开混输不会进模式路径。
            wubi_z_wildcard: self.z_wildcard && self.raw_input.contains('z'),
            valid: !self.raw_input.is_empty(),
            ..Default::default()
        }
    }

    fn preedit(&self) -> String {
        self.raw_input.clone()
    }

    fn scheme_type(&self) -> SchemeType {
        SchemeType::Wubi
    }
}

// ---------------------------------------------------------------------------------------------
// 日语罗马字
// ---------------------------------------------------------------------------------------------

#[derive(Default)]
pub struct JapaneseRomajiScheme {
    raw_input: String,
    key_strokes: Vec<KeyStroke>,
}

impl InputScheme for JapaneseRomajiScheme {
    fn reset(&mut self) {
        self.raw_input.clear();
        self.key_strokes.clear();
    }

    fn set_raw_input(&mut self, raw_input: &str, raw_input_with_cases: &str) {
        let source = if raw_input_with_cases.is_empty() { raw_input } else { raw_input_with_cases };
        self.raw_input = source.chars().filter(|c| c.is_ascii_alphabetic() || *c == '\'' || *c == '-').collect();
        self.key_strokes.clear();
    }

    fn handle_key(&mut self, vk: u32, modifiers_down: u32, wch: u16) {
        match common_edit(&mut self.raw_input, &mut self.key_strokes, vk) {
            Some(true) => return self.reset(),
            Some(false) => return,
            None => {}
        }
        if vk == key::APOSTROPHE && wch == u16::from(b'\'') {
            self.raw_input.push('\'');
            self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
            return;
        }
        // '-' 是长音符（ー）的罗马字写法，不是翻页键，罗马字表里已有对应条目。
        if vk == key::MINUS && wch == u16::from(b'-') {
            self.raw_input.push('-');
            self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
            return;
        }
        if !is_alpha_vk(vk) {
            return;
        }
        self.key_strokes.push(KeyStroke { vk, modifiers_down, wch });
        self.raw_input.push(letter_from(vk, wch));
    }

    fn build_request(&self) -> QueryRequest {
        let raw_input = self.raw_input.to_ascii_lowercase();
        let converted = romaji::convert(&raw_input);
        let normalized_segmentation = format!("{}{}", converted.hiragana, converted.pending);
        QueryRequest {
            scheme: SchemeType::JapaneseRomaji,
            raw_input_with_cases: self.raw_input.clone(),
            normalized_input: raw_input.clone(),
            raw_segmentation: self.raw_input.clone(),
            segmentation: normalized_segmentation.clone(),
            normalized_segmentation,
            key_strokes: self.key_strokes.clone(),
            valid: !raw_input.is_empty(),
            raw_input,
            ..Default::default()
        }
    }

    fn preedit(&self) -> String {
        self.raw_input.clone()
    }

    fn scheme_type(&self) -> SchemeType {
        SchemeType::JapaneseRomaji
    }
}
