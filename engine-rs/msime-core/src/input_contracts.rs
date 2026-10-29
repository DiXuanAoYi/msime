//! 输入形状契约：Shift+T 日期时间模式的输入形状（`engine/contracts/date_time_input.h`）和双拼直接辅助码的按键形状
//! （`engine/contracts/direct_helpcode.h`）。TSF、Server、引擎三处共用同一条规则，这里是引擎一侧的 Rust 版本。
//!
//! 只看形状：月份、日期、时分秒的取值由引擎检查，取值不对时不出候选。

// ---------- Shift+T 日期时间输入形状 ----------

pub const DATE_TIME_MODE_PREFIX: u8 = b'T';
/// 最长的形状是 YYYYMMDDhh:mm:ss。
pub const DATE_TIME_MAX_BODY_LENGTH: usize = 16;
const MAX_GROUPS: usize = 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DateTimeShape {
    None,
    Date,
    DateTime,
    YearMonthDay,
    YearMonth,
    MonthDay,
    Time,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Group {
    pub start: usize,
    pub length: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DateTimeMatch {
    pub shape: DateTimeShape,
    pub group_count: usize,
    pub groups: [Group; MAX_GROUPS],
}

impl Default for DateTimeMatch {
    fn default() -> Self {
        Self { shape: DateTimeShape::None, group_count: 0, groups: [Group::default(); MAX_GROUPS] }
    }
}

struct Pattern {
    separator: u8,
    max_groups: usize,
    min_complete_groups: usize,
    min_length: [usize; MAX_GROUPS],
    max_length: [usize; MAX_GROUPS],
    shape_by_groups: [DateTimeShape; MAX_GROUPS],
}

use DateTimeShape as S;

/// 顺序就是写完时的认定顺序：20241225 同时是 YYYYMMDD 写完和 YYYYMMDDhh 没写完，按前者算。
const PATTERNS: [Pattern; 5] = [
    Pattern { separator: 0, max_groups: 1, min_complete_groups: 1, min_length: [8, 0, 0], max_length: [8, 0, 0], shape_by_groups: [S::Date, S::None, S::None] },
    Pattern { separator: b':', max_groups: 3, min_complete_groups: 1, min_length: [10, 2, 2], max_length: [10, 2, 2], shape_by_groups: [S::DateTime, S::DateTime, S::DateTime] },
    Pattern { separator: b'/', max_groups: 3, min_complete_groups: 2, min_length: [4, 1, 1], max_length: [4, 2, 2], shape_by_groups: [S::None, S::YearMonth, S::YearMonthDay] },
    Pattern { separator: b'/', max_groups: 2, min_complete_groups: 2, min_length: [1, 1, 0], max_length: [2, 2, 0], shape_by_groups: [S::None, S::MonthDay, S::None] },
    Pattern { separator: b':', max_groups: 3, min_complete_groups: 2, min_length: [1, 2, 2], max_length: [2, 2, 2], shape_by_groups: [S::None, S::Time, S::Time] },
];

/// 把 T 后面那一串切成数字段：只能含数字、/ 和 :，最多三段；有多个分隔符时必须是同一种。
fn split(body: &[u8]) -> Option<([Group; MAX_GROUPS], usize, u8)> {
    let mut groups = [Group::default(); MAX_GROUPS];
    let mut count = 1;
    let mut separator = 0u8;
    for (index, &ch) in body.iter().enumerate() {
        if ch.is_ascii_digit() {
            groups[count - 1].length += 1;
            continue;
        }
        if ch != b'/' && ch != b':' {
            return None;
        }
        if (separator != 0 && ch != separator) || count == MAX_GROUPS {
            return None;
        }
        separator = ch;
        groups[count] = Group { start: index + 1, length: 0 };
        count += 1;
    }
    Some((groups, count, separator))
}

/// `complete` 为 false 时只要求还能接成这个形状：最后一段可以没写完，紧跟在分隔符后面时甚至可以是空的。
fn fits(pattern: &Pattern, groups: &[Group; MAX_GROUPS], count: usize, separator: u8, complete: bool) -> bool {
    if count > pattern.max_groups || (count > 1 && separator != pattern.separator) || (complete && count < pattern.min_complete_groups) {
        return false;
    }
    for index in 0..count {
        let length = groups[index].length;
        let last = index + 1 == count;
        if length > pattern.max_length[index] || (length < pattern.min_length[index] && (complete || !last)) {
            return false;
        }
    }
    true
}

/// T 后面那一串是否以数字开头，即按指定日期时间而不是唤醒词处理。
pub fn is_specific_body(body: &[u8]) -> bool {
    body.first().is_some_and(u8::is_ascii_digit)
}

/// T 后面那一串写完了时是哪种形状，各段在哪里；没写完或不是这些形状时 shape 为 `None`。
pub fn match_complete(body: &[u8]) -> DateTimeMatch {
    if body.is_empty() || body.len() > DATE_TIME_MAX_BODY_LENGTH {
        return DateTimeMatch::default();
    }
    let Some((groups, count, separator)) = split(body) else { return DateTimeMatch::default() };
    for pattern in &PATTERNS {
        if fits(pattern, &groups, count, separator, true) {
            return DateTimeMatch { shape: pattern.shape_by_groups[count - 1], group_count: count, groups };
        }
    }
    // C++ 没认出形状时返回的是空的 Match，段数也是 0。
    DateTimeMatch::default()
}

/// T 后面那一串是否还能接成某一种形状（空串也算：刚进 T 模式）。
pub fn is_viable_prefix(body: &[u8]) -> bool {
    if body.is_empty() {
        return true;
    }
    if body.len() > DATE_TIME_MAX_BODY_LENGTH {
        return false;
    }
    let Some((groups, count, separator)) = split(body) else { return false };
    PATTERNS.iter().any(|pattern| fits(pattern, &groups, count, separator, false))
}

/// 整个输入串 `text`（开头是 T）里，光标停在 `text[caret]` 时能否插入 `ch`（数字、/ 或 :）。插入后 T 后面那一串
/// 还得能接成某一种形状，所以 Trq 后面不收数字，2024/12/25 后面也不再收。
pub fn date_time_accepts_at(text: &[u8], caret: usize, ch: u8) -> bool {
    let size = text.len();
    if size == 0 || text[0] != DATE_TIME_MODE_PREFIX || caret < 1 || caret > size || size > DATE_TIME_MAX_BODY_LENGTH || (!ch.is_ascii_digit() && ch != b'/' && ch != b':') {
        return false;
    }
    let mut body = Vec::with_capacity(size);
    for index in 1..=size {
        if index == caret {
            body.push(ch);
        }
        if index < size {
            body.push(text[index]);
        }
    }
    is_viable_prefix(&body)
}

// ---------- 双拼直接辅助码的按键形状 ----------

/// 光标停在 `text[caret]` 时能否插入 /：光标前是「两键音节 + 两个辅码字母」的形状（音节第二键可以是 ; 韵母），
/// 且光标后不是已经有一个 /。
pub fn direct_helpcode_accepts_slash_at(text: &[u8], caret: usize) -> bool {
    let size = text.len();
    if caret < 4 || caret > size || (caret < size && text[caret] == b'/') {
        return false;
    }
    text[caret - 1].is_ascii_alphabetic() && text[caret - 2].is_ascii_alphabetic() && (text[caret - 3].is_ascii_alphabetic() || text[caret - 3] == b';') && text[caret - 4].is_ascii_alphabetic()
}

/// 光标停在 `text[caret]` 时 ; 能否作为 ing 韵母（只对用 ; 当韵母的双拼方案有意义）。
pub fn direct_helpcode_accepts_semicolon_final_at(text: &[u8], caret: usize) -> bool {
    caret >= 1 && caret <= text.len() && text[caret - 1].is_ascii_alphabetic()
}
