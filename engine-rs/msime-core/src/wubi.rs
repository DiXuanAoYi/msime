//! 五笔：按键方案（`engine/schemes/wubi_scheme.cpp`）和码表查询（`engine/providers/wubi_candidate_provider.cpp`）。
//!
//! 码表是 `msime.db` 里的 `wubi86` 表，只读打开。调频、删词、造词写用户词库的那几个入口还在 C++。

use crate::lattice::Source;
use rusqlite::{Connection, OpenFlags};
use std::collections::HashSet;
use std::path::Path;

pub const VK_BACKSPACE: u32 = 0x08;
pub const VK_RETURN: u32 = 0x0D;
pub const VK_ESCAPE: u32 = 0x1B;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct KeyStroke {
    pub vk: u32,
    pub modifiers: u32,
    pub wch: u16,
}

/// 方案交给查询层的请求（只带五笔用得到的字段）。
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Request {
    pub code: String,
    pub key_strokes: Vec<KeyStroke>,
    pub z_wildcard: bool,
    pub valid: bool,
}

/// 码表候选。`pinyin` 是码表里的码，五笔没有规范读音。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Candidate {
    pub pinyin: String,
    pub canonical_pinyin: String,
    pub word: String,
    pub weight: i64,
    pub source: Source,
}

const MAX_CODE_LENGTH: usize = 4;
/// 够长到装下任何拼写，又短到仍是一次组合而不是一整段话。
const MAX_MIXED_CODE_LENGTH: usize = 32;
/// 前缀查询的截断上限：候选窗每页最多 10 个，50 给固定位置和重排留余量。
const QUERY_ROW_LIMIT: usize = 50;

fn is_wubi_letter(lower: u8, mixed_pinyin_allowed: bool, z_wildcard: bool) -> bool {
    // 两个开关都是为了放 z 进来：混输时 z 是拼音字母，通配时 z 代表一个不知道的码。
    (b'a'..=b'y').contains(&lower) || (lower == b'z' && (mixed_pinyin_allowed || z_wildcard))
}

#[derive(Debug, Clone, Default)]
pub struct Scheme {
    raw: String,
    key_strokes: Vec<KeyStroke>,
    extended_length_allowed: bool,
    mixed_pinyin_allowed: bool,
    z_wildcard: bool,
}

impl Scheme {
    pub fn new() -> Self {
        Self::default()
    }

    /// 清空输入；`mixed_pinyin_allowed` 和 `z_wildcard` 是设置，不随组合清掉，长度放宽跟着上一次查询走，会清。
    pub fn reset(&mut self) {
        self.raw.clear();
        self.key_strokes.clear();
        self.extended_length_allowed = false;
    }

    fn max_code_length(&self) -> usize {
        if self.extended_length_allowed {
            MAX_MIXED_CODE_LENGTH
        } else {
            MAX_CODE_LENGTH
        }
    }

    pub fn handle_key(&mut self, vk: u32, modifiers: u32, wch: u16) {
        if vk == VK_BACKSPACE {
            self.raw.pop();
            self.key_strokes.pop();
            return;
        }
        if vk == VK_ESCAPE || vk == VK_RETURN {
            self.reset();
            return;
        }
        let is_letter = (u32::from(b'A')..=u32::from(b'Z')).contains(&vk)
            && is_wubi_letter((vk as u8) + (b'a' - b'A'), self.mixed_pinyin_allowed, self.z_wildcard);
        if !is_letter || self.raw.len() >= self.max_code_length() {
            return;
        }
        self.key_strokes.push(KeyStroke { vk, modifiers, wch });
        self.raw.push(((vk as u8) + (b'a' - b'A')) as char);
    }

    pub fn set_raw_input(&mut self, raw: &str, raw_with_cases: &str) {
        let source = if raw_with_cases.is_empty() { raw } else { raw_with_cases };
        let limit = self.max_code_length();
        let mut normalized = String::new();
        for byte in source.bytes() {
            let lower = byte.to_ascii_lowercase();
            if !is_wubi_letter(lower, self.mixed_pinyin_allowed, self.z_wildcard) {
                continue;
            }
            normalized.push(lower as char);
            if normalized.len() == limit {
                break;
            }
        }
        self.raw = normalized;
        self.key_strokes.clear();
    }

    /// 混输时码表已经答不出手里的码才放宽长度，让 nihao、zhongguo 这样的拼写能打完。
    pub fn set_extended_length_allowed(&mut self, allowed: bool) {
        self.extended_length_allowed = allowed;
    }

    pub fn set_mixed_pinyin_allowed(&mut self, allowed: bool) {
        self.mixed_pinyin_allowed = allowed;
    }

    pub fn set_z_wildcard(&mut self, enabled: bool) {
        self.z_wildcard = enabled;
    }

    /// 完整五笔码就是四个字母；放宽长度之后更长的拼写是拼音的答案，不算完整的五笔码。
    pub fn has_complete_code(&self) -> bool {
        self.raw.len() == MAX_CODE_LENGTH
    }

    pub fn preedit(&self) -> &str {
        &self.raw
    }

    pub fn build_request(&self) -> Request {
        Request {
            code: self.raw.clone(),
            key_strokes: self.key_strokes.clone(),
            z_wildcard: self.z_wildcard && self.raw.contains('z'),
            valid: !self.raw.is_empty(),
        }
    }
}

pub struct Dictionary {
    connection: Connection,
}

impl Dictionary {
    pub fn open(path: &Path) -> rusqlite::Result<Self> {
        let connection = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_URI)?;
        Ok(Self { connection })
    }

    fn collect(&self, sql: &str, params: impl rusqlite::Params) -> Vec<Candidate> {
        let Ok(mut statement) = self.connection.prepare(sql) else { return Vec::new() };
        let Ok(rows) = statement.query_map(params, |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, i64>(2)?))) else {
            return Vec::new();
        };
        // 同一个字常同时有简码与全码（工 = a / aaaa），前缀查询会把两行都带出来。
        // 按 ORDER BY 顺序只保留每个词的第一行：精确码行排在最前，调频也就落在用户实际敲的那个码上。
        let mut seen = HashSet::new();
        let mut candidates = Vec::new();
        for row in rows {
            let Ok((key, value, weight)) = row else { return Vec::new() };
            if !seen.insert(value.clone()) {
                continue;
            }
            candidates.push(Candidate { pinyin: key, canonical_pinyin: String::new(), word: value, weight, source: Source::Database });
        }
        candidates
    }

    pub fn query(&self, request: &Request) -> Vec<Candidate> {
        if !request.valid || request.code.is_empty() {
            return Vec::new();
        }
        if request.z_wildcard {
            // 通配码：z 代表一个不知道的字母，所以模式是把 z 换成 GLOB 的 '?'、结尾补 '*'。
            // 用 GLOB 不用 LIKE：码是小写 ASCII、key 列是 BINARY，字面前缀仍能约束索引扫描。
            let mut pattern: String = request.code.chars().map(|c| if c == 'z' { '?' } else { c }).collect();
            pattern.push('*');
            return self.collect(
                &format!(
                    "SELECT \"key\", \"value\", \"weight\" FROM wubi86 WHERE \"key\" GLOB ?1 ORDER BY \"weight\" DESC, \"key\" ASC, rowid ASC LIMIT {QUERY_ROW_LIMIT}"
                ),
                [pattern],
            );
        }
        // 上界 = 前缀末字节 +1：五笔码字母域是 a–y，末字节自增不会越过域边界。
        let mut upper = request.code.clone().into_bytes();
        *upper.last_mut().expect("非空") += 1;
        let upper = String::from_utf8_lossy(&upper).into_owned();
        self.collect(
            &format!(
                "SELECT \"key\", \"value\", \"weight\" FROM wubi86 WHERE \"key\" >= ?1 AND \"key\" < ?2 ORDER BY (\"key\" = ?1) DESC, \"weight\" DESC, \"key\" ASC, rowid ASC LIMIT {QUERY_ROW_LIMIT}"
            ),
            [request.code.as_str(), upper.as_str()],
        )
    }
}
