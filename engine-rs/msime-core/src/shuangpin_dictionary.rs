//! 双拼词库：把双拼输入串变成候选列表，移植自 `engine/shuangpin/shuangpin_dictionary.cpp`。
//!
//! 搬过来的是数据这一层：按双拼切分查全拼词库（整串、声母、前缀子串）、整句词格并入候选、辅助码筛选与重排、
//! 句中辅助码约束、造词/调频/删词的 SQL 与校验，以及“单字母候选扩到全部”。
//!
//! 没搬的（C++ 里也各自是独立的来源或接线，Rust 这边还没有对应的块）：
//! - Google 解码器整句、神经重排、万象语法搭配——`SentenceAssociation` 里只有词格两个开关；
//! - 领域词库（默认关，开了才会并进候选）；
//! - 按键状态机 `handleVkCode`、直接辅助码的 `resolve_direct_helpcode` 接线、`query_single_char_rows`；
//! - 各种结果缓存（只影响速度；写库后 C++ 会清缓存，这里每次都重新查）。

use crate::dict::{Dictionary, KeyedEntry, QuerySource};
use crate::helpcode::{self, Keymap, SingleMatch};
use crate::lattice::{self, CharConstraint, Item, LanguageModel, Lexeme, Source};
use crate::quanpin_query::{
    cut_pinyin_by_mode, has_only_complete_pinyin_segments, is_complete_pinyin_input, join_segments, segments_to_jianpin, split_segments, CutMode, Segments,
};
use crate::shuangpin::{self, Profile};
use crate::shuangpin_query::{self, SyllableHelpcode};
use crate::table::table_name;
use crate::user_dictionary::{self, DictionaryKind};
use rusqlite::{Connection, OpenFlags};
use std::path::{Path, PathBuf};
use std::time::Duration;

pub const OK: i32 = 0;
pub const ERROR_CODE: i32 = -1;

const INITIAL_CANDIDATE_LIMIT: usize = 24;
/// 词库读连接等一次提交的时间，对应 C++ 的 `kDictionaryBusyTimeoutMs`。
const DICTIONARY_BUSY_TIMEOUT: Duration = Duration::from_millis(250);

/// 整句联想的开关里，这一层认的两个。
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct SentenceAssociation {
    pub word_lattice: bool,
    pub show_next_on_duplicate: bool,
}

pub struct ShuangpinDictionary<'a> {
    profile: &'static Profile,
    db_path: PathBuf,
    journal_path: String,
    dictionary: Option<Dictionary>,
    keymap: Keymap,
    language_model: Option<&'a dyn LanguageModel>,
    association: SentenceAssociation,
    fuzzy_rules: u32,
    syllable_helpcodes: Vec<SyllableHelpcode>,
}

fn remove_delimiters(segmented: &str) -> String {
    segmented.replace('\'', "")
}

fn escape_sql_text(text: &str) -> String {
    text.replace('\'', "''")
}

fn item(pinyin: &str, word: &str, weight: i64, source: Source, canonical: &str) -> Item {
    Item {
        pinyin: pinyin.to_string(),
        canonical_pinyin: canonical.to_string(),
        word: word.to_string(),
        weight,
        source,
        sentence_association: false,
        input_domains: Vec::new(),
    }
}

impl<'a> ShuangpinDictionary<'a> {
    /// `db_path` 是 `msime.db`，`journal_path` 是用户词库日志（`msime_user.db`）。词库打不开时所有查询都返回空。
    pub fn open(profile: &'static Profile, db_path: &Path, journal_path: &str, keymap: Keymap, language_model: Option<&'a dyn LanguageModel>) -> Self {
        let mut dictionary = Self {
            profile,
            db_path: db_path.to_path_buf(),
            journal_path: journal_path.to_string(),
            dictionary: None,
            keymap,
            language_model,
            association: SentenceAssociation::default(),
            fuzzy_rules: 0,
            syllable_helpcodes: Vec::new(),
        };
        dictionary.reopen();
        dictionary
    }

    fn reopen(&mut self) {
        self.dictionary = Dictionary::open(&self.db_path).ok();
    }

    pub fn profile(&self) -> &'static Profile {
        self.profile
    }

    pub fn set_sentence_association(&mut self, association: SentenceAssociation) {
        self.association = association;
    }

    pub fn set_fuzzy_pinyin(&mut self, rules: u32) {
        self.fuzzy_rules = rules;
    }

    pub fn set_syllable_helpcodes(&mut self, helpcodes: Vec<SyllableHelpcode>) {
        self.syllable_helpcodes = helpcodes;
    }

    pub fn set_helpcode_keymap(&mut self, keymap: Keymap) {
        self.keymap = keymap;
    }

    fn quanpin_segmentation(&self, pinyin_segmentation: &str) -> String {
        shuangpin::segmented_to_pinyin(pinyin_segmentation, self.profile)
    }

    // ------------------------------------------------------------------------------------------
    // 查询
    // ------------------------------------------------------------------------------------------

    fn query_from_quanpin_database(&self, pinyin_sequence: &str, pinyin_segmentation: &str) -> Vec<Item> {
        let Some(dictionary) = &self.dictionary else { return Vec::new() };
        if pinyin_segmentation.is_empty() {
            return Vec::new();
        }
        let segments = split_segments(&self.quanpin_segmentation(pinyin_segmentation));
        if segments.is_empty() {
            return Vec::new();
        }
        let rows = dictionary.query_keyed_flat(&segments, i32::MAX as usize, QuerySource::Shuangpin).unwrap_or_default();
        rows.into_iter().map(|row| item(pinyin_sequence, &row.text, row.weight, Source::Database, &row.key)).collect()
    }

    fn query_initial_from_quanpin_database(&self, code: &str, limit: i64) -> Vec<Item> {
        let Some(dictionary) = &self.dictionary else { return Vec::new() };
        if code.len() != 1 || !code.as_bytes()[0].is_ascii_lowercase() {
            return Vec::new();
        }
        let initial = self.quanpin_segmentation(code);
        let rows = dictionary.query_initial(&initial, limit).unwrap_or_default();
        rows.into_iter().map(|row| item(code, &row.text, row.weight, Source::Database, &row.key)).collect()
    }

    pub fn generate(&self, pinyin_sequence: &str, pinyin_segmentation: &str) -> Vec<Item> {
        if pinyin_sequence.is_empty() {
            return Vec::new();
        }
        if pinyin_sequence.len() == 1 {
            return self.query_initial_from_quanpin_database(pinyin_sequence, INITIAL_CANDIDATE_LIMIT as i64);
        }
        self.query_from_quanpin_database(pinyin_sequence, pinyin_segmentation)
    }

    /// 词库里这个键下这个词的权重。
    pub fn find_candidate(&self, key: &str, value: &str) -> Option<Item> {
        let dictionary = self.dictionary.as_ref()?;
        let table = table_name(&split_segments(key))?;
        let sql = format!("SELECT weight FROM \"{table}\" WHERE key=?1 AND value=?2 LIMIT 1");
        let weight: i64 = dictionary.connection().query_row(&sql, rusqlite::params![key, value], |row| row.get(0)).ok()?;
        Some(item(key, value, weight, Source::Database, key))
    }

    /// 单字母输入只取了 24 条时，把它们换成全部（并回原来的位置）。返回是否扩展了。
    pub fn expand_initial_candidates(&self, code: &str, candidates: &mut Vec<Item>) -> bool {
        if code.len() != 1 {
            return false;
        }
        let is_limited_initial = |item: &Item| item.source == Source::Database && item.pinyin == code;
        let limited_count = candidates.iter().filter(|item| is_limited_initial(item)).count();
        if limited_count != INITIAL_CANDIDATE_LIMIT {
            return false;
        }
        let expanded = self.query_initial_from_quanpin_database(code, i64::from(i32::MAX));
        if expanded.len() <= limited_count {
            return false;
        }
        let mut merged: Vec<Item> = Vec::with_capacity(candidates.len() - limited_count + expanded.len());
        let mut inserted = false;
        for candidate in candidates.drain(..) {
            if is_limited_initial(&candidate) {
                if !inserted {
                    merged.extend(expanded.iter().cloned());
                    inserted = true;
                }
                continue;
            }
            merged.push(candidate);
        }
        *candidates = merged;
        true
    }

    /// 词格的跨度查询：词库行加上模糊音变体，按折后权重稳定排序，截到 `limit`。
    fn lattice_lookup(&self, span: &[String], limit: i32) -> Vec<Lexeme> {
        let Some(dictionary) = &self.dictionary else { return Vec::new() };
        let mut rows = dictionary.lattice_lookup(span, limit, self.fuzzy_rules).unwrap_or_default();
        rows.sort_by(|a, b| {
            let lhs = a.weight as f64 * 10f64.powf(-a.penalty);
            let rhs = b.weight as f64 * 10f64.powf(-b.penalty);
            rhs.partial_cmp(&lhs).unwrap_or(std::cmp::Ordering::Equal)
        });
        rows.truncate(limit.max(0) as usize);
        rows
    }

    fn accepts_syllable_char(&self, helpcode: &SyllableHelpcode, hanzi: &str) -> bool {
        self.keymap.get(hanzi).is_some_and(|code| helpcode.matches(code))
    }

    /// 双拼串对应的所有候选：整串精确命中的词在前，再是各个前缀子串的词，最后并进词格整句。
    pub fn generate_series(&self, pinyin_sequence: &str, pinyin_segmentation: &str) -> Vec<Item> {
        let mut candidates: Vec<Item> = Vec::new();
        if pinyin_sequence.is_empty() {
            return candidates;
        }
        if pinyin_sequence.len() == 1 {
            return self.query_initial_from_quanpin_database(pinyin_sequence, INITIAL_CANDIDATE_LIMIT as i64);
        }

        // 严格对应当前拼音的数据。（词库查不到时 C++ 在 Google 联想开着时走一条整句兜底，这里没有 Google 解码器。）
        candidates.extend(self.generate(pinyin_sequence, pinyin_segmentation));

        // 前缀子串，给接下来可能的造词用。
        let mut seg_pinyin = pinyin_segmentation.to_string();
        while let Some(pos) = seg_pinyin.rfind('\'') {
            seg_pinyin.truncate(pos);
            let pure_pinyin = remove_delimiters(&seg_pinyin);
            candidates.extend(self.generate(&pure_pinyin, &seg_pinyin));
        }

        let quanpin_segmentation = self.quanpin_segmentation(pinyin_segmentation);
        let quanpin_syllables = split_segments(&quanpin_segmentation);

        // 词格：开了显示开关才并进候选。
        if self.association.word_lattice {
            let needs_alternatives = self.association.word_lattice && self.association.show_next_on_duplicate;
            let accepts: Vec<Box<dyn Fn(&str) -> bool + '_>> = self
                .syllable_helpcodes
                .iter()
                .map(|helpcode| {
                    let helpcode = *helpcode;
                    Box::new(move |hanzi: &str| self.accepts_syllable_char(&helpcode, hanzi)) as Box<dyn Fn(&str) -> bool + '_>
                })
                .collect();
            let mut options = lattice::Options {
                nbest: if needs_alternatives { 12 } else { 1 },
                include_lattice_best: self.association.word_lattice,
                show_next_on_duplicate: self.association.show_next_on_duplicate,
                language_model: self.language_model,
                ..lattice::Options::default()
            };
            for (helpcode, accept) in self.syllable_helpcodes.iter().zip(&accepts) {
                options.char_constraints.push(CharConstraint { syllable: helpcode.syllable, accept: accept.as_ref() });
            }
            // 受约束的跨度不截断地查（生僻字才筛得出来）。
            let constrained = |span: &[String]| self.lattice_lookup(span, 4096);
            if !options.char_constraints.is_empty() {
                options.constrained_lookup = Some(&constrained);
            }
            let plain = |span: &[String]| self.lattice_lookup(span, options.span_limit);
            lattice::merge_lattice_candidates(&mut candidates, &quanpin_syllables, &plain, pinyin_sequence, &options, &[]);
        }
        candidates
    }

    // ------------------------------------------------------------------------------------------
    // 辅助码
    // ------------------------------------------------------------------------------------------

    fn filter_with_single_helpcode(&self, candidates: &[Item], result: &mut Vec<Item>, help_code: &str, pinyin_sequence: &str) {
        if candidates.is_empty() || help_code.len() != 1 {
            return;
        }
        let prefer_last = help_code.as_bytes()[0].is_ascii_uppercase();
        let normalized = help_code.to_ascii_lowercase();
        let (mut first, mut last, mut left): (Vec<Item>, Vec<Item>, Vec<Item>) = (Vec::new(), Vec::new(), Vec::new());
        for candidate in candidates {
            match helpcode::match_single_helpcode(&candidate.word, &normalized, &self.keymap) {
                SingleMatch::First => first.push(candidate.clone()),
                SingleMatch::Last => last.push(candidate.clone()),
                SingleMatch::Both => (if prefer_last { &mut last } else { &mut first }).push(candidate.clone()),
                SingleMatch::None => left.push(candidate.clone()),
            }
        }
        if prefer_last {
            result.extend(last);
            result.extend(first);
        } else {
            result.extend(first);
            result.extend(last);
        }
        // 把原始拼音的候选放在辅助码筛出来的后面，再是没筛中的。
        let original_segmentation = shuangpin::segment(pinyin_sequence, self.profile);
        result.extend(self.generate_series(pinyin_sequence, &original_segmentation));
        result.extend(left);
    }

    fn filter_with_double_helpcodes(&self, candidates: &[Item], result: &mut Vec<Item>, help_codes: &str) {
        for candidate in candidates {
            if helpcode::matches_double_helpcodes(&candidate.word, help_codes, &self.keymap) {
                result.push(candidate.clone());
            }
        }
    }

    /// 辅助码筛选。`help_codes` 一位是单码（大写表示优先匹配末字），两位是双码。
    pub fn generate_with_helpcodes(&self, pure_pinyin: &str, pure_pinyin_segmentation: &str, pinyin_sequence: &str, help_codes: &str) -> Vec<Item> {
        let candidates = self.generate_series(pure_pinyin, pure_pinyin_segmentation);
        let mut result = Vec::new();
        match help_codes.len() {
            1 => self.filter_with_single_helpcode(&candidates, &mut result, help_codes, pinyin_sequence),
            2 => self.filter_with_double_helpcodes(&candidates, &mut result, help_codes),
            _ => {}
        }
        result
    }

    fn satisfies_syllable_helpcodes(&self, word: &str) -> bool {
        if self.syllable_helpcodes.is_empty() {
            return true;
        }
        let chars: Vec<String> = word.chars().map(|c| c.to_string()).collect();
        self.syllable_helpcodes.iter().all(|helpcode| helpcode.syllable >= chars.len() || self.accepts_syllable_char(helpcode, &chars[helpcode.syllable]))
    }

    /// 候选覆盖到受约束的音节时，那个位置上的字必须满足约束，不满足的去掉。
    pub fn filter_by_syllable_helpcodes(&self, candidates: &mut Vec<Item>) {
        if self.syllable_helpcodes.is_empty() {
            return;
        }
        let is_pinyin_hanzi = |item: &Item| match item.source {
            Source::Database
            | Source::UserDatabase
            | Source::Generated
            | Source::Fallback
            | Source::NeuralDesktop
            | Source::NeuralKeyboard
            | Source::CloudSuggestion
            | Source::AiSuggestion => helpcode::count_han_chars(&item.word) == helpcode::count_utf8_chars(&item.word),
            _ => false,
        };
        candidates.retain(|item| !(is_pinyin_hanzi(item) && !self.satisfies_syllable_helpcodes(&item.word)));
    }

    // ------------------------------------------------------------------------------------------
    // 造词、调频、删词
    // ------------------------------------------------------------------------------------------

    fn normalize_shuangpin_to_quanpin_input(&self, pinyin: &str) -> String {
        if pinyin.is_empty() {
            return String::new();
        }
        remove_delimiters(&shuangpin_query::normalize_input_with_delimiters(pinyin, self.profile))
    }

    /// 造词时按双拼串取已有的词：每个前缀各查一次，长的在前。
    pub fn build_quanpin_sql_for_creating_word(&self, pinyin: &str) -> String {
        let normalized = self.normalize_shuangpin_to_quanpin_input(pinyin);
        let cuts = cut_pinyin_by_mode(&normalized, CutMode::Correction);
        let Some(cut) = cuts.first() else { return String::new() };
        let mut sql = String::new();
        for i in 1..=cut.len() {
            let partial = &cut[..i];
            let key = join_segments(partial);
            let table = table_name(partial).unwrap_or_default();
            let each = format!("select * from(select * from {table} where key = '{key}' order by weight desc)");
            sql = if sql.is_empty() { each } else { format!("{each} union all {sql}") };
        }
        sql
    }

    pub fn generate_for_creating_word(&self, code: &str) -> Vec<Item> {
        let sql = self.build_quanpin_sql_for_creating_word(code);
        let Some(dictionary) = &self.dictionary else { return Vec::new() };
        let Ok(mut statement) = dictionary.connection().prepare(&sql) else { return Vec::new() };
        let Ok(rows) = statement.query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(2)?, row.get::<_, i64>(3)?))) else {
            return Vec::new();
        };
        rows.filter_map(Result::ok).map(|(key, value, weight)| item(&key, &value, weight, Source::Database, &key)).collect()
    }

    fn build_quanpin_sql_for_checking_word(&self, key: &str, value: &str) -> String {
        let cuts = cut_pinyin_by_mode(key, CutMode::Correction);
        let Some(cut) = cuts.first() else { return String::new() };
        let table = table_name(cut).unwrap_or_default();
        format!("select 1 from {table} where key = '{}' and value = '{}';", escape_sql_text(key), escape_sql_text(value))
    }

    fn build_quanpin_sql_for_inserting_word(&self, key: &str, jp: &str, value: &str) -> String {
        let cuts = cut_pinyin_by_mode(key, CutMode::Correction);
        let Some(cut) = cuts.first() else { return String::new() };
        let table = table_name(cut).unwrap_or_default();
        format!(
            "insert into {table} (key, jp, value, weight) values ('{}', '{}', '{}', '{}');",
            escape_sql_text(key),
            escape_sql_text(jp),
            escape_sql_text(value),
            10000
        )
    }

    /// 调用方约定传入 canonical 全拼；这里不能再当双拼归一化。
    pub fn build_quanpin_sql_for_updating_word(&self, pinyin: &str, word: &str) -> String {
        let cuts = cut_pinyin_by_mode(&remove_delimiters(pinyin), CutMode::Correction);
        let Some(cut) = cuts.first() else { return String::new() };
        let han_count = helpcode::count_han_chars(word);
        let mut segments: Segments = cut.clone();
        if segments.len() > han_count {
            segments.truncate(han_count);
        }
        let pinyin = join_segments(&segments);
        let jp = segments_to_jianpin(&segments);
        if !self.do_validate(&pinyin, &jp, word) {
            return String::new();
        }
        let table = table_name(&segments).unwrap_or_default();
        format!(
            "update {table} set weight = ( select MAX(weight) + 1 from {table} AS sub where sub.key = '{0}') where key = '{0}' and value = '{1}';",
            escape_sql_text(&pinyin),
            escape_sql_text(word)
        )
    }

    fn do_validate(&self, key: &str, jp: &str, value: &str) -> bool {
        let pure_key = remove_delimiters(key);
        if pure_key.is_empty() {
            return false;
        }
        let han_count = helpcode::count_han_chars(value);
        if jp.len() != han_count {
            return false;
        }
        let cuts = cut_pinyin_by_mode(&pure_key, CutMode::Correction);
        if let Some(cut) = cuts.first() {
            return cut.len() == han_count;
        }
        pure_key.len() % 2 == 0 && pure_key.len() == han_count * 2
    }

    fn writable(&self) -> Option<Connection> {
        let connection = Connection::open_with_flags(&self.db_path, OpenFlags::SQLITE_OPEN_READ_WRITE).ok()?;
        connection.busy_timeout(DICTIONARY_BUSY_TIMEOUT).ok()?;
        Some(connection)
    }

    /// 执行一条（或空的）语句：空串什么也不做，和 `sqlite3_exec("")` 一样算成功。
    fn exec(&self, sql: &str) -> i32 {
        let Some(connection) = self.writable() else { return ERROR_CODE };
        if connection.execute_batch(sql).is_err() {
            return ERROR_CODE;
        }
        OK
    }

    fn row_exists(&self, sql: &str) -> bool {
        let Some(dictionary) = &self.dictionary else { return false };
        let Ok(mut statement) = dictionary.connection().prepare(sql) else { return false };
        statement.exists([]).unwrap_or(false)
    }

    pub fn create_word(&mut self, pinyin: &str, word: &str) -> i32 {
        let normalized = shuangpin_query::normalize_input_with_delimiters(pinyin, self.profile);
        self.create_word_from_quanpin(&normalized, word)
    }

    pub fn create_word_from_quanpin(&mut self, pinyin: &str, word: &str) -> i32 {
        let segments = split_segments(pinyin);
        let han_count = helpcode::count_han_chars(word);
        if segments.is_empty() || segments.len() != han_count || segments.iter().any(|segment| segment.is_empty() || !is_complete_pinyin_input(segment)) {
            return ERROR_CODE;
        }
        let pinyin = join_segments(&segments);
        let jp = segments_to_jianpin(&segments);
        if !self.do_validate(&pinyin, &jp, word) {
            return ERROR_CODE;
        }
        if self.row_exists(&self.build_quanpin_sql_for_checking_word(&pinyin, word)) {
            return OK;
        }
        if self.exec(&self.build_quanpin_sql_for_inserting_word(&pinyin, &jp, word)) != OK {
            return ERROR_CODE;
        }
        let _ = user_dictionary::record_user_insert(&self.journal_path, DictionaryKind::Pinyin, &pinyin, word, 10000, "");
        self.reopen();
        OK
    }

    /// 把词的权重提到这个读音下的最高 + 1。`pinyin` 可以是双拼原串，也可以是全拼。
    pub fn update_weight_by_pinyin_and_word(&mut self, pinyin: &str, word: &str) -> i32 {
        let mut pinyin = pinyin.to_string();
        let direct_cuts = cut_pinyin_by_mode(&remove_delimiters(&pinyin), CutMode::Correction);
        // 只有“完整合法拼读”的直切才可信，否则当双拼原串归一化。
        if direct_cuts.is_empty()
            || !has_only_complete_pinyin_segments(&direct_cuts[0])
            || remove_delimiters(&join_segments(&direct_cuts[0])) != remove_delimiters(&pinyin)
        {
            pinyin = self.normalize_shuangpin_to_quanpin_input(&pinyin);
        }
        let cuts = cut_pinyin_by_mode(&remove_delimiters(&pinyin), CutMode::Correction);
        let Some(cut) = cuts.first() else { return ERROR_CODE };
        let mut segments = cut.clone();
        let han_count = helpcode::count_han_chars(word);
        if segments.len() > han_count {
            segments.truncate(han_count);
        }
        let normalized = join_segments(&segments);
        if self.exec(&self.build_quanpin_sql_for_updating_word(&normalized, word)) != OK {
            return ERROR_CODE;
        }
        let _ = user_dictionary::record_pinyin_upsert_from_database(&self.db_path.to_string_lossy(), &normalized, word, &self.journal_path);
        self.reopen();
        OK
    }

    pub fn delete_by_pinyin_and_word(&mut self, pinyin: &str, word: &str) -> i32 {
        let mut pinyin = pinyin.to_string();
        let direct_cuts = cut_pinyin_by_mode(&remove_delimiters(&pinyin), CutMode::Correction);
        let normalized_shuangpin = self.normalize_shuangpin_to_quanpin_input(&pinyin);
        let shuangpin_cuts = cut_pinyin_by_mode(&normalized_shuangpin, CutMode::Correction);
        let han_count = helpcode::count_han_chars(word);
        let direct_key_matches_word = direct_cuts.first().is_some_and(|cut| cut.len() == han_count);
        let shuangpin_key_matches_word = shuangpin_cuts.first().is_some_and(|cut| cut.len() == han_count);
        if (!direct_key_matches_word && shuangpin_key_matches_word)
            || direct_cuts.is_empty()
            || remove_delimiters(&join_segments(&direct_cuts[0])) != remove_delimiters(&pinyin)
        {
            pinyin = normalized_shuangpin;
        }
        let cuts = cut_pinyin_by_mode(&remove_delimiters(&pinyin), CutMode::Correction);
        let Some(cut) = cuts.first() else { return ERROR_CODE };
        let normalized = join_segments(cut);
        if !user_dictionary::delete_dictionary_candidate(&self.db_path.to_string_lossy(), &self.journal_path, DictionaryKind::Pinyin, &normalized, word) {
            return ERROR_CODE;
        }
        self.reopen();
        OK
    }
}

/// 词库行转候选的小助手，给调用方和测试用。
pub fn keyed_to_item(pinyin: &str, row: &KeyedEntry) -> Item {
    item(pinyin, &row.text, row.weight, Source::Database, &row.key)
}
