//! 英文词典：按前缀取词、中英释义（对应 `engine/english/english_dictionary.cpp` 的读取部分）。
//!
//! 释义有四层：用户的自定义翻译侧车文件（`custom_translations.txt`）最优先，其次是 `qingjian-glossary.db`、
//! `industry-learning.db` 里的行业词包，最后是 `english.db` 里的旧表；后三层的结果用 `merge_glosses` 合并去重。
//! 建表、升级旧库和写入释义（`ensure_schema`、`upsert_gloss`）要写库，还在 C++。

use crate::lattice::Source;
use crate::local::Item;
use rusqlite::{Connection, OpenFlags};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

pub struct EnglishDictionary {
    connection: Option<Connection>,
    bundled: Option<Connection>,
    packs: Option<Connection>,
    custom_en_zh: HashMap<String, String>,
    custom_zh_en: HashMap<String, String>,
    disabled: String,
}

fn open_read_only(path: &Path) -> Option<Connection> {
    Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_URI).ok()
}

/// C 的 `isspace`（C 区域）。
fn is_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

fn trim_ascii_space(text: &str) -> &str {
    text.trim_matches(|c: char| c.is_ascii() && is_space(c as u8))
}

/// 保留清简的义项顺序，再补上旧表里不重复的义项；用户的自定义翻译不走这里，它说了算。
pub fn merge_glosses(primary: &str, fallback: &str) -> String {
    if primary.is_empty() {
        return fallback.to_string();
    }
    const POS: [&str; 12] = ["n. ", "v. ", "adj. ", "adv. ", "prep. ", "pron. ", "conj. ", "int. ", "num. ", "aux. ", "phr. ", "m. "];
    let mut result = String::new();
    let mut seen = HashSet::new();
    for source in [primary, fallback] {
        for sense in source.split(';') {
            let sense = trim_ascii_space(sense);
            if sense.is_empty() {
                continue;
            }
            let mut key = sense;
            if let Some(prefix) = POS.iter().find(|p| key.starts_with(**p)) {
                key = &key[prefix.len()..];
            }
            if seen.insert(key.to_ascii_lowercase()) {
                if !result.is_empty() {
                    result.push_str("; ");
                }
                result.push_str(sense);
            }
        }
    }
    result
}

fn query_gloss(connection: &Connection, sql: &str, key: &str) -> String {
    connection
        .prepare(sql)
        .and_then(|mut statement| statement.query_row([key], |row| row.get::<_, Option<String>>(0)))
        .ok()
        .flatten()
        .unwrap_or_default()
}

impl EnglishDictionary {
    /// `db_path` 是 `english.db`；`translations_path` 为空时读它同目录的 `custom_translations.txt`。
    /// 清简词库和行业词包按 `english.db` 同目录的固定文件名找。
    pub fn open(db_path: &Path, translations_path: Option<&Path>) -> Self {
        let directory = db_path.parent().map(Path::to_path_buf).unwrap_or_else(PathBuf::new);
        let sidecar = translations_path.map(Path::to_path_buf).unwrap_or_else(|| directory.join("custom_translations.txt"));
        let (custom_en_zh, custom_zh_en) = load_custom_translations(&sidecar);
        // 词库缺了就当没有：对应的那一层不出结果，和 C++ 里 open 失败一致。
        let bundled = open_read_only(&directory.join("qingjian-glossary.db"))
            .filter(|c| c.prepare("SELECT gloss FROM glosses WHERE word=?1 AND language=?2").is_ok());
        let packs = open_read_only(&directory.join("industry-learning.db"))
            .filter(|c| c.prepare("SELECT pack,gloss FROM glosses WHERE language=?1 AND word=?2 ORDER BY pack").is_ok());
        Self { connection: open_read_only(db_path), bundled, packs, custom_en_zh, custom_zh_en, disabled: String::new() }
    }

    pub fn set_disabled_dictionaries(&mut self, ids: &str) {
        self.disabled = format!(",{ids},");
    }

    fn dictionary_enabled(&self, id: &str) -> bool {
        !self.disabled.contains(&format!(",{id},"))
    }

    /// 英文前缀补全：`prefix` 必须是小写 ASCII 字母；精确等于前缀的词在前，再按权重、长度、字典序。
    pub fn query_prefix(&self, prefix: &str, limit: usize) -> Vec<Item> {
        if prefix.is_empty() || !prefix.bytes().all(|b| b.is_ascii_lowercase()) || limit == 0 {
            return Vec::new();
        }
        let Some(connection) = &self.connection else { return Vec::new() };
        let sql = "SELECT word,display,weight FROM english_words WHERE word >= ?1 AND word < ?2 \
                   ORDER BY CASE WHEN word = ?1 THEN 0 ELSE 1 END, weight DESC, length(word), word, display LIMIT ?3";
        let Ok(mut statement) = connection.prepare(sql) else { return Vec::new() };
        let upper = format!("{prefix}{{");
        let limit = i64::from(i32::try_from(limit).unwrap_or(i32::MAX));
        let Ok(rows) = statement.query_map(rusqlite::params![prefix, upper, limit], |row| {
            Ok((row.get::<_, Option<String>>(0)?, row.get::<_, Option<String>>(1)?, row.get::<_, i64>(2)?))
        }) else {
            return Vec::new();
        };
        let mut items = Vec::new();
        for row in rows {
            let Ok((word, display, weight)) = row else { return Vec::new() };
            if let (Some(word), Some(display)) = (word, display) {
                items.push(Item::new(&word, &display, weight, Source::EnglishDictionary));
            }
        }
        items
    }

    fn legacy_gloss(&self, sql: &str, key: &str) -> String {
        if !self.dictionary_enabled("legacy-en") || key.is_empty() {
            return String::new();
        }
        // 两张释义表要同时存在：C++ 一次准备两条语句，缺一张就都不用。
        let Some(connection) = &self.connection else { return String::new() };
        if connection.prepare("SELECT chinese_gloss FROM en_zh_glosses WHERE english=?1").is_err()
            || connection.prepare("SELECT english_gloss FROM zh_en_glosses WHERE chinese=?1").is_err()
        {
            return String::new();
        }
        query_gloss(connection, sql, key)
    }

    pub fn query_chinese_gloss(&self, english: &str) -> String {
        if let Some(custom) = self.custom_en_zh.get(english) {
            return custom.clone();
        }
        let gloss = self.query_translation_gloss(english, "zh");
        if !gloss.is_empty() {
            return gloss;
        }
        self.legacy_gloss("SELECT chinese_gloss FROM en_zh_glosses WHERE english=?1", english)
    }

    pub fn query_english_gloss(&self, chinese: &str) -> String {
        if let Some(custom) = self.custom_zh_en.get(chinese) {
            return custom.clone();
        }
        let bundled = self.query_translation_gloss(chinese, "en");
        let fallback = self.legacy_gloss("SELECT english_gloss FROM zh_en_glosses WHERE chinese=?1", chinese);
        merge_glosses(&bundled, &fallback)
    }

    fn query_qingjian_gloss(&self, word: &str, language: &str) -> String {
        if !self.dictionary_enabled(&format!("qingjian-{language}")) {
            return String::new();
        }
        if word.is_empty() || !matches!(language, "en" | "zh" | "ja" | "es" | "vi") {
            return String::new();
        }
        let Some(bundled) = &self.bundled else { return String::new() };
        bundled
            .prepare("SELECT gloss FROM glosses WHERE word=?1 AND language=?2")
            .and_then(|mut s| s.query_row([word, language], |row| row.get::<_, Option<String>>(0)))
            .ok()
            .flatten()
            .unwrap_or_default()
    }

    /// 清简词库和各行业词包里 `word` 的释义（`language` 是释义的语言），合并去重。
    pub fn query_translation_gloss(&self, word: &str, language: &str) -> String {
        let mut result = self.query_qingjian_gloss(word, language);
        if word.is_empty() {
            return result;
        }
        let Some(packs) = &self.packs else { return result };
        let Ok(mut statement) = packs.prepare("SELECT pack,gloss FROM glosses WHERE language=?1 AND word=?2 ORDER BY pack") else { return result };
        let Ok(rows) = statement.query_map([language, word], |row| Ok((row.get::<_, Option<String>>(0)?, row.get::<_, Option<String>>(1)?))) else {
            return result;
        };
        for row in rows.flatten() {
            if let (Some(pack), Some(gloss)) = row {
                if self.dictionary_enabled(&pack) {
                    result = merge_glosses(&result, &gloss);
                }
            }
        }
        result
    }
}

/// 读自定义翻译侧车：每行 `源<Tab>释义`，`#` 开头是注释；源里有非 ASCII 字符算中译英，否则英译中。
fn load_custom_translations(path: &Path) -> (HashMap<String, String>, HashMap<String, String>) {
    let (mut en_zh, mut zh_en) = (HashMap::new(), HashMap::new());
    let Ok(bytes) = std::fs::read(path) else { return (en_zh, zh_en) };
    let bytes = bytes.strip_prefix(&[0xEF, 0xBB, 0xBF]).unwrap_or(&bytes);
    let text = String::from_utf8_lossy(bytes);
    let mut rest: &str = &text;
    while !rest.is_empty() {
        let end = rest.find(['\r', '\n']).unwrap_or(rest.len());
        let line = &rest[..end];
        let skip = if rest[end..].starts_with("\r\n") { 2 } else { usize::from(end < rest.len()) };
        rest = &rest[end + skip..];

        let line = trim_ascii_space(line);
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some(tab) = line.find('\t') else { continue };
        if tab == 0 || tab + 1 >= line.len() {
            continue;
        }
        let source = line[..tab].trim_end_matches(|c: char| c.is_ascii() && is_space(c as u8));
        let gloss = trim_ascii_space(&line[tab + 1..]);
        if source.is_empty() || gloss.is_empty() {
            continue;
        }
        let target = if source.bytes().any(|b| b >= 0x80) { &mut zh_en } else { &mut en_zh };
        target.insert(source.to_string(), gloss.to_string());
    }
    (en_zh, zh_en)
}

// ---------------------------------------------------------------------------------------------
// 写库：建表/升级旧表、写释义（移植自 `EnglishDictionary::ensure_schema` / `upsert_gloss`）
// ---------------------------------------------------------------------------------------------

/// 保证 `english.db` 有当前结构：`english_words` 是（词，显示）复合主键、带权重的 WITHOUT ROWID 表（旧库缺权重或
/// 主键不对就迁移），加上两张释义表，`user_version` 置 3。库不存在时创建。
pub fn ensure_schema(db_path: &str) -> bool {
    let Ok(database) = Connection::open_with_flags(db_path, OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE) else {
        return false;
    };
    let (mut has_weight, mut has_table, mut primary_key_columns) = (false, false, 0);
    if let Ok(mut columns) = database.prepare("PRAGMA table_info(english_words)") {
        if let Ok(rows) = columns.query_map([], |row| Ok((row.get::<_, Option<String>>(1)?, row.get::<_, i64>(5)?))) {
            for (name, primary_key) in rows.flatten() {
                has_table = true;
                has_weight = has_weight || name.as_deref() == Some("weight");
                if primary_key > 0 {
                    primary_key_columns += 1;
                }
            }
        }
    }
    let composite_primary_key = primary_key_columns == 2;
    let create = "(word TEXT COLLATE BINARY NOT NULL,display TEXT NOT NULL,weight INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(word,display)) WITHOUT ROWID";
    let english_words_ok = if !has_table {
        database.execute_batch(&format!("CREATE TABLE english_words{create};")).is_ok()
    } else if has_weight && composite_primary_key {
        true
    } else {
        let copy_sql = if has_weight {
            "INSERT OR IGNORE INTO english_words_new(word,display,weight) SELECT word,display,weight FROM english_words;"
        } else {
            "INSERT OR IGNORE INTO english_words_new(word,display,weight) SELECT word,display,0 FROM english_words;"
        };
        let migrated = database.execute_batch("BEGIN IMMEDIATE").is_ok()
            && database.execute_batch(&format!("CREATE TABLE english_words_new{create}")).is_ok()
            && database.execute_batch(copy_sql).is_ok()
            && database.execute_batch("DROP TABLE english_words").is_ok()
            && database.execute_batch("ALTER TABLE english_words_new RENAME TO english_words").is_ok();
        let _ = database.execute_batch(if migrated { "COMMIT" } else { "ROLLBACK" });
        migrated
    };
    english_words_ok
        && database
            .execute_batch(
                "CREATE TABLE IF NOT EXISTS en_zh_glosses(english TEXT COLLATE BINARY PRIMARY KEY,chinese_gloss TEXT NOT NULL) WITHOUT ROWID;\
                 CREATE TABLE IF NOT EXISTS zh_en_glosses(chinese TEXT COLLATE BINARY PRIMARY KEY,english_gloss TEXT NOT NULL) WITHOUT ROWID;\
                 PRAGMA user_version=3;",
            )
            .is_ok()
}

/// 写一条释义（中译英或英译中），已有的覆盖。
pub fn upsert_gloss(db_path: &str, chinese_to_english: bool, key: &str, gloss: &str) -> bool {
    if db_path.is_empty() || key.is_empty() || gloss.is_empty() || !ensure_schema(db_path) {
        return false;
    }
    let Ok(database) = Connection::open_with_flags(db_path, OpenFlags::SQLITE_OPEN_READ_WRITE) else { return false };
    let _ = database.busy_timeout(std::time::Duration::from_millis(250));
    let sql = if chinese_to_english {
        "INSERT OR REPLACE INTO zh_en_glosses(chinese,english_gloss) VALUES(?1,?2)"
    } else {
        "INSERT OR REPLACE INTO en_zh_glosses(english,chinese_gloss) VALUES(?1,?2)"
    };
    database.execute(sql, rusqlite::params![key, gloss]).is_ok()
}
