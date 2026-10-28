//! 领域词库：随包的 `domain-input.db`（只读）里按词包分的专业词条，移植自 `engine/domain_dictionary/domain_dictionary.cpp`。
//!
//! 每张 `tbl_*` 表多一列 `pack`。用户启用哪些词包，就在自己的连接上给每张表建一个同名的 TEMP 视图，只露出
//! 启用词包的行——这样全拼词库那套精确/前缀/混合简拼的查询原样可用，主库只读、从不改动。建视图失败时整个领域库
//! 关掉（宁可没有，也不露出没过滤的主表）。
//!
//! 没搬的：`membership` 的结果缓存（只影响速度）。

use crate::dict::{Dictionary, QuerySource};
use crate::lattice::{Item, Lexeme, Source};
use rusqlite::{Connection, OpenFlags};
use std::path::{Path, PathBuf};

/// 词包的公开 ID：配置和导出用，和源文件名无关。
pub fn ids() -> &'static [&'static str] {
    &["animals", "automotive", "finance", "food", "historical", "idioms", "it", "law", "medicine", "places", "poetry"]
}

/// 去掉不认识的名字和重复，按公开 ID 的固定次序用逗号连起来。
pub fn normalize_enabled(csv: &str) -> String {
    let selected: Vec<&str> = csv.split(',').collect();
    ids().iter().filter(|id| selected.contains(id)).copied().collect::<Vec<_>>().join(",")
}

pub struct DomainDictionary {
    path: PathBuf,
    dictionary: Option<Dictionary>,
    selected: String,
}

impl DomainDictionary {
    pub fn new(path: &Path) -> Self {
        Self { path: path.to_path_buf(), dictionary: None, selected: String::new() }
    }

    pub fn enabled(&self) -> &str {
        &self.selected
    }

    /// 换启用的词包。没有变化返回假；变了返回真（即使领域库打不开，调用方也要清缓存）。
    pub fn set_enabled(&mut self, csv: &str) -> bool {
        let normalized = normalize_enabled(csv);
        if normalized == self.selected {
            return false;
        }
        self.selected = normalized.clone();
        if self.dictionary.is_none() && !normalized.is_empty() {
            // 绝不在用户目录里创建或迁移这份不可变的资源。
            self.dictionary = Connection::open_with_flags(&self.path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX | OpenFlags::SQLITE_OPEN_URI)
                .ok()
                .map(Dictionary::from_connection);
        }
        let Some(dictionary) = &self.dictionary else { return true };
        let filter = std::iter::once("'__none__'".to_string()).chain(normalized.split(',').filter(|id| !id.is_empty()).map(|id| format!("'{id}'"))).collect::<Vec<_>>().join(",");
        let tables: Vec<String> = {
            let connection = dictionary.connection();
            let Ok(mut statement) = connection.prepare("SELECT name FROM main.sqlite_master WHERE type='table' AND name LIKE 'tbl_%'") else {
                return true;
            };
            let Ok(rows) = statement.query_map([], |row| row.get::<_, String>(0)) else { return true };
            rows.filter_map(Result::ok).collect()
        };
        for table in tables {
            if !table.chars().all(|c| matches!(c, 'a'..='z' | '0'..='9' | '_')) {
                continue;
            }
            let sql = format!(
                "DROP VIEW IF EXISTS temp.\"{table}\"; CREATE TEMP VIEW \"{table}\" AS SELECT key,jp,value,weight FROM main.\"{table}\" WHERE pack IN ({filter})"
            );
            if dictionary.connection().execute_batch(&sql).is_err() {
                self.dictionary = None; // 失败就整个关掉：不露出没过滤的主表。
                break;
            }
        }
        true
    }

    /// 词条所属的、且当前启用的词包（按库里的行序）。
    fn membership(&self, dictionary: &Dictionary, key: &str, word: &str) -> Vec<String> {
        let Ok(mut statement) = dictionary.connection().prepare("SELECT pack FROM entries WHERE key=?1 AND value=?2") else { return Vec::new() };
        let Ok(rows) = statement.query_map(rusqlite::params![key, word], |row| row.get::<_, String>(0)) else { return Vec::new() };
        // 认不得的词包名经 normalize 会被丢掉，所以照搬原判据：并进已启用集合后不变就算。
        rows.filter_map(Result::ok).filter(|pack| normalize_enabled(&format!("{},{pack}", self.selected)) == self.selected).collect()
    }

    fn convert(&self, dictionary: &Dictionary, rows: &[crate::dict::KeyedEntry], code: &str) -> Vec<Item> {
        rows.iter()
            .map(|row| Item {
                pinyin: if code.is_empty() { row.key.clone() } else { code.to_string() },
                canonical_pinyin: row.key.clone(),
                word: row.text.clone(),
                weight: row.weight,
                source: Source::Database,
                sentence_association: false,
                input_domains: self.membership(dictionary, &row.key, &row.text),
                fuzzy: false,
                corrected_from: String::new(),
            })
            .collect()
    }

    fn active(&self) -> Option<&Dictionary> {
        if self.selected.is_empty() {
            return None;
        }
        self.dictionary.as_ref()
    }

    pub fn query(&self, segments: &[String], code: &str, limit: usize, source: QuerySource) -> Vec<Item> {
        let Some(dictionary) = self.active() else { return Vec::new() };
        if segments.is_empty() {
            return Vec::new();
        }
        let rows = if segments.len() == 1 && segments[0].len() == 1 {
            dictionary.query_initial(&segments[0], limit as i64)
        } else {
            dictionary.query_keyed_flat(segments, limit, source)
        };
        self.convert(dictionary, &rows.unwrap_or_default(), code)
    }

    pub fn exact(&self, cuts: &[Vec<String>], limit: usize) -> Vec<Item> {
        let Some(dictionary) = self.active() else { return Vec::new() };
        let rows = dictionary.query_exact_paths(cuts, limit).unwrap_or_default();
        self.convert(dictionary, &rows, "")
    }

    /// 词格的跨度查询：词库行加上近音变体，再给每行标上所属的词包。
    pub fn lattice(&self, span: &[String], limit: i32, fuzzy_rules: u32) -> Vec<Lexeme> {
        let Some(dictionary) = self.active() else { return Vec::new() };
        let mut rows = dictionary.lattice_lookup(span, limit, fuzzy_rules).unwrap_or_default();
        for row in &mut rows {
            row.input_domains = self.membership(dictionary, &row.key, &row.value);
        }
        rows
    }
}

/// 把额外候选并进 `base`，不改 `base` 里已有的读音、权重和用户覆盖：同词同读音的只补输入域。
pub fn merge(base: &mut Vec<Item>, extra: Vec<Item>) {
    for item in extra {
        match base.iter_mut().find(|row| row.word == item.word && row.canonical_pinyin == item.canonical_pinyin) {
            None => base.push(item),
            Some(found) => {
                for id in &item.input_domains {
                    if !found.input_domains.contains(id) {
                        found.input_domains.push(id.clone());
                    }
                }
            }
        }
    }
}
