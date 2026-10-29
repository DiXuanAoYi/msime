//! 光标编辑与输入单元边界。对应 `engine/core/input_session_editing.cpp`。

use super::{Command, InputSession, KeyResult, LocalInputMode};
use crate::input_contracts;
use crate::mid_sentence;
use crate::request::{QueryRequest, SchemeType};
use crate::shuangpin_query as sq;

/// 全拼拼写在原始坐标里的单元边界。可见预编辑总是由原始字母重建的（纠错切分和别名层只移动或增加分隔符），
/// 所以显示里的分隔符就是下一个原始单元的起点。显示解释不了原始字母时不给边界，调用方退回逐字符编辑。
fn quanpin_raw_boundaries(raw: &str, display: &str) -> Vec<usize> {
    let raw_letter_offsets: Vec<usize> = raw.bytes().enumerate().filter(|(_, b)| *b != b'\'').map(|(index, _)| index).collect();
    if raw_letter_offsets.is_empty() {
        return Vec::new();
    }

    let mut boundaries = vec![0];
    let mut letters_seen = 0;
    for ch in display.chars() {
        if ch != '\'' {
            letters_seen += 1;
            continue;
        }
        // 下一个单元从下一个字母的原始偏移开始；末尾的分隔符后面没有字母，不开新单元。
        if letters_seen < raw_letter_offsets.len() {
            boundaries.push(raw_letter_offsets[letters_seen]);
        }
    }
    if letters_seen != raw_letter_offsets.len() {
        // 显示和原始字母不再逐字母对应：宁可不映射，也不删一段随意的范围。
        return Vec::new();
    }
    boundaries.push(raw.len());
    boundaries.dedup();
    boundaries
}

fn lowercase_without_separators(text: &str) -> String {
    text.chars().filter(|c| *c != '\'').map(|c| c.to_ascii_lowercase()).collect()
}

/// 带辅助码段的双拼（句中辅助码的反引号段 / 大写段、直接辅助码解析出的辅码字母）：按请求里的段布局切单元，
/// 每个音节和挂在它后面的辅码段各算一个单元，按段删除可以只删辅码，光标也能停在音节与辅码之间。
/// 布局对不上原串时不给边界，宿主退回逐字符编辑。
fn syllable_helpcode_raw_boundaries(raw: &str, request: &QueryRequest) -> Vec<usize> {
    let units: Vec<(usize, String)> = request.syllable_helpcode_decorations.iter().map(|(syllable, text)| (*syllable, format!("'{text}"))).collect();
    let segmentation = if request.raw_segmentation.is_empty() { &request.raw_input } else { &request.raw_segmentation };
    let display = sq::decorate_segmentation(segmentation, &units);
    if lowercase_without_separators(&display) != lowercase_without_separators(raw) {
        return Vec::new();
    }
    quanpin_raw_boundaries(raw, &display)
}

impl<'a> InputSession<'a> {
    pub fn editing_text(&self) -> String {
        if self.dedicated_english_mode {
            return self.dedicated_english_preedit.clone();
        }
        if self.local_input_mode == LocalInputMode::TemporaryJapanese {
            return format!("R{}", self.engine.request().raw_input_with_cases);
        }
        if self.local_input_mode != LocalInputMode::None {
            return self.local_preedit.clone();
        }
        self.get_pinyin_sequence_with_cases().to_string()
    }

    pub fn caret_position(&self) -> usize {
        let size = self.editing_text().len();
        self.caret.unwrap_or(size).min(size)
    }

    /// 把光标挪到某处而不改原始串。`None` 是串尾（整串解码，默认）。值夹在 `[0, editing_text().len()]`。
    /// 设置只更新状态，候选在下一次 `recompute_candidates` 或按键时按新边界重新解码。
    pub fn set_caret(&mut self, caret: Option<usize>) {
        let size = self.editing_text().len();
        self.caret = caret.map(|value| value.min(size));
    }

    /// 光标落在最近一个完整单元边界（向下取整）。光标未设置或方案没有单元模型时解整串。
    pub(super) fn quantized_prefix_end(&self) -> usize {
        let raw_length = self.get_pinyin_sequence_with_cases().len();
        if self.caret.is_none() {
            return raw_length;
        }
        // 切分契约：光标前缀只能对 segment_raw_boundaries 取整，不能对重新推出来的切分取整。
        let boundaries = self.segment_raw_boundaries();
        // 没有单元模型（五笔、日语、本地模式、专用英文）：不量化。
        if boundaries.is_empty() {
            return raw_length;
        }
        // 向下取整到光标处或之前的最后一个完整单元：末尾不完整的单元属于待处理后缀。boundaries 总含 0，所以前一个必存在。
        let caret = self.caret_position();
        let upper = boundaries.partition_point(|boundary| *boundary <= caret);
        boundaries[upper - 1]
    }

    pub fn prefix_end(&self) -> usize {
        self.quantized_prefix_end()
    }

    /// 这次解码没消耗的待处理输入，保留原来的大小写。前缀严格短于原串时才非空。
    pub fn pending_suffix(&self) -> String {
        let raw = self.get_pinyin_sequence_with_cases();
        let end = self.quantized_prefix_end();
        if end < raw.len() {
            raw.get(end..).unwrap_or("").to_string()
        } else {
            String::new()
        }
    }

    /// 输入单元的起点偏移（总含 0 和原串长度）。全拼、双拼有单元模型；五笔、日语、本地模式、专用英文返回空，
    /// 宿主退回逐字符编辑。
    pub fn segment_raw_boundaries(&self) -> Vec<usize> {
        // 本地模式和专用英文拼的是词不是音节：宿主按普通退格处理。
        if self.dedicated_english_mode || self.local_input_mode != LocalInputMode::None {
            return Vec::new();
        }
        let raw_with_cases = self.get_pinyin_sequence_with_cases();
        let raw = self.get_pinyin_sequence();
        if raw_with_cases.is_empty() {
            return Vec::new();
        }

        if self.current_scheme_type() == SchemeType::Shuangpin {
            // 句中辅助码段和直接辅助码的辅码字母混在音节之间，贪心切分算不出单元边界，改按请求带着的段布局切。
            if self.request().has_syllable_helpcode_layout {
                return syllable_helpcode_raw_boundaries(raw_with_cases, self.request());
            }
            let helpcode_length = sq::detect_active_double_helpcode_length(raw, raw_with_cases, self.profile);
            let base = if helpcode_length > 0 { sq::trim_trailing_letters_preserve_delimiters(raw_with_cases, helpcode_length) } else { raw_with_cases.to_string() };
            let mut boundaries = sq::segment_raw_boundaries(&base, self.profile);
            if helpcode_length > 0 && !boundaries.is_empty() {
                // 生效的双辅助码自成一个可编辑单元，和 raw_segmentation 在它前面画的边界一致。
                boundaries.push(base.len());
                boundaries.push(raw_with_cases.len());
                boundaries.sort_unstable();
                boundaries.dedup();
            }
            return boundaries;
        }

        if self.current_scheme_type() != SchemeType::Quanpin {
            return Vec::new();
        }
        quanpin_raw_boundaries(raw_with_cases, &self.get_pinyin_segmentation_with_cases())
    }

    pub(super) fn edit_at_caret(&mut self, command: Command) -> KeyResult {
        let mut text = self.editing_text();
        let mut caret = self.caret_position();
        // 本地模式的标记是命令不是可编辑内容：标记后面没有内容时的退格仍是原来的取消行为。
        let begin = if self.local_input_mode == LocalInputMode::None { 0 } else { 1 };
        match command {
            Command::MoveLeft => caret = if caret > begin { caret - 1 } else { begin },
            Command::MoveRight => caret = (caret + 1).min(text.len()),
            Command::MoveHome => caret = begin,
            Command::MoveEnd => caret = text.len(),
            Command::Backspace => {
                if caret <= begin {
                    return KeyResult::handled();
                }
                caret -= 1;
                text.remove(caret);
                return self.replace_editing_text(text, caret);
            }
            Command::DeleteForward => {
                if caret == text.len() {
                    return KeyResult::handled();
                }
                text.remove(caret);
                return self.replace_editing_text(text, caret);
            }
            _ => return KeyResult::default(),
        }
        self.caret = Some(caret);
        // 纯移动同样改了光标，所以要像插入、删除一样重新判断前缀边界；否则 candidates() 会一直给上一个边界的列表。
        if !self.dedicated_english_mode && self.local_input_mode == LocalInputMode::None {
            self.update_mixed_candidates();
        }
        KeyResult::handled()
    }

    pub(super) fn insert_at_caret(&mut self, character: u8) -> KeyResult {
        let mut text = self.editing_text();
        let caret = self.caret_position();
        let lower = character.is_ascii_lowercase();
        let upper = character.is_ascii_uppercase();
        let mut accepted = lower || upper;
        if !self.dedicated_english_mode {
            match self.local_input_mode {
                LocalInputMode::Unicode => {
                    accepted = character.is_ascii_digit() || (b'a'..=b'f').contains(&character) || (b'A'..=b'F').contains(&character) || (character == b'+' && caret == 1 && !text.contains('+'));
                }
                LocalInputMode::QuickPhrase => accepted = lower,
                // 唤醒词不在中间插字；指定的日期时间按与 TSF、Server 同一条形状规则收数字、/ 和 :。
                LocalInputMode::DateTime => accepted = input_contracts::date_time_accepts_at(text.as_bytes(), caret, character),
                LocalInputMode::None => {
                    let scheme = self.scheme();
                    accepted = lower
                        || (upper
                            && ((scheme == SchemeType::Quanpin && self.quanpin_helpcode_enabled)
                                || (scheme == SchemeType::Shuangpin && (self.shuangpin_helpcode_enabled || self.direct_helpcode_enabled))));
                    if character == b';' && scheme == SchemeType::Shuangpin && self.profile.uses_semicolon_final() {
                        accepted = if self.direct_helpcode_enabled {
                            input_contracts::direct_helpcode_accepts_semicolon_final_at(text.as_bytes(), caret)
                        } else {
                            mid_sentence::accepts_semicolon_final_at(text.as_bytes(), caret, self.engine.mid_sentence_uppercase_trigger_active())
                        };
                    }
                    if character == b'/' {
                        accepted = self.accepts_direct_helpcode_slash_at(caret);
                    }
                    if character == b'\'' && scheme != SchemeType::Wubi {
                        accepted = caret > 0;
                    }
                    // 光标移回句中补句中辅助码：反引号和它后面的码按光标前的部分判断。
                    if character == mid_sentence::MARKER {
                        accepted = self.accepts_mid_sentence_helpcode_marker_at(caret);
                    } else if upper && !accepted {
                        accepted = self.accepts_mid_sentence_code_at(caret, character);
                    }
                }
                LocalInputMode::Emoji | LocalInputMode::Kaomoji | LocalInputMode::TemporaryJapanese => accepted = accepted || character == b'\'',
                _ => {}
            }
        }
        if !accepted {
            return KeyResult::default();
        }
        let bytes = text.as_bytes();
        if character == b'\'' && ((caret > 0 && bytes.get(caret - 1) == Some(&b'\'')) || bytes.get(caret) == Some(&b'\'')) {
            return KeyResult::handled();
        }
        text.insert(caret, character as char);
        self.replace_editing_text(text, caret + 1)
    }

    fn replace_editing_text(&mut self, text: String, caret: usize) -> KeyResult {
        let mut diagnostic = None;
        if self.dedicated_english_mode {
            self.dedicated_english_preedit = text;
            self.update_dedicated_english_candidates();
        } else if self.local_input_mode != LocalInputMode::None && self.local_input_mode != LocalInputMode::TemporaryJapanese {
            self.local_preedit = text;
            diagnostic = self.update_local_candidates();
        } else {
            let payload = if self.local_input_mode == LocalInputMode::TemporaryJapanese { text.get(1..).unwrap_or("").to_string() } else { text };
            let normalized = payload.to_ascii_lowercase();
            self.set_pinyin_sequence(&normalized);
            self.set_pinyin_sequence_with_cases(&payload);
            self.apply_pending_sequence();
            if self.local_input_mode == LocalInputMode::TemporaryJapanese {
                self.local_preedit = format!("R{}", self.engine.preedit());
                self.local_candidates = self.engine.candidates().to_vec();
            }
        }
        self.caret = Some(caret);
        self.online_requests.invalidate();
        self.discard_abandoned_phrase_progress();
        // apply_pending_sequence 已经在光标清空时刷新过候选；现在光标回来了，重新判断前缀边界（光标在文本末尾关掉前缀解码，在中间重新解码）。
        if !self.dedicated_english_mode && self.local_input_mode == LocalInputMode::None {
            self.update_mixed_candidates();
        }
        KeyResult::with(true, None, diagnostic)
    }
}
