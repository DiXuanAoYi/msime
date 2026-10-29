//! 用户词库的日志与调频，移植自 `engine/user_dictionary/{user_dictionary_journal,personal_dictionary,dictionary_state}.cpp`。
//!
//! 用户词库是一份 SQLite 日志（`msime_user.db`）：每次调频、造词、删词都记一行，词库升级时 [`replay`] 把它重放到新的
//! `msime.db` / `english.db` 上。日志里还有选择计数、固定位置、快捷短语和英文候选的槽位、日期时间格式的顺序。
//!
//! 和 C++ 的差别：C++ 对默认日志路径缓存每线程一个连接（性能手段，且 `close_default_user_database` 要配合关闭），
//! 这里每次操作开一个连接，等价于 C++ 对非默认路径的行为。日志连接的 `updated_at` 仍由 SQLite 的 `unixepoch()` 写。
//! 事务边界、两个库之间的提交先后、失败时的回滚都照 C++ 的次序。

use crate::english;
use crate::lattice::Source;
use rusqlite::{params, Connection, OpenFlags};
use std::path::{Path, PathBuf};

pub const MAIN_DICTIONARY: &str = "msime.db";
pub const ENGLISH_DICTIONARY: &str = "english.db";
pub const USER_JOURNAL: &str = "msime_user.db";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DictionaryKind {
    Pinyin,
    Wubi,
    QuickPhrase,
    English,
}

fn kind_name(kind: DictionaryKind) -> &'static str {
    match kind {
        DictionaryKind::Pinyin => "pinyin",
        DictionaryKind::Wubi => "wubi",
        DictionaryKind::QuickPhrase => "quick",
        DictionaryKind::English => "english",
    }
}

/// 调频用的候选（只带这里读写的字段）。
#[derive(Debug, Clone, PartialEq)]
pub struct WordItem {
    pub pinyin: String,
    pub canonical_pinyin: String,
    pub word: String,
    pub weight: i64,
    pub source: Source,
    pub fixed_position: i32,
}

impl Default for WordItem {
    fn default() -> Self {
        Self { pinyin: String::new(), canonical_pinyin: String::new(), word: String::new(), weight: 0, source: Source::Database, fixed_position: 0 }
    }
}

const MANAGED_WEIGHT_CEILING: i64 = 100_000_000;
const MANAGED_WEIGHT_FLOOR: i64 = 1;
const REBALANCE_GAP: i64 = 1000;
const REBALANCE_COUNT: usize = 16;

// ---------------------------------------------------------------------------------------------
// 连接与建表
// ---------------------------------------------------------------------------------------------

fn open_database(path: &str, flags: OpenFlags) -> Option<Connection> {
    let connection = Connection::open_with_flags(path, flags | OpenFlags::SQLITE_OPEN_FULL_MUTEX).ok()?;
    connection.busy_timeout(std::time::Duration::from_millis(5000)).ok()?;
    Some(connection)
}

fn read_write() -> OpenFlags {
    OpenFlags::SQLITE_OPEN_READ_WRITE
}

fn exec(db: &Connection, sql: &str) -> bool {
    db.execute_batch(sql).is_ok()
}

/// 学习写入跑在词库写线程上，按键线程同时读同一批文件；回滚日志模式下每次提交都要独占锁，WAL 让读不必等提交。
fn use_write_ahead_log(db: &Connection) -> bool {
    db.query_row("PRAGMA journal_mode=WAL", [], |row| row.get::<_, String>(0)).map(|mode| mode == "wal").unwrap_or(false)
}

pub fn enable_write_ahead_log(db_path: &str) -> bool {
    open_database(db_path, read_write()).is_some_and(|db| use_write_ahead_log(&db))
}

/// 旧版日文模式的调频把罗马音当拼音键写进了拼音日志（#869），回放会把这些日文词插进拼音表。拼音的自动学习不会产生
/// 带假名的词，带假名的非手动条目一律视为这类残留。SQLite 的 GLOB 字符类按码位比较 UTF-8：平假名、片假名、
/// 片假名音标扩展、半角片假名。
const LEARNED_KANA_PINYIN_CONDITION: &str = "dictionary='pinyin' AND operation='upsert' AND user_inserted=0 AND value GLOB '*[ぁ-ゖァ-ヺㇰ-ㇿｦ-ﾝ]*'";

fn has_column(db: &Connection, table: &str, column: &str) -> bool {
    let Ok(mut columns) = db.prepare(&format!("PRAGMA table_info(\"{table}\")")) else { return false };
    let Ok(names) = columns.query_map([], |row| row.get::<_, String>(1)) else { return false };
    let found = names.filter_map(Result::ok).any(|name| name == column);
    found
}

fn ensure_schema(db: &Connection) -> bool {
    let sql = "CREATE TABLE IF NOT EXISTS user_dictionary_operations(\
        dictionary TEXT NOT NULL,\
        key TEXT NOT NULL,\
        value TEXT NOT NULL,\
        operation TEXT NOT NULL CHECK(operation IN ('upsert','delete')),\
        weight INTEGER NOT NULL DEFAULT 0,\
        display TEXT NOT NULL DEFAULT '',\
        user_inserted INTEGER NOT NULL DEFAULT 0,\
        weight_edited INTEGER NOT NULL DEFAULT 0,\
        updated_at INTEGER NOT NULL DEFAULT(unixepoch()),\
        PRIMARY KEY(dictionary,key,value));\
        CREATE TABLE IF NOT EXISTS personal_dictionary_receipts( request_id TEXT PRIMARY KEY, payload TEXT NOT NULL);\
        CREATE TABLE IF NOT EXISTS candidate_selection_state(\
        context_key TEXT NOT NULL,entry_key TEXT NOT NULL,value TEXT NOT NULL,\
        selection_count INTEGER NOT NULL DEFAULT 0,\
        PRIMARY KEY(context_key,entry_key,value));\
        CREATE TABLE IF NOT EXISTS fixed_candidate_positions(\
        context_key TEXT NOT NULL,entry_key TEXT NOT NULL,value TEXT NOT NULL,\
        position INTEGER NOT NULL CHECK(position BETWEEN 1 AND 5),\
        PRIMARY KEY(context_key,entry_key,value),UNIQUE(context_key,position));\
        CREATE TABLE IF NOT EXISTS quick_phrase_slots(\
        code TEXT PRIMARY KEY,slot INTEGER NOT NULL CHECK(slot>=0));\
        CREATE TABLE IF NOT EXISTS english_slots(\
        code TEXT PRIMARY KEY,slot INTEGER NOT NULL CHECK(slot>=0));\
        CREATE TABLE IF NOT EXISTS date_time_format_order(\
        category TEXT NOT NULL,format TEXT NOT NULL,position INTEGER NOT NULL CHECK(position>=0),\
        PRIMARY KEY(category,format));\
        CREATE TABLE IF NOT EXISTS dictionary_state(name TEXT PRIMARY KEY,value TEXT NOT NULL);";
    if !exec(db, sql) {
        return false;
    }
    if !has_column(db, "user_dictionary_operations", "user_inserted") && !exec(db, "ALTER TABLE user_dictionary_operations ADD COLUMN user_inserted INTEGER NOT NULL DEFAULT 0") {
        return false;
    }
    if !has_column(db, "user_dictionary_operations", "weight_edited") && !exec(db, "ALTER TABLE user_dictionary_operations ADD COLUMN weight_edited INTEGER NOT NULL DEFAULT 0") {
        return false;
    }
    // 写 user_version 即使值相同也是一次写；已经是当前版本就跳过，打开连接就不会去等别的写者。
    if db.query_row("PRAGMA user_version", [], |row| row.get::<_, i64>(0)).is_ok_and(|v| v == 3) {
        return true;
    }
    exec(db, "PRAGMA user_version=3")
}

/// 日志连接：打开（必要时创建）、切到 WAL、建表。离开作用域时回滚没收尾的事务。
struct UserDatabase {
    db: Connection,
}

impl UserDatabase {
    fn open(path: &str) -> Option<Self> {
        let db = open_database(path, OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE)?;
        let _ = use_write_ahead_log(&db);
        if !ensure_schema(&db) {
            return None;
        }
        Some(Self { db })
    }
}

impl Drop for UserDatabase {
    fn drop(&mut self) {
        if !self.db.is_autocommit() {
            let _ = self.db.execute_batch("ROLLBACK");
        }
    }
}

pub fn ensure_user_database(user_db_path: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    // 删掉旧阶梯重排留下的调频行，免得安装时重放把出厂词频压成负权重。
    let _ = db.db.execute_batch("DELETE FROM user_dictionary_operations WHERE dictionary='pinyin' AND operation='upsert' AND weight < 1");
    let _ = db.db.execute_batch(&format!("DELETE FROM user_dictionary_operations WHERE {LEARNED_KANA_PINYIN_CONDITION}"));
    true
}

// ---------------------------------------------------------------------------------------------
// 拼音键与权重
// ---------------------------------------------------------------------------------------------

fn pinyin_segments(key: &str) -> Vec<String> {
    let mut segments = Vec::new();
    for segment in key.split('\'') {
        if segment.is_empty() {
            return Vec::new();
        }
        segments.push(segment.to_string());
    }
    segments
}

fn pinyin_table(key: &str) -> String {
    let segments = pinyin_segments(key);
    let Some(first) = segments.first() else { return String::new() };
    let initial = first.as_bytes()[0];
    if !initial.is_ascii_lowercase() {
        return String::new();
    }
    let count = segments.len();
    format!("tbl_{}_{}", if count <= 7 { count.to_string() } else { "others".to_string() }, initial as char)
}

fn clamp_managed_weight(weight: i64) -> i64 {
    weight.clamp(MANAGED_WEIGHT_FLOOR, MANAGED_WEIGHT_CEILING)
}

fn candidate_dictionary_key(item: &WordItem, context_key: &str) -> String {
    if !item.canonical_pinyin.is_empty() {
        return item.canonical_pinyin.clone();
    }
    let context_segments = pinyin_segments(context_key);
    if context_segments.len() <= 1 {
        return item.pinyin.clone();
    }
    let mut item_segments = context_segments;
    let char_count = item.word.chars().count();
    if char_count > 0 && item_segments.len() > char_count {
        item_segments.truncate(char_count);
    }
    item_segments.join("'")
}

fn write_upsert_journal(journal: &Connection, kind: DictionaryKind, key: &str, value: &str, weight: i64, display: &str) -> bool {
    journal
        .execute(
            "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display) VALUES(?1,?2,?3,'upsert',?4,?5)\
             ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',weight=excluded.weight,\
             display=excluded.display,weight_edited=0,updated_at=unixepoch()",
            params![kind_name(kind), key, value, weight, display],
        )
        .is_ok()
}

fn update_pinyin_weight(main_db: &Connection, journal: &Connection, key: &str, value: &str, weight: i64) -> bool {
    let weight = clamp_managed_weight(weight);
    let table = pinyin_table(key);
    if table.is_empty() {
        return false;
    }
    let Ok(changes) = main_db.execute(&format!("UPDATE \"{table}\" SET weight=?1 WHERE key=?2 AND value=?3"), params![weight, key, value]) else { return false };
    // 调频只改已有的行。没改到行时不能记日志：回放的 apply_pinyin 找不到行就插入，词库里本不存在的词（如日文候选，#869）
    // 会在升级后凭空出现。
    if changes == 0 {
        return true;
    }
    write_upsert_journal(journal, DictionaryKind::Pinyin, key, value, weight, "")
}

fn update_wubi_weight(main_db: &Connection, journal: &Connection, key: &str, value: &str, weight: i64) -> bool {
    let weight = clamp_managed_weight(weight);
    if key.is_empty() || value.is_empty() {
        return false;
    }
    main_db.execute("UPDATE wubi86 SET weight=?1 WHERE key=?2 AND value=?3", params![weight, key, value]).is_ok_and(|changes| changes > 0)
        && write_upsert_journal(journal, DictionaryKind::Wubi, key, value, weight, "")
}

fn update_ranked_weight(main_db: &Connection, journal: &Connection, kind: DictionaryKind, key: &str, value: &str, weight: i64) -> bool {
    if kind == DictionaryKind::Wubi {
        update_wubi_weight(main_db, journal, key, value, weight)
    } else {
        update_pinyin_weight(main_db, journal, key, value, weight)
    }
}

fn ranking_target(rank: usize, mode: &str, linear_step: i32, force_top: bool) -> usize {
    if force_top {
        return 0;
    }
    match mode {
        "halve" => rank / 2,
        "linear" => {
            let step = linear_step as usize;
            if rank > step {
                rank - step
            } else {
                0
            }
        }
        "promote" => {
            if rank > 4 {
                4
            } else {
                rank.wrapping_sub(1)
            }
        }
        _ => 0,
    }
}

fn jianpin(segments: &[String]) -> String {
    segments.iter().map(|s| &s[..1]).collect()
}

fn apply_pinyin(db: &Connection, key: &str, value: &str, operation: &str, weight: i64) -> bool {
    let segments = pinyin_segments(key);
    if segments.is_empty() {
        return false;
    }
    let table = pinyin_table(key);
    if operation == "delete" {
        return db.execute(&format!("DELETE FROM \"{table}\" WHERE key=?1 AND value=?2"), params![key, value]).is_ok();
    }
    let jp = jianpin(&segments);
    match db.execute(&format!("UPDATE \"{table}\" SET jp=?1,weight=?2 WHERE key=?3 AND value=?4"), params![jp, weight, key, value]) {
        Ok(changes) if changes > 0 => return true,
        Ok(_) => {}
        Err(_) => return false,
    }
    db.execute(&format!("INSERT INTO \"{table}\"(key,jp,value,weight) VALUES(?1,?2,?3,?4)"), params![key, jp, value, weight]).is_ok()
}

fn apply_simple(db: &Connection, table: &str, key: &str, value: &str, operation: &str, weight: i64) -> bool {
    if operation == "delete" {
        return db.execute(&format!("DELETE FROM \"{table}\" WHERE \"key\"=?1 AND \"value\"=?2"), params![key, value]).is_ok();
    }
    match db.execute(&format!("UPDATE \"{table}\" SET weight=?1 WHERE \"key\"=?2 AND \"value\"=?3"), params![weight, key, value]) {
        Ok(changes) if changes > 0 => return true,
        Ok(_) => {}
        Err(_) => return false,
    }
    db.execute(&format!("INSERT INTO \"{table}\"(\"key\",\"value\",weight) VALUES(?1,?2,?3)"), params![key, value, weight]).is_ok()
}

fn apply_english(db: &Connection, key: &str, value: &str, operation: &str, weight: i64, display: &str) -> bool {
    let effective_display = if display.is_empty() { value } else { display };
    if operation == "delete" {
        return db.execute("DELETE FROM replay_english.english_words WHERE word=?1 AND display=?2", params![key, effective_display]).is_ok();
    }
    db.execute(
        "INSERT INTO replay_english.english_words(word,display,weight) VALUES(?1,?2,?3) ON CONFLICT(word,display) DO UPDATE SET weight=excluded.weight",
        params![key, effective_display, weight],
    )
    .is_ok()
}

// ---------------------------------------------------------------------------------------------
// 日志记录与查询
// ---------------------------------------------------------------------------------------------

pub fn record_upsert(user_db_path: &str, kind: DictionaryKind, key: &str, value: &str, weight: i64, display: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    write_upsert_journal(&db.db, kind, key, value, weight, display)
}

/// 用户在设置页给词库里已有的词改了权重。和 `record_upsert` 一样写一条 upsert，但记下这是用户亲手定的值：出厂词库换过时
/// 回放照写这个权重，不像自动调频那样让位给新的出厂权重（见 `replay`）。
pub fn record_weight_edit(user_db_path: &str, kind: DictionaryKind, key: &str, value: &str, weight: i64, display: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    db.db
        .execute(
            "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display,weight_edited)\
             VALUES(?1,?2,?3,'upsert',?4,?5,1)\
             ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',weight=excluded.weight,\
             display=excluded.display,weight_edited=1,updated_at=unixepoch()",
            params![kind_name(kind), key, value, weight, display],
        )
        .is_ok()
}

pub fn record_user_insert(user_db_path: &str, kind: DictionaryKind, key: &str, value: &str, weight: i64, display: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    db.db
        .execute(
            "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display,user_inserted)\
             VALUES(?1,?2,?3,'upsert',?4,?5,1)\
             ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',weight=excluded.weight,\
             display=excluded.display,user_inserted=1,updated_at=unixepoch()",
            params![kind_name(kind), key, value, weight, display],
        )
        .is_ok()
}

pub fn record_delete(user_db_path: &str, kind: DictionaryKind, key: &str, value: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    db.db
        .execute(
            "INSERT INTO user_dictionary_operations(dictionary,key,value,operation) VALUES(?1,?2,?3,'delete')\
             ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='delete',weight=0,display='',updated_at=unixepoch()",
            params![kind_name(kind), key, value],
        )
        .is_ok()
}

pub fn is_user_inserted(user_db_path: &str, kind: DictionaryKind, key: &str, value: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    db.db
        .query_row(
            "SELECT user_inserted FROM user_dictionary_operations WHERE dictionary=?1 AND key=?2 AND value=?3 LIMIT 1",
            params![kind_name(kind), key, value],
            |row| row.get::<_, i64>(0),
        )
        .is_ok_and(|v| v != 0)
}

/// 该键下是否有用户留下的权重（调频或造词的 upsert，不看具体词）。删除记录不算：它表达的是「不要这个词」。
pub fn has_user_upsert_for_key(user_db_path: &str, kind: DictionaryKind, key: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    db.db
        .query_row("SELECT 1 FROM user_dictionary_operations WHERE dictionary=?1 AND key=?2 AND operation='upsert' LIMIT 1", params![kind_name(kind), key], |_| Ok(()))
        .is_ok()
}

pub fn record_pinyin_upsert_from_database(main_db_path: &str, key: &str, value: &str, user_db_path: &str) -> bool {
    let Some(db) = open_database(main_db_path, OpenFlags::SQLITE_OPEN_READ_ONLY) else { return false };
    let table = pinyin_table(key);
    if table.is_empty() {
        return false;
    }
    let Ok(weight) = db.query_row(&format!("SELECT weight FROM \"{table}\" WHERE key=?1 AND value=?2 LIMIT 1"), params![key, value], |row| row.get::<_, i64>(0)) else {
        return false;
    };
    record_upsert(user_db_path, DictionaryKind::Pinyin, key, value, weight, "")
}

// ---------------------------------------------------------------------------------------------
// 固定位置
// ---------------------------------------------------------------------------------------------

pub fn set_fixed_position(user_db_path: &str, context_key: &str, entry_key: &str, value: &str, position: i32) -> bool {
    if !(1..=5).contains(&position) || context_key.is_empty() || entry_key.is_empty() || value.is_empty() {
        return false;
    }
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    let _ = db.db.execute_batch("BEGIN IMMEDIATE");
    let ok = db.db.execute("DELETE FROM fixed_candidate_positions WHERE context_key=?1 AND position=?2", params![context_key, position]).is_ok()
        && db
            .db
            .execute(
                "INSERT INTO fixed_candidate_positions(context_key,entry_key,value,position) VALUES(?1,?2,?3,?4)\
                 ON CONFLICT(context_key,entry_key,value) DO UPDATE SET position=excluded.position",
                params![context_key, entry_key, value, position],
            )
            .is_ok();
    let _ = db.db.execute_batch(if ok { "COMMIT" } else { "ROLLBACK" });
    ok
}

pub fn clear_fixed_position(user_db_path: &str, context_key: &str, entry_key: &str, value: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    db.db.execute("DELETE FROM fixed_candidate_positions WHERE context_key=?1 AND entry_key=?2 AND value=?3", params![context_key, entry_key, value]).is_ok()
}

pub fn is_fixed(user_db_path: &str, context_key: &str, entry_key: &str, value: &str) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    db.db
        .query_row("SELECT 1 FROM fixed_candidate_positions WHERE context_key=?1 AND entry_key=?2 AND value=?3", params![context_key, entry_key, value], |_| Ok(()))
        .is_ok()
}

impl crate::lattice::Positioned for WordItem {
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

pub type FindCandidate<'a, T = WordItem> = &'a dyn Fn(&str, &str) -> Option<T>;

/// 把用户固定的候选摆回它们的位置。云和 AI 候选平时先拿出来，摆完再放回第 1、2 位；
/// 调用方已经定好它们的位置时（比如辅助码筛过之后）用 `keep_dynamic_candidate_positions`。
pub fn apply_fixed_positions<T: crate::lattice::Positioned + Clone>(
    user_db_path: &str,
    context_key: &str,
    candidates: &mut Vec<T>,
    include_missing: bool,
    find_candidate: Option<FindCandidate<T>>,
    keep_dynamic_candidate_positions: bool,
) {
    if context_key.is_empty() || candidates.is_empty() {
        return;
    }
    let Some(user_db) = UserDatabase::open(user_db_path) else { return };
    let Ok(mut fixed) = user_db.db.prepare("SELECT entry_key,value,position FROM fixed_candidate_positions WHERE context_key=?1 ORDER BY position") else {
        return;
    };
    let Ok(fixed_rows) = fixed.query_map(params![context_key], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, i32>(2)?))) else {
        return;
    };
    let fixed_rows: Vec<(String, String, i32)> = fixed_rows.filter_map(Result::ok).collect();

    let mut dynamic_candidates: Vec<T> = Vec::new();
    if !keep_dynamic_candidate_positions {
        candidates.retain(|item| {
            if item.source() != Source::CloudSuggestion && item.source() != Source::AiSuggestion {
                return true;
            }
            dynamic_candidates.push(item.clone());
            false
        });
    }

    let mut rows: Vec<(T, i32)> = Vec::new();
    for (key, value, position) in fixed_rows {
        if let Some(existing) = candidates.iter().find(|item| item.word() == value) {
            let mut item = existing.clone();
            item.set_fixed_position(position);
            rows.push((item, position));
        } else if include_missing {
            if let Some(mut item) = find_candidate.and_then(|f| f(&key, &value)) {
                item.set_fixed_position(position);
                rows.push((item, position));
            }
        }
    }
    for (row, _) in &rows {
        candidates.retain(|item| item.word() != row.word());
    }
    for (row, position) in rows {
        let index = ((position - 1) as usize).min(candidates.len());
        candidates.insert(index, row);
    }
    for item in dynamic_candidates {
        let preferred = if item.source() == Source::CloudSuggestion { 1 } else { 2 };
        let index = preferred.min(candidates.len());
        candidates.insert(index, item);
    }
}

// ---------------------------------------------------------------------------------------------
// 中文调频
// ---------------------------------------------------------------------------------------------

fn counter_reached(db: &Connection, context_key: &str, entry_key: &str, value: &str, trigger_count: i32) -> Option<bool> {
    let count: i64 = db
        .query_row(
            "INSERT INTO candidate_selection_state(context_key,entry_key,value,selection_count) VALUES(?1,?2,?3,1)\
             ON CONFLICT(context_key,entry_key,value) DO UPDATE SET selection_count=selection_count+1 RETURNING selection_count",
            params![context_key, entry_key, value],
            |row| row.get(0),
        )
        .ok()?;
    Some(count >= i64::from(trigger_count))
}

fn reset_counter(db: &Connection, context_key: &str, entry_key: &str, value: &str) -> bool {
    db.execute("DELETE FROM candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3", params![context_key, entry_key, value]).is_ok()
}

/// 用户选中了 `value`：计数到 `trigger_count` 次（或 `force_top`）后，在 `msime.db` 里把它的权重挪到目标名次，
/// 需要时重排邻近几行，并把新权重记进日志。两个库各开一个事务，先提交日志、再提交词库。
#[allow(clippy::too_many_arguments)]
pub fn adjust_candidate_ranking(
    main_db_path: &str,
    user_db_path: &str,
    context_key: &str,
    ordered_candidates: &[WordItem],
    entry_key: &str,
    value: &str,
    mode: &str,
    linear_step: i32,
    trigger_count: i32,
    force_top: bool,
    ranking_changed: Option<&mut bool>,
    kind: DictionaryKind,
) -> bool {
    let mut changed = false;
    let result = adjust_candidate_ranking_inner(
        main_db_path, user_db_path, context_key, ordered_candidates, entry_key, value, mode, linear_step, trigger_count, force_top, &mut changed, kind,
    );
    if let Some(flag) = ranking_changed {
        *flag = changed;
    }
    result
}

#[allow(clippy::too_many_arguments)]
fn adjust_candidate_ranking_inner(
    main_db_path: &str,
    user_db_path: &str,
    context_key: &str,
    ordered_candidates: &[WordItem],
    entry_key: &str,
    value: &str,
    mode: &str,
    linear_step: i32,
    trigger_count: i32,
    force_top: bool,
    ranking_changed: &mut bool,
    kind: DictionaryKind,
) -> bool {
    if entry_key.is_empty() || value.is_empty() || ordered_candidates.is_empty() || is_fixed(user_db_path, context_key, entry_key, value) {
        return false;
    }
    if !force_top && mode == "disabled" {
        return true;
    }
    let Some(user_db) = UserDatabase::open(user_db_path) else { return false };
    if !exec(&user_db.db, "BEGIN IMMEDIATE") {
        return false;
    }
    let rollback_user = || {
        let _ = exec(&user_db.db, "ROLLBACK");
    };
    let trigger_count = trigger_count.min(10).max(1);
    if !force_top {
        match counter_reached(&user_db.db, context_key, entry_key, value, trigger_count) {
            None => {
                rollback_user();
                return false;
            }
            Some(false) => {
                let committed = exec(&user_db.db, "COMMIT");
                if !committed {
                    rollback_user();
                }
                return committed;
            }
            Some(true) => {}
        }
    }

    // 一个列表里混着几个 entry_key（单字母简拼、九键数字、改切分的候选），共用一个权重尺度：按权重从高到低排。
    // 中点算法要求这个顺序；显示顺序里有被保护的位置和钉住的候选，不满足它。写入只落在 entry_key 自己的行上。
    let mut database_candidates: Vec<WordItem> =
        ordered_candidates.iter().filter(|item| item.source == Source::Database || item.source == Source::UserDatabase).cloned().collect();
    database_candidates.sort_by(|a, b| b.weight.cmp(&a.weight));
    let owns_entry_key: Vec<bool> = database_candidates
        .iter()
        .map(|item| {
            let item_key = if kind == DictionaryKind::Wubi { item.pinyin.clone() } else { candidate_dictionary_key(item, context_key) };
            item_key == entry_key
        })
        .collect();
    let Some(rank) = database_candidates.iter().position(|item| item.word == value) else {
        rollback_user();
        return false;
    };
    if rank == 0 {
        let committed = reset_counter(&user_db.db, context_key, entry_key, value) && exec(&user_db.db, "COMMIT");
        if !committed {
            rollback_user();
        }
        return committed;
    }
    let target = ranking_target(rank, mode, linear_step, force_top);
    let main_db = open_database(main_db_path, read_write());
    let Some(main_db) = main_db.filter(|db| exec(db, "BEGIN IMMEDIATE")) else {
        rollback_user();
        return false;
    };
    let rollback_main = || {
        let _ = exec(&main_db, "ROLLBACK");
    };
    // 学到的权重留在出货词典的量级内（现在低于 7700 万）；曾经有一次局部重排写出 10^12 级数值，
    // 还有一次拿权重为 1 的生僻词当阶梯基准，把负权重写到了邻近的键上。
    let mut new_weight: i64 = 0;
    let top_near_limit = target == 0 && database_candidates[0].weight > i64::MAX - 1000;
    let upper = if target == 0 && !top_near_limit {
        database_candidates[0].weight + 1000
    } else if target == 0 {
        database_candidates[0].weight
    } else {
        database_candidates[target - 1].weight
    };
    let lower = database_candidates[target].weight;
    let oversized = upper > MANAGED_WEIGHT_CEILING || lower > MANAGED_WEIGHT_CEILING;
    let mut need_rebalance = top_near_limit || lower == i64::MAX || upper <= lower || oversized;
    if !need_rebalance {
        if upper == lower + 1 {
            // 没有整数中点：在上面那个候选之上加一，并跳过一小段已被占用的权重。
            new_weight = upper + 1;
            let mut conflicts = 0;
            for i in (0..target).rev() {
                let occupied = database_candidates[i].weight;
                if occupied < new_weight {
                    continue;
                }
                if occupied > new_weight {
                    break;
                }
                conflicts += 1;
                if conflicts >= 16 || new_weight >= MANAGED_WEIGHT_CEILING {
                    need_rebalance = true;
                    break;
                }
                new_weight += 1;
            }
        } else {
            new_weight = lower + (upper - lower) / 2;
        }
    }
    if need_rebalance && !oversized && !top_near_limit && target != 0 {
        let rebalance_end = database_candidates.len().min(target + REBALANCE_COUNT);
        let last_index = rebalance_end as i64 - 1;
        let last_weight = upper - REBALANCE_GAP - (last_index - target as i64) * REBALANCE_GAP;
        if last_weight < MANAGED_WEIGHT_FLOOR {
            // 同权重或低权重没有位置放 16 级的递减阶梯：只抬选中的这一行，不往邻居上写负数。
            let cluster = upper.max(lower);
            if cluster <= i64::MAX - REBALANCE_GAP {
                new_weight = clamp_managed_weight(cluster + REBALANCE_GAP);
                need_rebalance = false;
            }
        }
    }
    if need_rebalance {
        let rebalance_begin = target;
        let rebalance_end = database_candidates.len().min(rebalance_begin + REBALANCE_COUNT);
        let mut base = if target == 0 { MANAGED_WEIGHT_CEILING - REBALANCE_GAP } else { MANAGED_WEIGHT_CEILING };
        if target != 0 && !top_near_limit && !oversized {
            let requested_base = upper + target as i64 * REBALANCE_GAP - REBALANCE_GAP;
            base = requested_base.min(MANAGED_WEIGHT_CEILING);
        }
        let mut rebalance_ok = true;
        for i in rebalance_begin..rebalance_end {
            if !rebalance_ok {
                break;
            }
            if !owns_entry_key[i] {
                continue;
            }
            let weight = clamp_managed_weight(base - i as i64 * REBALANCE_GAP);
            rebalance_ok = update_ranked_weight(&main_db, &user_db.db, kind, entry_key, &database_candidates[i].word, weight);
        }
        if !rebalance_ok {
            rollback_main();
            rollback_user();
            return false;
        }
        new_weight = if target == 0 { base + REBALANCE_GAP } else { base - target as i64 * REBALANCE_GAP + REBALANCE_GAP / 2 };
    }
    new_weight = clamp_managed_weight(new_weight);
    if !update_ranked_weight(&main_db, &user_db.db, kind, entry_key, value, new_weight) {
        rollback_main();
        rollback_user();
        return false;
    }
    if !reset_counter(&user_db.db, context_key, entry_key, value) || !exec(&user_db.db, "COMMIT") {
        rollback_main();
        rollback_user();
        return false;
    }
    if !exec(&main_db, "COMMIT") {
        rollback_main();
        return false;
    }
    *ranking_changed = true;
    true
}

// ---------------------------------------------------------------------------------------------
// 英文候选调频
// ---------------------------------------------------------------------------------------------

/// 只和英文候选比：混输列表里的中文权重是另一个词库的量纲。英文排在中文第几位由按输入记的槽位决定。
#[allow(clippy::too_many_arguments)]
pub fn adjust_english_candidate_ranking(
    english_db_path: &str,
    user_db_path: &str,
    context_key: &str,
    ordered_candidates: &[WordItem],
    entry_key: &str,
    value: &str,
    mode: &str,
    linear_step: i32,
    trigger_count: i32,
    force_top: bool,
    ranking_changed: Option<&mut bool>,
) -> bool {
    let mut changed = false;
    let result = adjust_english_inner(
        english_db_path, user_db_path, context_key, ordered_candidates, entry_key, value, mode, linear_step, trigger_count, force_top, &mut changed,
    );
    if let Some(flag) = ranking_changed {
        *flag = changed;
    }
    result
}

#[allow(clippy::too_many_arguments)]
fn adjust_english_inner(
    english_db_path: &str,
    user_db_path: &str,
    context_key: &str,
    ordered_candidates: &[WordItem],
    entry_key: &str,
    value: &str,
    mode: &str,
    linear_step: i32,
    trigger_count: i32,
    force_top: bool,
    ranking_changed: &mut bool,
) -> bool {
    if entry_key.is_empty() || value.is_empty() || ordered_candidates.is_empty() || is_fixed(user_db_path, context_key, entry_key, value) {
        return false;
    }
    if !force_top && mode == "disabled" {
        return true;
    }
    let Some(user_db) = UserDatabase::open(user_db_path) else { return false };
    if !exec(&user_db.db, "BEGIN IMMEDIATE") {
        return false;
    }
    let rollback_user = || {
        let _ = exec(&user_db.db, "ROLLBACK");
    };
    let trigger_count = trigger_count.min(10).max(1);
    if !force_top {
        match counter_reached(&user_db.db, context_key, entry_key, value, trigger_count) {
            None => {
                rollback_user();
                return false;
            }
            Some(false) => {
                let committed = exec(&user_db.db, "COMMIT");
                if !committed {
                    rollback_user();
                }
                return committed;
            }
            Some(true) => {}
        }
    }

    let english_candidates: Vec<&WordItem> = ordered_candidates.iter().filter(|item| item.source == Source::EnglishDictionary).collect();
    let Some(rank) = english_candidates.iter().position(|item| item.pinyin == entry_key && item.word == value) else {
        rollback_user();
        return false;
    };
    let selected = english_candidates[rank];
    if rank == 0 {
        let committed = exec(&user_db.db, "COMMIT");
        if !committed {
            rollback_user();
        }
        return committed;
    }
    let target = if force_top {
        0
    } else {
        match mode {
            "halve" => rank / 2,
            "linear" => {
                let step = linear_step.max(1) as usize;
                if rank > step {
                    rank - step
                } else {
                    0
                }
            }
            "promote" => {
                if rank > 4 {
                    4
                } else {
                    rank - 1
                }
            }
            _ => 0,
        }
    };
    let maximum_weight = english_candidates.iter().fold(0i64, |maximum, item| maximum.max(item.weight));
    let new_weight = if target == 0 { maximum_weight.max(selected.weight) + 1000 } else { english_candidates[target - 1].weight + 1 };

    let english_db = open_database(english_db_path, read_write());
    // 先把新权重记进日志再写 english.db：日志里领先一步可以重放补上，没记进日志的提权会在下次词库升级时被悄悄还原。
    // 这一行必须已经存在，否则重放会插进一个用户从没学过的词。
    let exists = english_db
        .as_ref()
        .is_some_and(|db| db.query_row("SELECT 1 FROM english_words WHERE word=?1 AND display=?2", params![entry_key, value], |_| Ok(())).is_ok());
    if english_db.is_none() || !exists || !write_upsert_journal(&user_db.db, DictionaryKind::English, entry_key, value, new_weight, value) {
        rollback_user();
        return false;
    }
    let reset_ok = reset_counter(&user_db.db, context_key, entry_key, value);
    if !reset_ok || !exec(&user_db.db, "COMMIT") {
        rollback_user();
        return false;
    }
    let english_db = english_db.unwrap();
    let ok = english_db
        .execute("UPDATE english_words SET weight=?1 WHERE word=?2 AND display=?3", params![new_weight, entry_key, value])
        .is_ok_and(|changes| changes > 0);
    if ok {
        *ranking_changed = true;
    }
    ok
}

// ---------------------------------------------------------------------------------------------
// 删词、五笔置顶
// ---------------------------------------------------------------------------------------------

fn attach(db: &Connection, path: &str, name: &str) -> bool {
    db.execute(&format!("ATTACH DATABASE ?1 AS {name}"), params![path]).is_ok()
}

/// 精确删一个词并把墓碑记进日志，一个事务（词库和日志附在同一个连接上）。
pub fn delete_dictionary_candidate(dictionary_db_path: &str, user_db_path: &str, kind: DictionaryKind, entry_key: &str, value: &str) -> bool {
    if entry_key.is_empty() || value.is_empty() {
        return false;
    }
    let table = match kind {
        DictionaryKind::Pinyin => pinyin_table(entry_key),
        DictionaryKind::Wubi => "wubi86".to_string(),
        DictionaryKind::English => "english_words".to_string(),
        DictionaryKind::QuickPhrase => return false,
    };
    if table.is_empty() {
        return false;
    }
    if !ensure_user_database(user_db_path) {
        return false;
    }
    let Some(database) = open_database(dictionary_db_path, read_write()) else { return false };
    if !attach(&database, user_db_path, "candidate_journal") {
        return false;
    }
    if !exec(&database, "BEGIN IMMEDIATE") {
        return false;
    }
    let rollback = || {
        let _ = exec(&database, "ROLLBACK");
        false
    };
    let columns = if kind == DictionaryKind::English { "word=?1 AND display=?2" } else { "key=?1 AND value=?2" };
    match database.execute(&format!("DELETE FROM main.\"{table}\" WHERE {columns}"), params![entry_key, value]) {
        Ok(changes) if changes > 0 => {}
        _ => return rollback(),
    }
    if database
        .execute(
            "INSERT INTO candidate_journal.user_dictionary_operations(dictionary,key,value,operation) VALUES(?1,?2,?3,'delete')\
             ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='delete',weight=0,display='',updated_at=unixepoch()",
            params![kind_name(kind), entry_key, value],
        )
        .is_err()
    {
        return rollback();
    }
    if !exec(&database, "COMMIT") {
        return rollback();
    }
    true
}

pub fn delete_english_candidate(english_db_path: &str, user_db_path: &str, entry_key: &str, value: &str) -> bool {
    delete_dictionary_candidate(english_db_path, user_db_path, DictionaryKind::English, entry_key, value)
}

const JOURNAL_UPSERT_VIA_ATTACH: &str = "INSERT INTO candidate_journal.user_dictionary_operations(dictionary,key,value,operation,weight,display) \
    VALUES(?1,?2,?3,'upsert',?4,'') ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',\
    weight=excluded.weight,display='',updated_at=unixepoch()";

/// 选中的五笔候选升到同码组的最高权重 + 1，并记日志，一个事务。行不存在时不造行。
pub fn bump_wubi_weight(main_db_path: &str, user_db_path: &str, key: &str, value: &str) -> bool {
    if key.is_empty() || value.is_empty() {
        return false;
    }
    if !ensure_user_database(user_db_path) {
        return false;
    }
    let Some(database) = open_database(main_db_path, read_write()) else { return false };
    if !attach(&database, user_db_path, "candidate_journal") || !exec(&database, "BEGIN IMMEDIATE") {
        return false;
    }
    let rollback = || {
        let _ = exec(&database, "ROLLBACK");
        false
    };
    let Ok(current) = database.query_row("SELECT MAX(weight) FROM main.\"wubi86\" WHERE \"key\"=?1", params![key], |row| row.get::<_, Option<i64>>(0)) else {
        return rollback();
    };
    let new_weight = clamp_managed_weight(current.unwrap_or(0) + 1);
    match database.execute("UPDATE main.\"wubi86\" SET weight=?1 WHERE \"key\"=?2 AND \"value\"=?3", params![new_weight, key, value]) {
        Ok(changes) if changes > 0 => {}
        _ => return rollback(),
    }
    if database.execute(JOURNAL_UPSERT_VIA_ATTACH, params![kind_name(DictionaryKind::Wubi), key, value, new_weight]).is_err() {
        return rollback();
    }
    if !exec(&database, "COMMIT") {
        return rollback();
    }
    true
}

// ---------------------------------------------------------------------------------------------
// 槽位：快捷短语、英文
// ---------------------------------------------------------------------------------------------

/// 快捷短语组的选择计数借用 candidate_selection_state，context 按编码区分，promote / bypass 两个方向各数各的。
const QUICK_PHRASE_SELECTION_CONTEXT: &str = "quick_phrase:";
const ENGLISH_SLOT_SELECTION_CONTEXT: &str = "english_slot:";
const DATE_TIME_SELECTION_CONTEXT: &str = "date_time_order:";

fn read_quick_phrase_slot(db: &Connection, schema: &str, code: &str) -> Option<i32> {
    let mut statement = db.prepare(&format!("SELECT slot FROM {schema}quick_phrase_slots WHERE code=?1")).ok()?;
    let mut rows = statement.query(params![code]).ok()?;
    match rows.next() {
        Ok(Some(row)) => Some(row.get::<_, i32>(0).ok()?.max(0)),
        Ok(None) => Some(0),
        Err(_) => None,
    }
}

fn write_quick_phrase_slot(db: &Connection, schema: &str, code: &str, slot: i32) -> bool {
    if slot <= 0 {
        db.execute(&format!("DELETE FROM {schema}quick_phrase_slots WHERE code=?1"), params![code]).is_ok()
    } else {
        db.execute(
            &format!("INSERT INTO {schema}quick_phrase_slots(code,slot) VALUES(?1,?2) ON CONFLICT(code) DO UPDATE SET slot=excluded.slot"),
            params![code, slot],
        )
        .is_ok()
    }
}

/// 计一次数；到 `trigger_count` 时置 reached 并清零，计数不会越过状态导出允许的上限。
fn count_slot_selection(db: &Connection, schema: &str, context_prefix: &str, code: &str, direction: &str, trigger_count: i32) -> Option<bool> {
    let context = format!("{context_prefix}{code}");
    let count: i64 = db
        .query_row(
            &format!(
                "INSERT INTO {schema}candidate_selection_state(context_key,entry_key,value,selection_count) VALUES(?1,?2,?3,1)\
                 ON CONFLICT(context_key,entry_key,value) DO UPDATE SET selection_count=selection_count+1 RETURNING selection_count"
            ),
            params![context, direction, code],
            |row| row.get(0),
        )
        .ok()?;
    let reached = count >= i64::from(trigger_count.min(10).max(1));
    if !reached {
        return Some(false);
    }
    db.execute(&format!("DELETE FROM {schema}candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3"), params![context, direction, code])
        .ok()?;
    Some(true)
}

pub fn quick_phrase_slot(user_db_path: &str, code: &str) -> i32 {
    if code.is_empty() {
        return 0;
    }
    let Some(db) = UserDatabase::open(user_db_path) else { return 0 };
    read_quick_phrase_slot(&db.db, "", code).unwrap_or(0)
}

/// 用户选中了组里的一条快捷短语。累计到 `trigger_count` 次后按调频模式把组往前挪，选中的不是组内第一条时
/// 再把它的权重升到组内最高 + 1 并记日志。
pub fn learn_quick_phrase_selection(
    main_db_path: &str,
    user_db_path: &str,
    code: &str,
    value: &str,
    first_in_group: bool,
    mode: &str,
    linear_step: i32,
    trigger_count: i32,
) -> bool {
    if code.is_empty() || value.is_empty() {
        return false;
    }
    if mode == "disabled" {
        return true;
    }
    if !ensure_user_database(user_db_path) {
        return false;
    }
    let Some(database) = open_database(main_db_path, read_write()) else { return false };
    if !attach(&database, user_db_path, "candidate_journal") || !exec(&database, "BEGIN IMMEDIATE") {
        return false;
    }
    let rollback = || {
        let _ = exec(&database, "ROLLBACK");
        false
    };
    let commit = || exec(&database, "COMMIT") || rollback();
    let schema = "candidate_journal.";
    let Some(slot) = read_quick_phrase_slot(&database, schema, code) else { return rollback() };
    // 组在首位、选的又是组内第一条：这就是默认上屏的那一个，没有可学的。
    if slot == 0 && first_in_group {
        return commit();
    }
    let Some(reached) = count_slot_selection(&database, schema, QUICK_PHRASE_SELECTION_CONTEXT, code, "promote", trigger_count) else {
        return rollback();
    };
    if !reached {
        return commit();
    }
    if slot > 0 && !write_quick_phrase_slot(&database, schema, code, ranking_target(slot as usize, mode, linear_step.max(1), false) as i32) {
        return rollback();
    }
    if !first_in_group {
        // 组内顺序按权重走，语义同 bump_wubi_weight：选中的那条升到同码最高 + 1。
        let Ok(current) = database.query_row("SELECT MAX(weight) FROM main.quick_parases WHERE key=?1", params![code], |row| row.get::<_, Option<i64>>(0)) else {
            return rollback();
        };
        let new_weight = clamp_managed_weight(current.unwrap_or(0) + 1);
        match database.execute("UPDATE main.quick_parases SET weight=?1 WHERE key=?2 AND value=?3", params![new_weight, code, value]) {
            Ok(changes) if changes > 0 => {}
            _ => return rollback(),
        }
        if database.execute(JOURNAL_UPSERT_VIA_ATTACH, params![kind_name(DictionaryKind::QuickPhrase), code, value, new_weight]).is_err() {
            return rollback();
        }
    }
    commit()
}

/// 用户越过快捷短语组选了排在组后面的普通候选。整组算作它前面的一个位置，它按调频模式的目标位置越过这一组时，
/// 累计到 `trigger_count` 次后槽位 + 1，最多到 `max_slot`（首页最后一位）。
pub fn learn_quick_phrase_bypass(user_db_path: &str, code: &str, ordinary_rank: i32, mode: &str, linear_step: i32, trigger_count: i32, max_slot: i32) -> bool {
    if code.is_empty() || ordinary_rank < 0 {
        return false;
    }
    if mode == "disabled" || max_slot <= 0 {
        return true;
    }
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    if !exec(&db.db, "BEGIN IMMEDIATE") {
        return false;
    }
    let rollback = || {
        let _ = exec(&db.db, "ROLLBACK");
        false
    };
    let commit = || exec(&db.db, "COMMIT") || rollback();
    let Some(slot) = read_quick_phrase_slot(&db.db, "", code) else { return rollback() };
    // 选中的候选本来就在组前面，或者组已经退到首页最后一位。
    if ordinary_rank < slot || slot >= max_slot {
        return commit();
    }
    let target = ranking_target(ordinary_rank as usize + 1, mode, linear_step.max(1), false);
    if target > slot as usize {
        return commit();
    }
    let Some(reached) = count_slot_selection(&db.db, "", QUICK_PHRASE_SELECTION_CONTEXT, code, "bypass", trigger_count) else {
        return rollback();
    };
    if reached && !write_quick_phrase_slot(&db.db, "", code, (slot + 1).min(max_slot)) {
        return rollback();
    }
    commit()
}

fn write_english_slot(db: &Connection, code: &str, slot: i32) -> bool {
    db.execute("INSERT INTO english_slots(code,slot) VALUES(?1,?2) ON CONFLICT(code) DO UPDATE SET slot=excluded.slot", params![code, slot.max(0)]).is_ok()
}

/// 选择计数与写槽位放在一个事务里；`write_slot` 只在计数到 `trigger_count` 时调用。
fn count_and_write_english_slot(user_db_path: &str, code: &str, direction: &str, trigger_count: i32, write_slot: impl Fn(&Connection) -> bool) -> bool {
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    if !exec(&db.db, "BEGIN IMMEDIATE") {
        return false;
    }
    let rollback = || {
        let _ = exec(&db.db, "ROLLBACK");
        false
    };
    let Some(reached) = count_slot_selection(&db.db, "", ENGLISH_SLOT_SELECTION_CONTEXT, code, direction, trigger_count) else {
        return rollback();
    };
    if reached && !write_slot(&db.db) {
        return rollback();
    }
    exec(&db.db, "COMMIT") || rollback()
}

pub fn english_slot(user_db_path: &str, code: &str) -> Option<i32> {
    if code.is_empty() {
        return None;
    }
    let db = UserDatabase::open(user_db_path)?;
    db.db.query_row("SELECT slot FROM english_slots WHERE code=?1", params![code], |row| row.get::<_, i32>(0)).ok().map(|slot| slot.max(0))
}

/// 用户显式置顶英文候选：直接写槽位，不走计数。
pub fn set_english_slot(user_db_path: &str, code: &str, slot: i32) -> bool {
    if code.is_empty() {
        return false;
    }
    UserDatabase::open(user_db_path).is_some_and(|db| write_english_slot(&db.db, code, slot))
}

pub fn learn_english_slot_selection(user_db_path: &str, code: &str, english_index: i32, mode: &str, linear_step: i32, trigger_count: i32) -> bool {
    if code.is_empty() || english_index < 0 {
        return false;
    }
    // 已经在首位就是默认上屏的那一个，没有可学的。
    if mode == "disabled" || english_index == 0 {
        return true;
    }
    let target = ranking_target(english_index as usize, mode, linear_step.max(1), false) as i32;
    count_and_write_english_slot(user_db_path, code, "promote", trigger_count, |db| write_english_slot(db, code, target))
}

pub fn learn_english_slot_bypass(
    user_db_path: &str,
    code: &str,
    english_index: i32,
    selected_index: i32,
    mode: &str,
    linear_step: i32,
    trigger_count: i32,
    max_slot: i32,
) -> bool {
    if code.is_empty() || english_index < 0 || selected_index <= english_index {
        return false;
    }
    if mode == "disabled" || english_index >= max_slot {
        return true;
    }
    // 和快捷短语组同一个判据：选中的候选按调频模式的目标位置落在英文的位置或更前，才算越过了它。
    let target = ranking_target(selected_index as usize, mode, linear_step.max(1), false);
    if target > english_index as usize {
        return true;
    }
    count_and_write_english_slot(user_db_path, code, "bypass", trigger_count, |db| write_english_slot(db, code, (english_index + 1).min(max_slot)))
}

// ---------------------------------------------------------------------------------------------
// 日期时间格式的顺序
// ---------------------------------------------------------------------------------------------

/// 学到的顺序合进出厂顺序：表里记过的按记下的先后排，没记过的（后来新增的格式）插回它出厂时的下标。
/// 表里有、`default_ids` 里没有的（已经删掉的格式）直接丢掉。
fn read_date_time_order(db: &Connection, category: &str, default_ids: &[String]) -> Option<Vec<String>> {
    let mut statement = db.prepare("SELECT format FROM date_time_format_order WHERE category=?1 ORDER BY position").ok()?;
    let formats: Vec<Option<String>> = statement.query_map(params![category], |row| row.get::<_, Option<String>>(0)).ok()?.collect::<Result<_, _>>().ok()?;
    let mut order: Vec<String> = Vec::new();
    for id in formats.into_iter().flatten() {
        if default_ids.contains(&id) && !order.contains(&id) {
            order.push(id);
        }
    }
    for (index, id) in default_ids.iter().enumerate() {
        if !order.contains(id) {
            let at = index.min(order.len());
            order.insert(at, id.clone());
        }
    }
    Some(order)
}

fn write_date_time_order(db: &Connection, category: &str, order: &[String]) -> bool {
    if db.execute("DELETE FROM date_time_format_order WHERE category=?1", params![category]).is_err() {
        return false;
    }
    order.iter().enumerate().all(|(index, format)| {
        db.execute("INSERT INTO date_time_format_order(category,format,position) VALUES(?1,?2,?3)", params![category, format, index as i64]).is_ok()
    })
}

#[allow(clippy::too_many_arguments)]
fn move_date_time_format(
    user_db_path: &str,
    category: &str,
    default_ids: &[String],
    format_id: &str,
    mode: &str,
    linear_step: i32,
    trigger_count: i32,
    force_top: bool,
) -> bool {
    if category.is_empty() || format_id.is_empty() {
        return false;
    }
    if !force_top && mode == "disabled" {
        return true;
    }
    let Some(db) = UserDatabase::open(user_db_path) else { return false };
    if !exec(&db.db, "BEGIN IMMEDIATE") {
        return false;
    }
    let rollback = || {
        let _ = exec(&db.db, "ROLLBACK");
        false
    };
    let commit = || exec(&db.db, "COMMIT") || rollback();
    let Some(mut order) = read_date_time_order(&db.db, category, default_ids) else { return rollback() };
    // 已经在首位就是默认上屏的那一个，没有可学的。
    let Some(rank) = order.iter().position(|id| id == format_id) else { return commit() };
    if rank == 0 {
        return commit();
    }
    if !force_top {
        let Some(reached) = count_slot_selection(&db.db, "", DATE_TIME_SELECTION_CONTEXT, format_id, "promote", trigger_count) else {
            return rollback();
        };
        if !reached {
            return commit();
        }
    }
    let target = ranking_target(rank, mode, linear_step.max(1), force_top);
    let moved = order.remove(rank);
    let at = target.min(order.len());
    order.insert(at, moved);
    if !write_date_time_order(&db.db, category, &order) {
        return rollback();
    }
    commit()
}

pub fn date_time_fixed_position_context(category: &str) -> String {
    format!("date_time:{category}")
}

/// 按学到的顺序重排一组日期时间候选（`learned_order` 为 false 时跳过），再按固定位置摆放。
/// 候选的格式 ID 在 `pinyin` 字段里。
pub fn apply_date_time_order(user_db_path: &str, category: &str, candidates: &mut Vec<WordItem>, learned_order: bool) {
    if category.is_empty() || candidates.is_empty() {
        return;
    }
    let Some(db) = UserDatabase::open(user_db_path) else { return };
    let default_ids: Vec<String> = candidates.iter().map(|c| c.pinyin.clone()).collect();
    if learned_order {
        if let Some(order) = read_date_time_order(&db.db, category, &default_ids) {
            let rank = |item: &WordItem| order.iter().position(|id| *id == item.pinyin).unwrap_or(order.len());
            candidates.sort_by_key(|item| rank(item));
        }
    }
    // 固定位置借用 fixed_candidate_positions，entry_key 和 value 都写格式 ID：按文字认候选的那套对日期不管用，
    // 日期文本每天都变。
    let Ok(mut fixed) = db.db.prepare("SELECT entry_key,position FROM fixed_candidate_positions WHERE context_key=?1 ORDER BY position") else { return };
    let Ok(rows) = fixed.query_map(params![date_time_fixed_position_context(category)], |row| Ok((row.get::<_, Option<String>>(0)?, row.get::<_, i32>(1)?))) else {
        return;
    };
    let rows: Vec<(Option<String>, i32)> = rows.filter_map(Result::ok).collect();
    let mut moved: Vec<WordItem> = Vec::new();
    for (key, position) in rows {
        let Some(format_id) = key else { continue };
        let Some(index) = candidates.iter().position(|item| item.pinyin == format_id) else { continue };
        let mut item = candidates.remove(index);
        item.fixed_position = position;
        moved.push(item);
    }
    for row in moved {
        let index = ((row.fixed_position.max(1) - 1) as usize).min(candidates.len());
        candidates.insert(index, row);
    }
}

pub fn learn_date_time_selection(user_db_path: &str, category: &str, default_ids: &[String], format_id: &str, mode: &str, linear_step: i32, trigger_count: i32) -> bool {
    move_date_time_format(user_db_path, category, default_ids, format_id, mode, linear_step, trigger_count, false)
}

/// 用户显式置顶：直接挪到首位，不走计数。
pub fn pin_date_time_format(user_db_path: &str, category: &str, default_ids: &[String], format_id: &str) -> bool {
    move_date_time_format(user_db_path, category, default_ids, format_id, "pin", 1, 1, true)
}

/// 用户敲完上屏的英文单词学进 `english.db`（原样大小写作显示，小写作词）并记造词日志；日志写失败就把刚插的行删掉。
pub fn learn_entered_english_word(english_db_path: &str, user_db_path: &str, display: &str, weight: i64) -> bool {
    const MAXIMUM_LEARNED_ENGLISH_WORD_LENGTH: usize = 64;
    if display.is_empty() || display.len() > MAXIMUM_LEARNED_ENGLISH_WORD_LENGTH || !display.bytes().all(|ch| ch.is_ascii_alphabetic()) {
        return false;
    }
    let word = display.to_ascii_lowercase();
    let weight = weight.max(0);
    if !english::ensure_schema(english_db_path) {
        return false;
    }
    let Some(database) = open_database(english_db_path, read_write()) else { return false };
    match database.execute("INSERT OR IGNORE INTO english_words(word,display,weight) VALUES(?1,?2,?3)", params![word, display, weight]) {
        Ok(0) => return true,
        Ok(_) => {}
        Err(_) => return false,
    }
    if record_user_insert(user_db_path, DictionaryKind::English, &word, display, weight, display) {
        return true;
    }
    // 不留下一个撑不过词库升级的词。
    let _ = database.execute("DELETE FROM english_words WHERE word=?1 AND display=?2", params![word, display]);
    false
}

// ---------------------------------------------------------------------------------------------
// 重放
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ReplayResult {
    pub applied: i32,
    pub skipped: i32,
    pub failed: i32,
    /// 出厂词库换过时，日志里低于新出厂权重、因而保留新权重并改记进日志的自动调频行数。
    pub rebased: i32,
    pub error: String,
}

/// `dictionary_state` 里记「日志上次回放到的出厂词库」的那一行。
const REPLAYED_DICTIONARY_STATE: &str = "replayed_dictionary_sha256";

/// 出厂 msime.db 的身份：与它一起装进数据目录的 dictionary-manifest.json 记下的 SHA-256。msime.db 一经调频就不再等于
/// 出厂字节，所以不能拿文件本身算；清单缺失或读不懂时返回空，回放按原样写回日志里的权重。
fn shipped_dictionary_identity(main_db_path: &str) -> Option<String> {
    let manifest_path = Path::new(main_db_path).parent()?.join("dictionary-manifest.json");
    let text = std::fs::read(manifest_path).ok()?;
    let manifest: serde_json::Value = serde_json::from_slice(&text).ok()?;
    let sha256 = manifest.get("files")?.get("msime.db")?.get("sha256")?.as_str()?;
    if sha256.is_empty() { None } else { Some(sha256.to_string()) }
}

fn replayed_dictionary_identity(journal: &Connection) -> Option<String> {
    // 只读打开的旧日志可能还没有 dictionary_state 表，prepare 失败即视为没有记录。
    journal.query_row("SELECT value FROM dictionary_state WHERE name=?1", params![REPLAYED_DICTIONARY_STATE], |row| row.get::<_, Option<String>>(0)).ok().flatten()
}

/// 回放开始前目标词库里这一行的权重，即新装出厂词库的权重；没有这一行时为空。
fn shipped_weight(main_db: &Connection, kind: &str, key: &str, value: &str) -> Option<i64> {
    let table = if kind == "wubi" { "wubi86".to_string() } else { pinyin_table(key) };
    if table.is_empty() {
        return None;
    }
    main_db.query_row(&format!("SELECT MAX(weight) FROM \"{table}\" WHERE key=?1 AND value=?2"), params![key, value], |row| row.get::<_, Option<i64>>(0)).ok().flatten()
}

/// 把改记到新词库上的调频写回日志，并记下这次回放到的词库。写不成只是下次回放再改记一遍，不影响已经提交的 msime.db，
/// 所以不算回放失败。
fn save_rebased_journal(user_db_path: &str, dictionary_identity: &str, rebased: &[(String, String, String, i64)]) -> bool {
    let Some(user_db) = UserDatabase::open(user_db_path) else { return false };
    if !exec(&user_db.db, "BEGIN IMMEDIATE") {
        return false;
    }
    {
        let Ok(mut update) = user_db.db.prepare(
            "UPDATE user_dictionary_operations SET weight=?4 WHERE dictionary=?1 AND key=?2 AND value=?3 AND operation='upsert' AND user_inserted=0 AND weight_edited=0",
        ) else {
            return false;
        };
        for (kind, key, value, weight) in rebased {
            if update.execute(params![kind, key, value, weight]).is_err() {
                return false;
            }
        }
    }
    if user_db
        .db
        .execute(
            "INSERT INTO dictionary_state(name,value) VALUES(?1,?2) ON CONFLICT(name) DO UPDATE SET value=excluded.value",
            params![REPLAYED_DICTIONARY_STATE, dictionary_identity],
        )
        .is_err()
    {
        return false;
    }
    exec(&user_db.db, "COMMIT")
}

/// 把日志按 `updated_at, rowid` 的次序重放到（新的）词库上。全部成功才提交，否则整批回滚。
pub fn replay(user_db_path: &str, main_db_path: &str, english_db_path: &str) -> ReplayResult {
    let mut result = ReplayResult::default();
    if !Path::new(user_db_path).exists() {
        return result;
    }
    let Some(journal) = open_database(user_db_path, OpenFlags::SQLITE_OPEN_READ_ONLY) else {
        result.error = "cannot open user dictionary database".into();
        return result;
    };
    if !english::ensure_schema(english_db_path) {
        result.error = "cannot migrate English dictionary database".into();
        return result;
    }
    let Some(main_db) = open_database(main_db_path, read_write()) else {
        result.error = "cannot open target dictionary database".into();
        return result;
    };
    if !attach(&main_db, english_db_path, "replay_english") {
        result.error = "cannot attach English dictionary database".into();
        return result;
    }
    // 回放以只读方式打开日志，不做 schema 迁移；没有 user_inserted 列的旧日志里也不会有手动加的词。
    let learned_kana = if has_column(&journal, "user_dictionary_operations", "user_inserted") { format!("({LEARNED_KANA_PINYIN_CONDITION})") } else { "0".to_string() };
    // 旧日志没有 weight_edited 列，设置页改过的权重在那时与自动调频分不开，按自动调频对待。
    let user_owned = format!(
        "{}{}",
        if has_column(&journal, "user_dictionary_operations", "user_inserted") { "user_inserted" } else { "0" },
        if has_column(&journal, "user_dictionary_operations", "weight_edited") { " OR weight_edited" } else { "" }
    );
    // 日志记的是调频写下的绝对权重，按的是当时那份出厂词库的量级。出厂词库没变时原样写回；词库换过（或日志还没记过回放到
    // 哪份词库）时，旧量级的权重会把新词库的词频压下去，这时自动调频的行只在高过新出厂权重时写回，其余保留新权重，并把
    // 日志里的这一行改记成新权重，免得下次回放又写回旧值。用户手动加的词、在设置页亲手改的权重和删除照旧。
    let dictionary_identity = shipped_dictionary_identity(main_db_path);
    let rebase = dictionary_identity.is_some() && replayed_dictionary_identity(&journal) != dictionary_identity;
    let mut rebased: Vec<(String, String, String, i64)> = Vec::new();
    let Ok(mut rows) = journal.prepare(&format!("SELECT dictionary,key,value,operation,weight,display,{learned_kana},({user_owned}) FROM user_dictionary_operations ORDER BY updated_at,rowid")) else {
        result.error = "invalid user dictionary database".into();
        return result;
    };
    if !exec(&main_db, "BEGIN IMMEDIATE") {
        result.error = "cannot start replay transaction".into();
        return result;
    }
    let entries: Result<Vec<_>, _> = {
        let read = rows.query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, String>(2)?, row.get::<_, String>(3)?, row.get::<_, i64>(4)?, row.get::<_, String>(5)?, row.get::<_, i64>(6)? != 0, row.get::<_, i64>(7)? != 0))
        });
        match read {
            Ok(iter) => iter.collect(),
            Err(e) => Err(e),
        }
    };
    let Ok(entries) = entries else {
        result.error = if exec(&main_db, "ROLLBACK") {
            "cannot read the complete user dictionary journal; changes were rolled back"
        } else {
            "cannot read the complete user dictionary journal and rollback also failed"
        }
        .into();
        return result;
    };
    for (kind, key, value, operation, weight, display, learned_kana_pinyin, user_owned_row) in entries {
        if (kind == "pinyin" && operation == "upsert" && weight < 1) || learned_kana_pinyin {
            result.skipped += 1;
            continue;
        }
        if rebase && operation == "upsert" && !user_owned_row && (kind == "pinyin" || kind == "wubi") {
            if let Some(shipped) = shipped_weight(&main_db, &kind, &key, &value) {
                if shipped >= weight {
                    if shipped != weight {
                        rebased.push((kind.clone(), key.clone(), value.clone(), shipped));
                    }
                    result.applied += 1;
                    continue;
                }
            }
        }
        let ok = match kind.as_str() {
            "pinyin" => apply_pinyin(&main_db, &key, &value, &operation, weight),
            "wubi" => apply_simple(&main_db, "wubi86", &key, &value, &operation, weight),
            "quick" => apply_simple(&main_db, "quick_parases", &key, &value, &operation, weight),
            "english" => apply_english(&main_db, &key, &value, &operation, weight, &display),
            _ => false,
        };
        if ok {
            result.applied += 1;
        } else {
            result.failed += 1;
        }
    }
    if result.failed == 0 {
        if !exec(&main_db, "COMMIT") {
            result.error = if exec(&main_db, "ROLLBACK") {
                "cannot commit replay transaction; changes were rolled back"
            } else {
                "cannot commit or roll back replay transaction"
            }
            .into();
        } else if rebase {
            drop(rows);
            drop(journal);
            if let Some(identity) = &dictionary_identity {
                if save_rebased_journal(user_db_path, identity, &rebased) {
                    result.rebased = rebased.len() as i32;
                }
            }
        }
    } else {
        result.error = if exec(&main_db, "ROLLBACK") {
            "one or more operations failed; changes were rolled back"
        } else {
            "one or more operations failed and rollback also failed"
        }
        .into();
    }
    result
}

// ---------------------------------------------------------------------------------------------
// 个人词库（设置页的增删改）
// ---------------------------------------------------------------------------------------------

/// 运行时目录里和用户词库有关的两个：用户数据目录（放日志）和词库目录（放 `msime.db`、`english.db`）。
#[derive(Debug, Clone)]
pub struct RuntimePaths {
    pub user_data: PathBuf,
    pub dictionaries: PathBuf,
}

impl RuntimePaths {
    pub fn journal(&self) -> String {
        self.user_data.join(USER_JOURNAL).to_string_lossy().into_owned()
    }
    pub fn main_dictionary(&self) -> String {
        self.dictionaries.join(MAIN_DICTIONARY).to_string_lossy().into_owned()
    }
    pub fn english_dictionary(&self) -> String {
        self.dictionaries.join(ENGLISH_DICTIONARY).to_string_lossy().into_owned()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PersonalDictionaryKind {
    Pinyin,
    Wubi,
    QuickPhrase,
    English,
}

fn journal_kind(kind: PersonalDictionaryKind) -> DictionaryKind {
    match kind {
        PersonalDictionaryKind::Pinyin => DictionaryKind::Pinyin,
        PersonalDictionaryKind::Wubi => DictionaryKind::Wubi,
        PersonalDictionaryKind::QuickPhrase => DictionaryKind::QuickPhrase,
        PersonalDictionaryKind::English => DictionaryKind::English,
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PersonalDictionaryEntry {
    pub kind: PersonalDictionaryKind,
    pub key: String,
    pub value: String,
    pub weight: i64,
}

/// 校验并规范化一条个人词库条目（键转小写，拼音键的空格换成 `'` 并逐音节核对）。失败返回原因。
pub fn validate_personal_dictionary_entry(mut entry: PersonalDictionaryEntry) -> Result<PersonalDictionaryEntry, &'static str> {
    if !(1..=100_000_000).contains(&entry.weight) {
        return Err("Weight must be between 1 and 100000000");
    }
    if entry.key.is_empty() || entry.key.len() > 512 || entry.value.is_empty() || entry.value.len() > 4096 {
        return Err("A valid, bounded UTF-8 word and input code are required");
    }
    let quick = entry.kind == PersonalDictionaryKind::QuickPhrase;
    for ch in entry.value.bytes() {
        if (ch < 32 && !(quick && (ch == b'\n' || ch == b'\t'))) || ch == 127 {
            return Err("The word contains an unsupported control character");
        }
    }
    entry.key = entry.key.to_ascii_lowercase();
    let letters = |ch: u8| ch.is_ascii_lowercase();
    match entry.kind {
        PersonalDictionaryKind::Pinyin => {
            // 要求写明音节；在这里猜切分可能存下另一个读音。
            entry.key = entry.key.replace(' ', "'");
            let syllables: Vec<&str> = entry.key.split('\'').collect();
            if !syllables.iter().all(|s| crate::syllables::is_intact(s)) {
                return Err("Use complete pinyin syllables separated by apostrophes or spaces");
            }
            if syllables.len() > 64 || syllables.len() != entry.value.chars().count() {
                return Err("Each character must have one pinyin syllable (maximum 64)");
            }
        }
        PersonalDictionaryKind::Wubi => {
            if entry.key.len() > 4 || !entry.key.bytes().all(letters) {
                return Err("Wubi codes contain one to four letters");
            }
        }
        PersonalDictionaryKind::QuickPhrase => {
            if entry.key.len() > 32 || !entry.key.bytes().all(|ch| letters(ch) || ch.is_ascii_digit()) {
                return Err("Quick-phrase codes contain one to 32 letters or digits");
            }
        }
        PersonalDictionaryKind::English => {
            let normalized = entry.value.to_ascii_lowercase();
            if entry.key.len() > 64 || !entry.key.bytes().all(letters) || normalized != entry.key {
                return Err("English code must match the word's letters, ignoring case");
            }
        }
    }
    Ok(entry)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PersonalDictionaryEditResult {
    pub success: bool,
    pub error: String,
}

fn edit_ok() -> PersonalDictionaryEditResult {
    PersonalDictionaryEditResult { success: true, error: String::new() }
}

fn apply_personal_edit(db: &Connection, entry: &PersonalDictionaryEntry, remove: bool) -> bool {
    let operation = if remove { "delete" } else { "upsert" };
    let applied = match entry.kind {
        PersonalDictionaryKind::Pinyin => apply_pinyin(db, &entry.key, &entry.value, operation, entry.weight),
        PersonalDictionaryKind::Wubi => apply_simple(db, "wubi86", &entry.key, &entry.value, operation, entry.weight),
        PersonalDictionaryKind::QuickPhrase => apply_simple(db, "quick_parases", &entry.key, &entry.value, operation, entry.weight),
        PersonalDictionaryKind::English => apply_english(db, &entry.key, &entry.value, operation, entry.weight, &entry.value),
    };
    if !applied {
        return false;
    }
    db.execute(
        "INSERT INTO personal_journal.user_dictionary_operations(dictionary,key,value,operation,weight,display,user_inserted)\
         VALUES(?1,?2,?3,?4,?5,?6,1) ON CONFLICT(dictionary,key,value) DO UPDATE SET operation=excluded.operation,\
         weight=excluded.weight,display=excluded.display,user_inserted=1,updated_at=unixepoch()",
        params![
            kind_name(journal_kind(entry.kind)),
            entry.key,
            entry.value,
            operation,
            if remove { 0 } else { entry.weight },
            if entry.kind == PersonalDictionaryKind::English { entry.value.as_str() } else { "" }
        ],
    )
    .is_ok()
}

fn kind_number(kind: PersonalDictionaryKind) -> i32 {
    match kind {
        PersonalDictionaryKind::Pinyin => 0,
        PersonalDictionaryKind::Wubi => 1,
        PersonalDictionaryKind::QuickPhrase => 2,
        PersonalDictionaryKind::English => 3,
    }
}

/// 增（`previous` 为空）、删（`replacement` 为空）或改一条个人词库条目和它的重放日志，一个附着库事务。
/// 旧条目必须仍是同权重的用户造词，过期的编辑什么都不动。`request_id` 非空时回执和编辑同事务提交：
/// 同一个规范化编辑重试是空操作成功，同一个 ID 换内容失败。
pub fn edit_personal_dictionary(
    paths: &RuntimePaths,
    previous: Option<&PersonalDictionaryEntry>,
    replacement: Option<&PersonalDictionaryEntry>,
    request_id: &str,
) -> PersonalDictionaryEditResult {
    let fail_with = |message: &str| PersonalDictionaryEditResult { success: false, error: message.to_string() };
    if request_id.len() > 128 || !request_id.bytes().all(|ch| ch.is_ascii_alphanumeric() || ch == b'-' || ch == b'_') {
        return fail_with("Invalid personal dictionary request ID");
    }
    if previous.is_none() && replacement.is_none() {
        return fail_with("An entry is required");
    }
    let old_entry = match previous.map(|e| validate_personal_dictionary_entry(e.clone())).transpose() {
        Ok(entry) => entry,
        Err(message) => return fail_with(message),
    };
    let new_entry = match replacement.map(|e| validate_personal_dictionary_entry(e.clone())).transpose() {
        Ok(entry) => entry,
        Err(message) => return fail_with(message),
    };
    let (journal_path, english_path) = (paths.journal(), paths.english_dictionary());
    if !ensure_user_database(&journal_path) || !english::ensure_schema(&english_path) {
        return fail_with("Cannot prepare personal dictionary storage");
    }
    let Some(db) = open_database(&paths.main_dictionary(), read_write()) else { return fail_with("Cannot open dictionary") };
    for (name, path) in [("personal_journal", &journal_path), ("replay_english", &english_path)] {
        if !attach(&db, path, name) {
            return fail_with("Cannot attach personal dictionary storage");
        }
    }
    if !exec(&db, "BEGIN IMMEDIATE") {
        return fail_with("Cannot begin personal dictionary edit");
    }
    let fail = |message: &str| {
        let _ = exec(&db, "ROLLBACK");
        fail_with(message)
    };
    let mut payload = String::new();
    let mut append_payload = |entry: &Option<PersonalDictionaryEntry>| match entry {
        None => payload.push_str("none;"),
        Some(entry) => {
            payload.push_str(&format!("{};{};", kind_number(entry.kind), entry.weight));
            for value in [&entry.key, &entry.value] {
                payload.push_str(&format!("{}:{}", value.len(), value));
            }
        }
    };
    if !request_id.is_empty() {
        append_payload(&old_entry);
        append_payload(&new_entry);
        let stored = db.query_row("SELECT payload FROM personal_journal.personal_dictionary_receipts WHERE request_id=?1", params![request_id], |row| row.get::<_, Option<String>>(0));
        match stored {
            Ok(stored) => {
                if stored.as_deref() != Some(payload.as_str()) {
                    return fail("Request ID was already used for another edit");
                }
                if !exec(&db, "COMMIT") {
                    return fail("Cannot finish edit retry");
                }
                return edit_ok();
            }
            Err(rusqlite::Error::QueryReturnedNoRows) => {}
            Err(_) => return fail("Cannot finish reading edit receipt"),
        }
    }
    if let Some(old) = &old_entry {
        let matched = db.query_row(
            "SELECT 1 FROM personal_journal.user_dictionary_operations WHERE dictionary=?1 AND key=?2 AND value=?3 AND weight=?4 AND operation='upsert' AND user_inserted=1",
            params![kind_name(journal_kind(old.kind)), old.key, old.value, old.weight],
            |_| Ok(()),
        );
        if matched.is_err() {
            return fail("The entry changed; reload it before editing");
        }
        if !apply_personal_edit(&db, old, true) {
            return fail("Cannot remove previous personal entry");
        }
    }
    if let Some(new) = &new_entry {
        if !apply_personal_edit(&db, new, false) {
            return fail("Cannot save personal entry");
        }
    }
    if !request_id.is_empty()
        && db.execute("INSERT INTO personal_journal.personal_dictionary_receipts(request_id,payload) VALUES(?1,?2)", params![request_id, payload]).is_err()
    {
        return fail("Cannot save edit receipt");
    }
    if !exec(&db, "COMMIT") {
        return fail("Cannot commit personal dictionary edit");
    }
    edit_ok()
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PersonalDictionaryPage {
    pub entries: Vec<PersonalDictionaryEntry>,
    pub has_more: bool,
    pub error: String,
}

/// 用户造的词（含自动造的），不含只调过频的和已删的，按种类/键/值稳定排序；`limit` 1..1000，`offset` ≤ 1000000。
pub fn personal_dictionary_entries(paths: &RuntimePaths, offset: usize, limit: usize) -> PersonalDictionaryPage {
    let mut result = PersonalDictionaryPage::default();
    if limit == 0 || limit > 1000 || offset > 1_000_000 {
        result.error = "Invalid personal dictionary page".into();
        return result;
    }
    let journal_path = paths.user_data.join(USER_JOURNAL);
    if !journal_path.exists() {
        return result;
    }
    let db = open_database(&journal_path.to_string_lossy(), OpenFlags::SQLITE_OPEN_READ_ONLY);
    let statement = db.as_ref().and_then(|db| {
        db.prepare(
            "SELECT dictionary,key,value,weight FROM user_dictionary_operations \
             WHERE user_inserted=1 AND operation='upsert' ORDER BY dictionary,key,value LIMIT ?1 OFFSET ?2",
        )
        .ok()
    });
    let Some(mut statement) = statement else {
        result.error = "Cannot read personal dictionary".into();
        return result;
    };
    let Ok(mut rows) = statement.query(params![(limit + 1) as i64, offset as i64]) else {
        result.error = "Cannot read personal dictionary".into();
        return result;
    };
    loop {
        match rows.next() {
            Ok(Some(row)) => {
                if result.entries.len() == limit {
                    result.has_more = true;
                    break;
                }
                let text = |index: usize| row.get::<_, Option<String>>(index).ok().flatten().unwrap_or_default();
                let kind = match text(0).as_str() {
                    "pinyin" => PersonalDictionaryKind::Pinyin,
                    "wubi" => PersonalDictionaryKind::Wubi,
                    "quick" => PersonalDictionaryKind::QuickPhrase,
                    "english" => PersonalDictionaryKind::English,
                    _ => continue,
                };
                result.entries.push(PersonalDictionaryEntry { kind, key: text(1), value: text(2), weight: row.get::<_, i64>(3).unwrap_or(0) });
            }
            Ok(None) => break,
            Err(_) => {
                if !result.has_more {
                    result.entries.clear();
                    result.error = "Cannot finish reading personal dictionary".into();
                }
                break;
            }
        }
    }
    result
}

// ---------------------------------------------------------------------------------------------
// 状态导出
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DictionaryStateEntry {
    pub kind: PersonalDictionaryKind,
    pub key: String,
    pub value: String,
    pub weight: i64,
    pub display: String,
    pub deleted: bool,
    pub user_inserted: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DictionaryStatePosition {
    pub context: String,
    pub key: String,
    pub value: String,
    pub position: i32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DictionaryStateSelection {
    pub context: String,
    pub key: String,
    pub value: String,
    pub count: i32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DictionaryStateRecord {
    Entry(DictionaryStateEntry),
    Position(DictionaryStatePosition),
    Selection(DictionaryStateSelection),
}

/// 在一个一致的读事务里导出日志的全部状态：操作、固定位置、选择计数。不导出回执。出错返回 `Err`。
pub fn dictionary_state_records(paths: &RuntimePaths) -> Result<Vec<DictionaryStateRecord>, String> {
    let file = paths.user_data.join(USER_JOURNAL);
    if !file.exists() {
        return Ok(Vec::new());
    }
    let invalid = || "Invalid or unavailable dictionary state".to_string();
    // 读者也可能需要创建 WAL 协调文件，所以以读写方式打开，再用 query_only 禁止改日志。
    let db = open_database(&file.to_string_lossy(), read_write()).ok_or_else(invalid)?;
    db.execute_batch("PRAGMA query_only=ON").map_err(|_| invalid())?;
    db.execute_batch("BEGIN").map_err(|_| invalid())?;
    let mut records = Vec::new();
    let text = |row: &rusqlite::Row, index: usize| row.get::<_, Option<String>>(index).ok().flatten().ok_or_else(|| "Invalid or unavailable dictionary state".to_string());
    {
        let mut rows = db
            .prepare("SELECT dictionary,key,value,weight,display,operation,user_inserted FROM user_dictionary_operations ORDER BY dictionary,key,value")
            .map_err(|_| invalid())?;
        let mut cursor = rows.query([]).map_err(|_| invalid())?;
        while let Some(row) = cursor.next().map_err(|_| invalid())? {
            let kind = match text(row, 0)?.as_str() {
                "pinyin" => PersonalDictionaryKind::Pinyin,
                "wubi" => PersonalDictionaryKind::Wubi,
                "english" => PersonalDictionaryKind::English,
                "quick" => PersonalDictionaryKind::QuickPhrase,
                _ => return Err("Invalid dictionary kind".into()),
            };
            records.push(DictionaryStateRecord::Entry(DictionaryStateEntry {
                kind,
                key: text(row, 1)?,
                value: text(row, 2)?,
                weight: row.get::<_, i64>(3).map_err(|_| invalid())?,
                display: text(row, 4)?,
                deleted: text(row, 5)? == "delete",
                user_inserted: row.get::<_, i64>(6).map_err(|_| invalid())? != 0,
            }));
        }
    }
    for fixed in [true, false] {
        let sql = if fixed {
            "SELECT context_key,entry_key,value,position FROM fixed_candidate_positions ORDER BY context_key,entry_key,value"
        } else {
            "SELECT context_key,entry_key,value,selection_count FROM candidate_selection_state ORDER BY context_key,entry_key,value"
        };
        let mut rows = db.prepare(sql).map_err(|_| invalid())?;
        let mut cursor = rows.query([]).map_err(|_| invalid())?;
        while let Some(row) = cursor.next().map_err(|_| invalid())? {
            let (context, key, value) = (text(row, 0)?, text(row, 1)?, text(row, 2)?);
            let number = row.get::<_, i32>(3).map_err(|_| invalid())?;
            records.push(if fixed {
                DictionaryStateRecord::Position(DictionaryStatePosition { context, key, value, position: number })
            } else {
                DictionaryStateRecord::Selection(DictionaryStateSelection { context, key, value, count: number })
            });
        }
    }
    db.execute_batch("COMMIT").map_err(|_| invalid())?;
    Ok(records)
}
