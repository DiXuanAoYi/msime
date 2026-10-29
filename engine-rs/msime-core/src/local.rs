//! 本地模式里查词库的几种：表情、颜文字、超级简拼、快捷短语，以及快捷短语的插入位置。
//! 对应 `engine/local_modes/{emoji,kaomoji,jianpin,quick_phrase}_query.cpp`。
//!
//! 库一律只读打开。C++ 里为出货词库做的共享连接缓存（`local_database.cpp`）只是性能手段，这里每次查询打开一个连接。

use crate::lattice::{Positioned, Source};
use crate::shuangpin::{self, Profile};
use rusqlite::{Connection, OpenFlags};
use std::collections::HashSet;
use std::path::Path;

/// 本地模式候选（`WordItem` 里这些模式用得到的字段）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Item {
    pub pinyin: String,
    pub canonical_pinyin: String,
    pub word: String,
    pub weight: i64,
    pub source: Source,
    pub fixed_position: i32,
}

impl Item {
    pub fn new(pinyin: &str, word: &str, weight: i64, source: Source) -> Self {
        Self { pinyin: pinyin.to_string(), canonical_pinyin: String::new(), word: word.to_string(), weight, source, fixed_position: 0 }
    }
}

impl Positioned for Item {
    fn word(&self) -> &str {
        &self.word
    }
    fn source(&self) -> Source {
        self.source
    }
    fn fixed_position(&self) -> i32 {
        self.fixed_position
    }
    fn set_fixed_position(&mut self, position: i32) {
        self.fixed_position = position;
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct QueryResult {
    pub items: Vec<Item>,
    pub diagnostic: Option<String>,
}

fn failure(diagnostic: &str) -> QueryResult {
    QueryResult { items: Vec::new(), diagnostic: Some(diagnostic.to_string()) }
}

fn open(path: &Path) -> Option<Connection> {
    if path.as_os_str().is_empty() {
        return None;
    }
    Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_URI).ok()
}

// ---------- 表情 / 颜文字 ----------

/// 双拼先解成全拼（去掉手动分隔符）。
fn shuangpin_to_quanpin(lower: &str, profile: &Profile) -> String {
    shuangpin::segmented_to_pinyin(&shuangpin::segment(lower, profile), profile).replace('\'', "")
}

/// 表情和颜文字共用一套流程，只有表名和 SQL 不同。
fn query_emoticon(code: &str, shuangpin_scheme: bool, database_path: &Path, limit: i32, profile: &Profile, sql: &str, source: Source, name: &str) -> QueryResult {
    let valid = !code.is_empty() && code.bytes().all(|b| b.is_ascii_alphabetic() || b == b'\'');
    if !valid || limit <= 0 {
        return QueryResult::default();
    }
    let unavailable = format!("{name} database is unavailable.");
    let broken = format!("{name} database could not be queried.");
    let Some(database) = open(database_path) else { return failure(&unavailable) };

    let lower = code.to_ascii_lowercase();
    // 每个查找码带一个"精确命中是否提级"的开关：双拼下原始码只按目录顺序参与前缀命中，精确命中只认展开后的全拼
    // （原始码恰好等于某个简拼或英文键时，不能压过用户想打的全拼）。
    let mut lookups: Vec<(String, bool)> = vec![(lower.clone(), true)];
    if shuangpin_scheme {
        let quanpin = shuangpin_to_quanpin(&lower, profile);
        if !quanpin.is_empty() && quanpin != lower {
            lookups[0].1 = false;
            lookups.push((quanpin, true));
        }
    }

    // SQL 只准备一次，对每个查找码重新绑定；返回 (文字, 目录序, 是否精确)，精确行在前，免得每个查找的 LIMIT 截掉精确命中。
    let Ok(mut statement) = database.prepare(sql) else { return failure(&broken) };
    let mut entries: Vec<(String, i64, i64)> = Vec::new(); // (文字, 档位, 目录序)
    for (prefix, exact_ranks) in &lookups {
        let upper_bound = format!("{prefix}\u{7f}");
        let Ok(rows) = statement.query_map(rusqlite::params![prefix, upper_bound, limit], |row| Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?, row.get::<_, i64>(2)?))) else {
            return failure(&broken);
        };
        for row in rows {
            let Ok((text, order, exact)) = row else { return failure(&broken) };
            let tier = if *exact_ranks && exact != 0 { 0 } else { 1 };
            entries.push((text, tier, order));
        }
    }
    // 先排序再去重：同一文字被多个查找或多个键命中时保留最好的名次。
    entries.sort_by_key(|(_, tier, order)| (*tier, *order));
    let mut seen = HashSet::new();
    let mut texts: Vec<String> = Vec::new();
    for (text, _, _) in entries {
        if texts.len() >= limit as usize {
            break;
        }
        if seen.insert(text.clone()) {
            texts.push(text);
        }
    }
    let count = texts.len();
    QueryResult {
        items: texts.into_iter().enumerate().map(|(index, text)| Item::new(&lower, &text, (count - index) as i64, source)).collect(),
        diagnostic: None,
    }
}

pub fn query_emoji(code: &str, shuangpin_scheme: bool, database_path: &Path, limit: i32, profile: &Profile) -> QueryResult {
    query_emoticon(
        code,
        shuangpin_scheme,
        database_path,
        limit,
        profile,
        "SELECT emoji,sort_order,key IS ?1 FROM emoji_pinyin WHERE key>=?1 AND key<?2 ORDER BY key IS NOT ?1,sort_order LIMIT ?3",
        Source::Emoji,
        "Emoji",
    )
}

pub fn query_kaomoji(code: &str, shuangpin_scheme: bool, database_path: &Path, limit: i32, profile: &Profile) -> QueryResult {
    query_emoticon(
        code,
        shuangpin_scheme,
        database_path,
        limit,
        profile,
        "SELECT kaomoji,sort_order,pinyin IS ?1 FROM kaomoji WHERE (pinyin>=?1 AND pinyin<?2) OR (jianpin>=?1 AND jianpin<?2) ORDER BY pinyin IS NOT ?1,sort_order LIMIT ?3",
        Source::Kaomoji,
        "Kaomoji",
    )
}

// ---------- 超级简拼 ----------

fn normalize_jianpin_code(code: &str) -> String {
    let lower = code.to_ascii_lowercase();
    if lower.bytes().all(|b| b.is_ascii_lowercase()) { lower } else { String::new() }
}

/// 双拼的键解成声母（`u` → `sh`）；不是声母键就是它自己。
fn decode_initial(key: char, profile: &Profile) -> String {
    let mapped = key.to_string();
    profile.initials.iter().find(|(_, value)| **value == mapped).map(|(initial, _)| initial.clone()).unwrap_or(mapped)
}

fn expand_code(normalized: &str, shuangpin_scheme: bool, profile: &Profile) -> Vec<String> {
    normalized.chars().map(|c| if shuangpin_scheme { decode_initial(c, profile) } else { c.to_string() }).collect()
}

fn syllable_initial(syllable: &str) -> &str {
    if syllable.len() >= 2 && matches!(&syllable[..2], "zh" | "ch" | "sh") {
        return &syllable[..2];
    }
    syllable.get(..1).unwrap_or("")
}

fn key_matches_initials(key: &str, initials: &[String]) -> bool {
    let syllables: Vec<&str> = key.split('\'').collect();
    syllables.len() == initials.len() && syllables.iter().zip(initials).all(|(syllable, initial)| syllable_initial(syllable) == initial)
}

/// 词库分表名：`tbl_{音节数}_{首字母}`，8 个及以上是 `others`；首字母不是 a–z 时没有表名。
fn jianpin_table(segments: &[String]) -> String {
    let Some(initial) = segments.first().and_then(|s| s.chars().next()) else { return String::new() };
    if !initial.is_ascii_lowercase() {
        return String::new();
    }
    if segments.len() <= 7 { format!("tbl_{}_{initial}", segments.len()) } else { format!("tbl_others_{initial}") }
}

pub fn jianpin_ranking_context(code: &str, shuangpin_scheme: bool, profile: &Profile) -> String {
    let normalized = normalize_jianpin_code(code);
    if normalized.is_empty() { String::new() } else { expand_code(&normalized, shuangpin_scheme, profile).join("'") }
}

pub fn query_jianpin(code: &str, shuangpin_scheme: bool, database_path: &Path, limit: i32, profile: &Profile) -> QueryResult {
    if limit <= 0 {
        return QueryResult::default();
    }
    let normalized = normalize_jianpin_code(code);
    if normalized.is_empty() {
        return QueryResult::default();
    }
    let segments = expand_code(&normalized, shuangpin_scheme, profile);
    let table = jianpin_table(&segments);
    if table.is_empty() {
        return QueryResult::default();
    }
    let Some(database) = open(database_path) else { return failure("Super-jianpin database is unavailable.") };
    let jianpin: String = segments.iter().filter_map(|s| s.chars().next()).collect();
    let filter_initials = shuangpin_scheme;
    let scan_limit = if !filter_initials || limit > 100_000_000 { limit } else { (limit * 32).max(512) };
    let sql = format!("SELECT \"key\",\"value\",\"weight\" FROM \"{table}\" WHERE \"jp\"=?1 ORDER BY \"weight\" DESC LIMIT ?2");
    let Ok(mut statement) = database.prepare(&sql) else { return failure("Super-jianpin database could not be queried.") };
    let Ok(rows) = statement.query_map(rusqlite::params![jianpin, scan_limit], |row| {
        Ok((row.get::<_, Option<String>>(0)?, row.get::<_, Option<String>>(1)?, row.get::<_, i64>(2)?))
    }) else {
        return failure("Super-jianpin database could not be queried.");
    };
    let matched_code = segments.join("'");
    let mut result = QueryResult::default();
    for row in rows {
        let Ok((key, value, weight)) = row else { return failure("Super-jianpin database could not be queried.") };
        let Some(value) = value else { continue };
        let canonical = key.unwrap_or_default();
        if filter_initials && !key_matches_initials(&canonical, &segments) {
            continue;
        }
        result.items.push(Item { pinyin: matched_code.clone(), canonical_pinyin: canonical, word: value, weight, source: Source::Database, fixed_position: 0 });
        if result.items.len() as i32 >= limit {
            break;
        }
    }
    result
}

// ---------- 快捷短语 ----------

fn query_quick_phrase_rows(code: &str, database_path: &Path, limit: i32, exact: bool) -> QueryResult {
    let valid = !code.is_empty() && code.bytes().all(|b| b.is_ascii_lowercase());
    if !valid || limit <= 0 {
        return QueryResult::default();
    }
    let Some(database) = open(database_path) else { return failure("Quick phrase database is unavailable.") };
    let sql = if exact {
        "SELECT key,value,weight FROM quick_parases WHERE key=?1 AND key=?2 ORDER BY weight DESC,value LIMIT ?3"
    } else {
        "SELECT key,value,weight FROM quick_parases WHERE key>=?1 AND key<?2 ORDER BY weight DESC,key,value LIMIT ?3"
    };
    let Ok(mut statement) = database.prepare(sql) else { return failure("Quick phrase database could not be queried.") };
    let upper = if exact { code.to_string() } else { format!("{code}\u{7f}") };
    let Ok(rows) = statement.query_map(rusqlite::params![code, upper, limit], |row| {
        Ok((row.get::<_, Option<String>>(0)?, row.get::<_, Option<String>>(1)?, row.get::<_, i64>(2)?))
    }) else {
        return failure("Quick phrase database could not be queried.");
    };
    let mut result = QueryResult::default();
    for row in rows {
        let Ok((key, value, weight)) = row else { return failure("Quick phrase database could not be queried.") };
        if let (Some(key), Some(value)) = (key, value) {
            result.items.push(Item::new(&key, &value, weight, Source::QuickPhrase));
        }
    }
    result
}

/// K 模式：按编码前缀匹配，权重降序。
pub fn query_quick_phrases(prefix: &str, database_path: &Path, limit: i32) -> QueryResult {
    query_quick_phrase_rows(prefix, database_path, limit, false)
}

/// 混进全拼/双拼普通候选的那组：按编码精确匹配，组内按权重降序。
pub fn query_quick_phrases_by_code(code: &str, database_path: &Path, limit: i32) -> QueryResult {
    query_quick_phrase_rows(code, database_path, limit, true)
}

/// 快捷短语组的槽位只数这些候选：词库、用户词库和整句。云/AI/英文/表情/颜文字按各自的异步槽位插队，
/// 固定位置的候选按自己的位置放，都不参与计数。
pub fn counts_toward_quick_phrase_slot<T: Positioned>(item: &T) -> bool {
    if item.fixed_position() > 0 {
        return false;
    }
    !matches!(
        item.source(),
        Source::QuickPhrase | Source::CloudSuggestion | Source::AiSuggestion | Source::EnglishDictionary | Source::Emoji | Source::Kaomoji | Source::DateTime
    )
}

/// 把同码快捷短语整组插到第 `slot` 个普通候选之前（0 即首位，固定位置候选仍在它们自己的位置上）。
/// 与快捷短语同文字的其他候选会被去掉，只留快捷短语那份；列表里已有的快捷短语先整体移除，重复调用结果不变。
pub fn place_quick_phrases<T: Positioned>(candidates: &mut Vec<T>, phrases: Vec<T>, slot: usize) {
    candidates.retain(|item| item.source() != Source::QuickPhrase && !phrases.iter().any(|p| p.word() == item.word()));
    if phrases.is_empty() {
        return;
    }
    let mut fixed: Vec<T> = Vec::new();
    let mut remaining = Vec::with_capacity(candidates.len());
    for item in candidates.drain(..) {
        if item.fixed_position() > 0 { fixed.push(item) } else { remaining.push(item) }
    }
    *candidates = remaining;

    let mut index = 0;
    let mut counted = 0;
    while index < candidates.len() && counted < slot {
        if counts_toward_quick_phrase_slot(&candidates[index]) {
            counted += 1;
        }
        index += 1;
    }
    candidates.splice(index..index, phrases);

    fixed.sort_by_key(|item| item.fixed_position());
    for item in fixed {
        let position = ((item.fixed_position() - 1) as usize).min(candidates.len());
        candidates.insert(position, item);
    }
}
