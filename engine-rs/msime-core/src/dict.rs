//! 全拼词库（`msime.db`）的只读查询。

use crate::lattice::Lexeme;
use crate::table::{key, table_name};
use rusqlite::{Connection, OpenFlags};
use std::path::Path;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub text: String,
    pub weight: i64,
}

/// 查询结果带上词库里的键，混合简拼要靠键逐段核对。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KeyedEntry {
    pub key: String,
    pub text: String,
    pub weight: i64,
}

/// 词格里模糊读音的行每错一个音节扣的分（对数），和模糊变体的路径数上限。
const FUZZY_LATTICE_PENALTY_LOG: f64 = 1.0;
const FUZZY_LATTICE_VARIANT_LIMIT: usize = 16;

/// ü 的两种写法归一到词库存的那一种：j/q/x/y 后面写 u，l/n 后面写 v。
fn canonical_syllable(syllable: &str) -> &str {
    match syllable {
        "jv" => "ju",
        "qv" => "qu",
        "xv" => "xu",
        "yv" => "yu",
        "jve" => "jue",
        "qve" => "que",
        "xve" => "xue",
        "yve" => "yue",
        "lue" => "lve",
        "nue" => "nve",
        other => other,
    }
}

/// 全拼和双拼共用同一套查询，只有简拼那几级认键的方式不同。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QuerySource {
    Quanpin,
    Shuangpin,
}

pub struct Dictionary {
    connection: Connection,
}

impl Dictionary {
    /// 只读打开，不会改动词库，也不会产生 -wal 文件。
    pub fn open(path: &Path) -> rusqlite::Result<Self> {
        let connection = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_URI)?;
        Ok(Self { connection })
    }

    /// 用现成的连接（领域词库在自己的连接上建了临时视图，把各张表过滤成启用的词包）。
    pub fn from_connection(connection: Connection) -> Self {
        Self { connection }
    }

    /// 底层只读连接，给需要自己拼 SQL 的调用方。
    pub fn connection(&self) -> &Connection {
        &self.connection
    }

    fn keyed(&self, sql: &str, params: impl rusqlite::Params) -> rusqlite::Result<Vec<KeyedEntry>> {
        let mut statement = match self.connection.prepare(sql) {
            Ok(statement) => statement,
            // 没有这个首字母的表：当作没有结果，和 C++ 里 prepare 失败返回空一致。
            Err(rusqlite::Error::SqliteFailure(_, Some(message))) if message.starts_with("no such table") => {
                return Ok(Vec::new())
            }
            Err(other) => return Err(other),
        };
        let rows = statement
            .query_map(params, |row| Ok(KeyedEntry { key: row.get(0)?, text: row.get(1)?, weight: row.get(2)? }))?;
        rows.collect()
    }

    /// 这一串音节对应的词，按权重从高到低；权重相同按词库里的先后（rowid）。
    pub fn exact<S: AsRef<str>>(&self, syllables: &[S]) -> rusqlite::Result<Vec<Entry>> {
        let Some(table) = table_name(syllables) else { return Ok(Vec::new()) };
        let sql = format!(
            "select key, value, weight from {table} where key = ?1 order by weight desc, rowid"
        );
        Ok(self
            .keyed(&sql, [key(syllables)])?
            .into_iter()
            .map(|row| Entry { text: row.text, weight: row.weight })
            .collect())
    }

    /// 一种切分对应的词库查询，和 C++ `query_single_cut_keyed` 同样的顺序：
    /// 完整音节就先查精确键；再查键前缀（最后一段没打完）；再查混合简拼；纯简拼最后查。
    /// 哪一步有结果就用哪一步的，不合并。SQL 和 C++ 保持一致（不加 rowid 次序），权重相同时的先后跟着 SQLite 走。
    pub fn query(&self, segments: &[String], limit: usize) -> rusqlite::Result<Vec<KeyedEntry>> {
        self.query_with_source(segments, limit, QuerySource::Quanpin)
    }

    /// 同 [`Dictionary::query`]，`source` 决定混合简拼那一级怎么认键：双拼里 `zh/ch/sh` 单独成段时也算简拼，
    /// 并且要和词库键这一段的声母（`zh/ch/sh` 或首字母）严格相等。
    pub fn query_with_source(&self, segments: &[String], limit: usize, source: QuerySource) -> rusqlite::Result<Vec<KeyedEntry>> {
        let Some(table) = table_name(segments) else { return Ok(Vec::new()) };
        let limit = limit as i64;
        let select = format!("SELECT \"key\", \"value\", \"weight\" FROM \"{table}\"");

        if can_match_exact_key(segments) {
            let rows = self.keyed(
                &format!("{select} WHERE \"key\" = ?1 ORDER BY \"weight\" DESC LIMIT ?2"),
                rusqlite::params![key(segments), limit],
            )?;
            if !rows.is_empty() {
                return Ok(rows);
            }
        }

        let prefix = key_prefix(segments);
        let rows = self.keyed(
            &format!("{select} WHERE \"key\" >= ?1 AND \"key\" < ?2 ORDER BY \"weight\" DESC LIMIT ?3"),
            rusqlite::params![prefix, format!("{prefix}{{"), limit],
        )?;
        if !rows.is_empty() {
            return Ok(rows);
        }

        let jianpin: String = segments.iter().filter_map(|s| s.chars().next()).collect();
        let by_jianpin = format!("{select} WHERE \"jp\" = ?1 ORDER BY \"weight\" DESC LIMIT ?2");
        if needs_mixed_jianpin(segments, source) {
            let scan = if limit > i64::from(i32::MAX) / 16 { i64::from(i32::MAX) } else { (limit * 16).max(128) };
            let rows: Vec<KeyedEntry> = self
                .keyed(&by_jianpin, rusqlite::params![jianpin, scan])?
                .into_iter()
                .filter(|row| matches_mixed_segments(&row.key, segments, source))
                .take(limit as usize)
                .collect();
            if !rows.is_empty() {
                return Ok(rows);
            }
        }

        if !is_pure_jianpin(segments) {
            return Ok(Vec::new());
        }
        self.keyed(&by_jianpin, rusqlite::params![jianpin, limit])
    }
}

impl Dictionary {
    /// 单个字母（声母）下的词，按权重从高到低，对应 C++ `quanpin::query_initial`：只查 `tbl_1_<字母>` 里键以 `prefix` 开头的行。
    pub fn query_initial(&self, prefix: &str, limit: i64) -> rusqlite::Result<Vec<KeyedEntry>> {
        let Some(first) = prefix.chars().next() else { return Ok(Vec::new()) };
        if !first.is_ascii_lowercase() || limit <= 0 {
            return Ok(Vec::new());
        }
        let sql = format!("SELECT \"key\", \"value\", \"weight\" FROM \"tbl_1_{first}\" WHERE \"key\" >= ?1 AND \"key\" < ?2 ORDER BY \"weight\" DESC LIMIT ?3");
        self.keyed(&sql, rusqlite::params![prefix, format!("{prefix}{{"), limit])
    }

    /// 对应 C++ `query_segments_keyed_flat`：查一种切分，同词只留第一条，按权重从高到低排（`std::sort`，平局次序跟 MSVC 一致），
    /// 截到 `limit`。
    pub fn query_keyed_flat(&self, segments: &[String], limit: usize, source: QuerySource) -> rusqlite::Result<Vec<KeyedEntry>> {
        if segments.is_empty() {
            return Ok(Vec::new());
        }
        let mut rows = dedupe_by_text(self.query_with_source(segments, limit, source)?);
        crate::msvc_sort::sort_by(&mut rows, |a, b| a.weight > b.weight);
        rows.truncate(limit);
        Ok(rows)
    }

    /// 一批切分各自的精确键，按表分组一次查完，合起来按权重从高到低（稳定）排。
    /// 对应 C++ `query_exact_segmentations_keyed_flat`：含没打完音节的路径跳过，重复的键只查一次，
    /// 表按名字排序依次查，每张表最多取 `limit` 行。
    pub fn query_exact_paths(&self, paths: &[Vec<String>], limit: usize) -> rusqlite::Result<Vec<KeyedEntry>> {
        use std::collections::{BTreeMap, HashSet};
        let mut keys_by_table: BTreeMap<String, Vec<String>> = BTreeMap::new();
        let mut seen = HashSet::new();
        for path in paths {
            if !can_match_exact_key(path) {
                continue;
            }
            let (Some(table), joined) = (table_name(path), key(path)) else { continue };
            if joined.is_empty() || !seen.insert(format!("{table}\n{joined}")) {
                continue;
            }
            keys_by_table.entry(table).or_default().push(joined);
        }
        let mut result = Vec::new();
        for (table, keys) in keys_by_table {
            let placeholders = vec!["?"; keys.len()].join(",");
            let sql = format!(
                "SELECT \"key\", \"value\", \"weight\" FROM \"{table}\" WHERE \"key\" IN ({placeholders}) ORDER BY \"weight\" DESC LIMIT ?"
            );
            let params = keys.iter().map(|k| rusqlite::types::Value::Text(k.clone())).chain(std::iter::once(
                rusqlite::types::Value::Integer(limit as i64),
            ));
            result.extend(self.keyed(&sql, rusqlite::params_from_iter(params))?);
        }
        result.sort_by(|a, b| b.weight.cmp(&a.weight)); // 稳定排序
        Ok(result)
    }

    /// 整句词格的一次词库查询，对应 C++ `quanpin::make_lattice_db_lookup` 返回的函数。
    ///
    /// 只收键与跨度完全相等的行（不降级到前缀和简拼，否则 `gun'qi` 会捞出 `gun'qiu` 的词，造句读音对不上）；
    /// ü 的另一种写法先归一到词库存的那种；开了模糊音时近音读音同批查，每个与敲出读音不同的音节扣一份分，
    /// 再按折后权重排序、同字去重、截回 `span_limit`。
    pub fn lattice_lookup(&self, span: &[String], span_limit: i32, fuzzy_rules: u32) -> rusqlite::Result<Vec<Lexeme>> {
        let normalized: Vec<String> = span.iter().map(|s| canonical_syllable(s).to_string()).collect();
        let mut keys = vec![normalized.clone()];
        for mut variant in crate::fuzzy::fuzzy_segmentations(&normalized, fuzzy_rules, FUZZY_LATTICE_VARIANT_LIMIT) {
            variant.iter_mut().for_each(|s| *s = canonical_syllable(s).to_string());
            keys.push(variant);
        }
        // 模糊变体与敲出的读音同批查（同表一条 SQL）。LIMIT 按表计，放宽到每个键一份，再截回 span_limit。
        let limit = span_limit.saturating_mul(keys.len() as i32);
        let rows = if limit <= 0 { Vec::new() } else { self.query_exact_paths(&keys, limit as usize)? };
        let mut lexemes: Vec<Lexeme> = rows
            .into_iter()
            .map(|row| {
                let mut lexeme = Lexeme { key: row.key.clone(), value: row.text, weight: row.weight, ..Lexeme::default() };
                if keys.len() > 1 {
                    // 每个与敲出读音不同的音节扣一份分：一个词里错两个音，比只错一个更不可能。
                    for (typed, found) in normalized.iter().zip(row.key.split('\'')) {
                        if found != typed {
                            lexeme.penalty += FUZZY_LATTICE_PENALTY_LOG;
                        }
                    }
                }
                lexeme
            })
            .collect();
        if keys.len() > 1 {
            let adjusted = |l: &Lexeme| l.weight as f64 * 10f64.powf(-l.penalty);
            lexemes.sort_by(|a, b| adjusted(b).partial_cmp(&adjusted(a)).unwrap_or(std::cmp::Ordering::Equal)); // 稳定排序
            // 同一个字两种读音都收（多音字），只留排在前面的那行。
            let mut seen = std::collections::HashSet::new();
            lexemes.retain(|l| seen.insert(l.value.clone()));
            lexemes.truncate(span_limit.max(0) as usize);
        }
        Ok(lexemes)
    }

    /// 精确候选：整串先查，再依次查越来越短的前缀（候选覆盖的输入越长越靠前），同一个词只留最先出现的。
    /// 每个前缀走 `query` 的那套顺序。整句词格、联想、用户词库调频不在这里。
    pub fn exact_candidates(&self, typed: &[String], limit: usize) -> rusqlite::Result<Vec<crate::fuzzy::Candidate>> {
        let mut seen = std::collections::HashSet::new();
        let mut result = Vec::new();
        for count in (1..=typed.len()).rev() {
            let prefix = &typed[..count];
            for row in self.query(prefix, limit)? {
                if seen.insert(row.text.clone()) {
                    result.push(crate::fuzzy::Candidate {
                        text: row.text,
                        pinyin: key(prefix),
                        weight: row.weight,
                        fuzzy: false,
                        from_dictionary: true,
                    });
                }
            }
        }
        Ok(result)
    }

    /// 模糊候选：从整串往短试，每个长度取近音路径的词，路径预算 128 条（每次最多 64 条）。
    /// 对应 C++ `QuanpinDictionary::fuzzy_candidates`；领域词库那一路还没移植。
    pub fn fuzzy_candidates(&self, typed: &[String], rules: u32) -> rusqlite::Result<Vec<crate::fuzzy::Candidate>> {
        let mut result = Vec::new();
        if rules == 0 {
            return Ok(result);
        }
        let mut budget = 128usize;
        for count in (1..=typed.len()).rev() {
            if budget <= 1 {
                break;
            }
            let prefix = &typed[..count];
            let paths = crate::fuzzy::fuzzy_segmentations(prefix, rules, budget.min(64));
            if paths.is_empty() {
                continue;
            }
            budget -= paths.len();
            for row in self.query_exact_paths(&paths, 128)? {
                result.push(crate::fuzzy::Candidate {
                    text: row.text,
                    pinyin: key(prefix),
                    weight: row.weight,
                    fuzzy: true,
                    from_dictionary: true,
                });
            }
        }
        Ok(result)
    }
}

/// 同一个词出现在多个键下（`shei` 和 `shui` 都有「谁」）时只留第一个，也就是权重最高的那条。
/// C++ 在合并各种切分的结果之后才做这一步，所以它不在 `query` 里，由调用方决定什么时候去重。
pub fn dedupe_by_text(rows: Vec<KeyedEntry>) -> Vec<KeyedEntry> {
    let mut seen = std::collections::HashSet::new();
    rows.into_iter().filter(|row| seen.insert(row.text.clone())).collect()
}

fn is_pure_jianpin(segments: &[String]) -> bool {
    segments.iter().all(|s| s.len() == 1)
}

fn can_match_exact_key(segments: &[String]) -> bool {
    !segments.is_empty() && segments.iter().all(|s| crate::syllables::is_intact(s))
}

/// 前面的段里有单字母，才需要走混合简拼（最后一段本来就按前缀匹配）。
fn needs_mixed_jianpin(segments: &[String], source: QuerySource) -> bool {
    segments.len() > 1
        && segments[..segments.len() - 1].iter().any(|s| s.len() == 1 || (source == QuerySource::Shuangpin && is_shuangpin_initial_token(s)))
}

fn is_shuangpin_initial_token(segment: &str) -> bool {
    matches!(segment, "zh" | "ch" | "sh")
}

/// 词库键这一段的声母：`zh/ch/sh` 或第一个字母。
fn extract_initial_token(segment: &str) -> &str {
    if segment.len() >= 2 && is_shuangpin_initial_token(&segment[..2]) {
        return &segment[..2];
    }
    segment.get(..1).unwrap_or("")
}

/// 词库键的范围查询下界：除最后一段外，单字母段也带 `%`，最后一段总是带；去掉末尾的 `%`。
/// 注意 `%` 在这里只是普通字符（用的是区间比较，不是 LIKE），带 `%` 的下界基本查不到东西，
/// 查不到就落到后面的混合简拼，这是 C++ 现在的实际行为，照搬。
fn key_prefix(segments: &[String]) -> String {
    let last = segments.len() - 1;
    let parts: Vec<String> = segments
        .iter()
        .enumerate()
        .map(|(i, s)| if i == last || s.len() == 1 { format!("{s}%") } else { s.clone() })
        .collect();
    let mut pattern = parts.join("'");
    pattern.pop();
    pattern
}

/// 键的每一段要么和输入完全相同，要么（输入是单字母时）以这个字母开头。
fn matches_mixed_segments(key: &str, segments: &[String], source: QuerySource) -> bool {
    let parts: Vec<&str> = key.split('\'').collect();
    parts.len() == segments.len()
        && segments.iter().zip(&parts).all(|(expected, actual)| {
            if expected.is_empty() || actual.is_empty() {
                return false;
            }
            let strict_initial = source == QuerySource::Shuangpin && is_shuangpin_initial_token(expected);
            if expected.len() == 1 || strict_initial {
                return if source == QuerySource::Shuangpin {
                    extract_initial_token(actual) == expected
                } else {
                    actual.as_bytes()[0] == expected.as_bytes()[0]
                };
            }
            actual == expected
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seg(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn prefix_pattern_follows_the_cpp_rule() {
        assert_eq!(key_prefix(&seg(&["zhong", "g"])), "zhong'g");
        assert_eq!(key_prefix(&seg(&["ni", "h"])), "ni'h");
        assert_eq!(key_prefix(&seg(&["n", "h"])), "n%'h");
        assert_eq!(key_prefix(&seg(&["sh"])), "sh");
    }

    #[test]
    fn dedupe_keeps_the_first_row_of_each_word() {
        let row = |key: &str, text: &str, weight| KeyedEntry { key: key.into(), text: text.into(), weight };
        let rows = vec![row("shui", "谁", 9), row("shei", "谁", 3), row("shu", "书", 2)];
        let kept = dedupe_by_text(rows);
        assert_eq!(kept.iter().map(|r| (r.key.as_str(), r.text.as_str())).collect::<Vec<_>>(), [("shui", "谁"), ("shu", "书")]);
    }

    #[test]
    fn mixed_matching_checks_each_segment() {
        assert!(matches_mixed_segments("ni'hao", &seg(&["ni", "h"]), QuerySource::Quanpin));
        assert!(matches_mixed_segments("nin'hao", &seg(&["n", "h"]), QuerySource::Quanpin));
        assert!(!matches_mixed_segments("ni'hao", &seg(&["ni", "z"]), QuerySource::Quanpin));
        assert!(!matches_mixed_segments("ni'hao'a", &seg(&["ni", "h"]), QuerySource::Quanpin));
    }

    #[test]
    fn only_a_leading_single_letter_needs_the_mixed_query() {
        assert!(needs_mixed_jianpin(&seg(&["n", "h"]), QuerySource::Quanpin));
        assert!(needs_mixed_jianpin(&seg(&["n", "hao"]), QuerySource::Quanpin));
        assert!(!needs_mixed_jianpin(&seg(&["ni", "h"]), QuerySource::Quanpin));
        assert!(!needs_mixed_jianpin(&seg(&["n"]), QuerySource::Quanpin));
    }
}
