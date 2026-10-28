//! 双拼输入串的处理：分段、转全拼、手动分隔符、全码辅助长度、大小写还原，以及句中辅助码的解析。
//!
//! 移植自 `engine/shuangpin/shuangpin_query.cpp`（不含词典查询，那在 `shuangpin_dictionary.cpp`）。
//! 输入是 ASCII 键序列，按字节处理，和 C++ 一样。

use crate::mid_sentence;
use crate::shuangpin::{self, Profile};

/// 句中辅助码约束：第 `syllable` 个音节（按去掉反引号段后的切分计，0 起）上的字，辅助码第一码必须是 `first`，
/// 第二码是 `second`（0 表示没有）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct SyllableHelpcode {
    pub syllable: usize,
    pub first: u8,
    pub second: u8,
}

impl SyllableHelpcode {
    /// 辅助码表里查到的码（如 `ek`）是否满足这一条约束。
    pub fn matches(&self, code: &str) -> bool {
        let code = code.as_bytes();
        if code.is_empty() || code[0] != self.first {
            return false;
        }
        self.second == 0 || (code.len() > 1 && code[1] == self.second)
    }
}

/// 缓存键用的签名：同一串拼音带不同约束，候选不同，不能共用缓存。
pub fn syllable_helpcodes_signature(helpcodes: &[SyllableHelpcode]) -> String {
    let mut signature = String::new();
    for helpcode in helpcodes {
        signature.push('`');
        signature.push_str(&helpcode.syllable.to_string());
        signature.push(helpcode.first as char);
        if helpcode.second != 0 {
            signature.push(helpcode.second as char);
        }
    }
    signature
}

fn split_chunks(raw: &str) -> impl Iterator<Item = &str> {
    raw.split('\'')
}

/// 按手动分隔符 `'` 切成块，每块各自贪心分段，再用 `'` 接起来；空块不产生分隔符。
pub fn segment_input(raw_input: &str, profile: &Profile) -> String {
    let mut result = String::new();
    let mut first = true;
    for chunk in split_chunks(raw_input).filter(|c| !c.is_empty()) {
        if !first {
            result.push('\'');
        }
        result += &shuangpin::segment(chunk, profile);
        first = false;
    }
    result
}

/// 每个音节在原串里的起点偏移，非空时一定含 0 和原串长度。一个单元是一个音节（1-2 键），手动分隔符另起一块。
pub fn segment_raw_boundaries(raw_input: &str, profile: &Profile) -> Vec<usize> {
    let bytes = raw_input.as_bytes();
    let mut boundaries = Vec::new();
    if bytes.is_empty() {
        return boundaries;
    }
    boundaries.push(0);
    let mut chunk_start = 0usize;
    loop {
        let separator = bytes[chunk_start..].iter().position(|&b| b == b'\'').map(|i| chunk_start + i);
        let chunk_end = separator.unwrap_or(bytes.len());
        let mut position = chunk_start;
        while position < chunk_end {
            let two_key = chunk_end - position >= 2
                && shuangpin::is_accepted_syllable_code(&raw_input[position..position + 2].to_ascii_lowercase(), profile);
            position += if two_key { 2 } else { 1 };
            boundaries.push(position);
        }
        let Some(separator) = separator else { break };
        chunk_start = separator + 1;
        if chunk_start < bytes.len() {
            boundaries.push(chunk_start);
        }
        if chunk_start > bytes.len() {
            break;
        }
    }
    // 末尾的分隔符不开新单元，但拼写的末尾总属于最后一个单元；分隔符紧挨着块首产生的重复也并掉。
    if *boundaries.last().unwrap() != bytes.len() {
        boundaries.push(bytes.len());
    }
    boundaries.dedup();
    boundaries
}

pub fn to_quanpin_segmentation(segmented_input: &str, profile: &Profile) -> String {
    shuangpin::segmented_to_pinyin(segmented_input, profile)
}

pub fn normalize_input_with_delimiters(raw_input: &str, profile: &Profile) -> String {
    to_quanpin_segmentation(&segment_input(raw_input, profile), profile)
}

pub fn remove_manual_delimiters(text: &str) -> String {
    text.replace('\'', "")
}

pub fn normalize_input(raw_input: &str, profile: &Profile) -> String {
    remove_manual_delimiters(&normalize_input_with_delimiters(raw_input, profile))
}

pub fn effective_input_length(raw_input: &str) -> usize {
    remove_manual_delimiters(raw_input).len()
}

/// 把去掉分隔符后量出的长度映回原串，得到恰好含这么多有效字符的原串前缀长度。
pub fn raw_length_for_effective_prefix(raw_input: &str, effective_length: usize) -> usize {
    let bytes = raw_input.as_bytes();
    let (mut raw_length, mut effective_count) = (0usize, 0usize);
    while raw_length < bytes.len() && effective_count < effective_length {
        if bytes[raw_length] != b'\'' {
            effective_count += 1;
        }
        raw_length += 1;
    }
    raw_length
}

/// 去掉末尾 `letter_count` 个有效字符，保留它们前面的所有分隔符。
pub fn trim_trailing_letters_preserve_delimiters(raw_input: &str, letter_count: usize) -> String {
    if letter_count == 0 || raw_input.is_empty() {
        return raw_input.to_string();
    }
    let bytes = raw_input.as_bytes();
    let mut remaining = letter_count;
    let mut pos = bytes.len();
    while pos > 0 {
        pos -= 1;
        if bytes[pos] == b'\'' {
            continue;
        }
        remaining -= 1;
        if remaining == 0 {
            return raw_input[..pos].to_string();
        }
    }
    String::new()
}

/// 输入串末尾是不是正在敲双辅助码：是则返回 2，否则 0。
pub fn detect_active_double_helpcode_length(raw_input: &str, raw_input_with_cases: &str, profile: &Profile) -> usize {
    let effective_input = remove_manual_delimiters(raw_input);
    let effective_input_with_cases = remove_manual_delimiters(if raw_input_with_cases.is_empty() { raw_input } else { raw_input_with_cases });
    if !shuangpin::is_full_help_mode(&effective_input_with_cases, profile) {
        return 0;
    }
    let base_length = effective_input.len().wrapping_sub(2);
    let raw_base_length = raw_length_for_effective_prefix(raw_input, base_length);
    if raw_base_length < raw_input.len() && raw_input.as_bytes()[raw_base_length] == b'\'' {
        return 0;
    }
    if is_complete_input(&raw_input[..raw_base_length.min(raw_input.len())], profile) {
        2
    } else {
        0
    }
}

/// 每个手动分隔的块都是整齐的完整音节。
pub fn is_complete_input(raw_input: &str, profile: &Profile) -> bool {
    if raw_input.is_empty() || raw_input.starts_with('\'') || raw_input.ends_with('\'') || raw_input.contains("''") {
        return false;
    }
    split_chunks(raw_input).all(|chunk| !chunk.is_empty() && shuangpin::all_units_complete(chunk, &shuangpin::segment(chunk, profile)))
}

/// 把原串的大小写还原到小写的切分串上；对不上就原样返回切分串。
pub fn apply_segmentation_cases(segmented_input: &str, raw_input_with_cases: &str) -> String {
    if segmented_input.is_empty() || raw_input_with_cases.is_empty() {
        return String::new();
    }
    let extracted: String = segmented_input.chars().filter(|&c| c != '\'').collect();
    if extracted != remove_manual_delimiters(raw_input_with_cases).to_ascii_lowercase() {
        return segmented_input.to_string();
    }
    let raw = raw_input_with_cases.as_bytes();
    let mut result = String::with_capacity(segmented_input.len());
    let mut index = 0usize;
    for ch in segmented_input.bytes() {
        if ch == b'\'' {
            result.push('\'');
            continue;
        }
        if index >= raw.len() {
            return segmented_input.to_string();
        }
        while index < raw.len() && raw[index] == b'\'' {
            index += 1;
        }
        if index >= raw.len() {
            return segmented_input.to_string();
        }
        let cased = raw[index];
        result.push(if ch == cased || ch == cased.wrapping_add(b'a' - b'A') { cased } else { ch } as char);
        index += 1;
    }
    result
}

// ---------------------------------------------------------------------------------------------
// 句中辅助码
// ---------------------------------------------------------------------------------------------

/// 解析结果：把每个辅助码段换成 `'` 之后的输入串，加上约束和每段原样的文本。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct MidSentenceHelpcodeInput {
    /// 每段换成一个 `'` 之后的输入串，保留大小写。
    pub input: String,
    /// `input[i]` 在原串里的下标，末尾多一项等于原串长度。
    pub source_index: Vec<usize>,
    pub helpcodes: Vec<SyllableHelpcode>,
    /// 每段原样的文本（含还没敲码的光杆反引号）及它挂在哪个音节后面。
    pub decorations: Vec<(usize, String)>,
}

fn count_segments(segmentation: &str) -> usize {
    if segmentation.is_empty() {
        0
    } else {
        segmentation.bytes().filter(|&b| b == b'\'').count() + 1
    }
}

pub fn has_mid_sentence_helpcode(raw_input: &str, uppercase_trigger: bool) -> bool {
    raw_input.as_bytes().contains(&mid_sentence::MARKER) || (uppercase_trigger && !mid_sentence::scan_blocks(raw_input.as_bytes(), true).0.is_empty())
}

pub fn parse_mid_sentence_helpcodes(raw_input_with_cases: &str, profile: &Profile, uppercase_trigger: bool) -> MidSentenceHelpcodeInput {
    let raw = raw_input_with_cases.as_bytes();
    let mut parsed = MidSentenceHelpcodeInput::default();
    let mut input: Vec<u8> = Vec::with_capacity(raw.len());
    let blocks = mid_sentence::scan_blocks(raw, uppercase_trigger).0;
    let mut next_block = blocks.iter().peekable();
    let mut index = 0usize;
    while index < raw.len() {
        let Some(block) = next_block.next_if(|b| b.begin == index).copied() else {
            input.push(raw[index]);
            parsed.source_index.push(index);
            index += 1;
            continue;
        };
        let syllables = count_segments(&segment_input(&String::from_utf8_lossy(&input).to_ascii_lowercase(), profile));
        if syllables > 0 {
            let syllable = syllables - 1;
            parsed.decorations.push((syllable, String::from_utf8_lossy(&raw[index..block.end]).into_owned()));
            if block.end > block.codes_begin {
                let helpcode = SyllableHelpcode {
                    syllable,
                    first: raw[block.codes_begin].to_ascii_lowercase(),
                    second: if block.end > block.codes_begin + 1 { raw[block.codes_begin + 1].to_ascii_lowercase() } else { 0 },
                };
                // 同一个音节敲了两段，以后一段为准。
                match parsed.helpcodes.iter_mut().find(|item| item.syllable == syllable) {
                    Some(existing) => *existing = helpcode,
                    None => parsed.helpcodes.push(helpcode),
                }
            }
        }
        // 反引号段同时是一个确定的音节边界，换成手动分隔符；已经有分隔符就不再叠一个。
        if input.last().is_some_and(|&last| last != b'\'') {
            input.push(b'\'');
            parsed.source_index.push(index);
        }
        index = block.end;
    }
    parsed.source_index.push(raw.len());
    parsed.input = String::from_utf8_lossy(&input).into_owned();
    parsed
}

/// 把每段反引号段原样接回切分串里对应音节的后面：`ul'pb'ih'fa` → ``ul'pb`x'ih'fa``。
pub fn decorate_segmentation(segmentation: &str, decorations: &[(usize, String)]) -> String {
    if decorations.is_empty() || segmentation.is_empty() {
        return segmentation.to_string();
    }
    let mut parts: Vec<String> = segmentation.split('\'').map(str::to_string).collect();
    for (syllable, text) in decorations {
        if let Some(part) = parts.get_mut(*syllable) {
            part.push_str(text);
        }
    }
    parts.join("'")
}

pub fn decorate_mid_sentence_segmentation(segmentation: &str, raw_input_with_cases: &str, profile: &Profile, uppercase_trigger: bool) -> String {
    if !has_mid_sentence_helpcode(raw_input_with_cases, uppercase_trigger) || segmentation.is_empty() {
        return segmentation.to_string();
    }
    decorate_segmentation(segmentation, &parse_mid_sentence_helpcodes(raw_input_with_cases, profile, uppercase_trigger).decorations)
}

pub fn accepts_mid_sentence_helpcode_marker(raw_input: &str, uppercase_trigger: bool) -> bool {
    mid_sentence::accepts_marker(raw_input.as_bytes(), uppercase_trigger)
}

pub fn accepts_mid_sentence_helpcode_marker_at(raw_input: &str, caret: usize, uppercase_trigger: bool) -> bool {
    mid_sentence::accepts_marker_at(raw_input.as_bytes(), caret, uppercase_trigger)
}

pub fn accepts_mid_sentence_second_code(raw_input: &str, ch: u8, uppercase_trigger: bool) -> bool {
    ch.is_ascii_uppercase() && mid_sentence::accepts_second_code_at(raw_input.as_bytes(), raw_input.len(), uppercase_trigger)
}
