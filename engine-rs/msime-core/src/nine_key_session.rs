//! 九键拼音输入：数字键的歧义组合，对应 `engine/core/nine_key_session.cpp`。
//!
//! 平台只发数字键、选候选、选拼写；这里自己维护数字串、已锁定的音节和候选。候选来自全拼词库对每条
//! “数字 → 音节路径”的查询，再按读音长度、是否模糊、权重排序。

use crate::helpcode;
use crate::input_session::{Command, FrequencyAdjustmentMode, FrequencyAdjustmentOptions, KeyResult};
use crate::lattice::{Item, LanguageModel, Source};
use crate::quanpin_dictionary::QuanpinDictionary;
use crate::quanpin_query::join_segments;
use crate::runtime_paths::RuntimeRoots;
use crate::syllables::{is_intact, INTACT};
use crate::user_dictionary::{self, DictionaryKind, WordItem as UserItem};
use std::collections::HashSet;
use std::path::Path;
use std::sync::OnceLock;

const PATH_LIMIT: usize = 48;
const DIGIT_LIMIT: usize = 32;

fn encode(pinyin: &str) -> String {
    const KEYS: &[u8] = b"22233344455566677778889999";
    pinyin.bytes().filter(|c| c.is_ascii_lowercase()).map(|c| KEYS[(c - b'a') as usize] as char).collect()
}

fn starts(text: &str, prefix: &str) -> bool {
    text.starts_with(prefix)
}

struct Spelling {
    text: String,
    digits: String,
}

/// 完整音节的所有前缀（含不完整的末音节），只收真实音节的前缀，不凭空造字母组合。
fn spellings() -> &'static [Spelling] {
    static VALUES: OnceLock<Vec<Spelling>> = OnceLock::new();
    VALUES.get_or_init(|| {
        let mut result = Vec::new();
        let mut seen = HashSet::new();
        for syllable in INTACT {
            for n in 1..=syllable.len() {
                let prefix = &syllable[..n];
                if seen.insert(prefix.to_string()) {
                    result.push(Spelling { text: prefix.to_string(), digits: encode(prefix) });
                }
            }
        }
        result
    })
}

type Path_ = Vec<String>;

fn paths(digits: &str) -> Vec<Path_> {
    let mut suffix: Vec<Vec<Path_>> = vec![Vec::new(); digits.len() + 1];
    suffix[digits.len()].push(Vec::new());
    for offset in (0..digits.len()).rev() {
        let mut result: Vec<Path_> = Vec::new();
        for spelling in spellings() {
            let end = offset + spelling.digits.len();
            if end > digits.len() || !digits[offset..].starts_with(&spelling.digits) {
                continue;
            }
            if end != digits.len() && !is_intact(&spelling.text) {
                continue;
            }
            for tail in &suffix[end] {
                let mut path = vec![spelling.text.clone()];
                path.extend(tail.iter().cloned());
                result.push(path);
            }
        }
        result.sort_by(|a, b| {
            if a.len() != b.len() {
                return a.len().cmp(&b.len());
            }
            let a_complete = is_intact(a.last().unwrap());
            let b_complete = is_intact(b.last().unwrap());
            match (a_complete, b_complete) {
                (true, false) => std::cmp::Ordering::Less,
                (false, true) => std::cmp::Ordering::Greater,
                _ => a.cmp(b),
            }
        });
        result.truncate(PATH_LIMIT);
        suffix[offset] = result;
    }
    suffix.swap_remove(0)
}

/// 平台能读到的九键状态。
#[derive(Debug, Clone, Default)]
pub struct NineKeySnapshot {
    pub preedit: String,
    pub editing_text: String,
    pub caret_position: usize,
    pub candidates: Vec<Item>,
    pub spellings: Vec<String>,
}

fn to_user_items(items: &[Item]) -> Vec<UserItem> {
    items
        .iter()
        .map(|item| UserItem {
            pinyin: item.pinyin.clone(),
            canonical_pinyin: item.canonical_pinyin.clone(),
            word: item.word.clone(),
            weight: item.weight,
            source: item.source,
            fixed_position: item.fixed_position,
        })
        .collect()
}

pub struct NineKeySession<'a> {
    roots: RuntimeRoots,
    learning: bool,
    frequency: FrequencyAdjustmentOptions,
    fuzzy_rules: u32,
    language_model: Option<&'a dyn LanguageModel>,
    dictionary: Option<QuanpinDictionary<'a>>,
    digits: String,
    locked: Vec<String>,
    spellings: Vec<String>,
    candidates: Vec<Item>,
}

impl<'a> NineKeySession<'a> {
    pub fn new(roots: RuntimeRoots, learning: bool, frequency: FrequencyAdjustmentOptions, fuzzy_rules: u32, language_model: Option<&'a dyn LanguageModel>) -> Self {
        Self { roots, learning, frequency, fuzzy_rules, language_model, dictionary: None, digits: String::new(), locked: Vec::new(), spellings: Vec::new(), candidates: Vec::new() }
    }

    pub fn active(&self) -> bool {
        !self.digits.is_empty()
    }

    fn main_dictionary_path(&self) -> String {
        self.roots.dictionaries.join("msime.db").to_string_lossy().into_owned()
    }

    fn journal_path(&self) -> String {
        self.roots.user_data.join("msime_user.db").to_string_lossy().into_owned()
    }

    fn locked_length(&self) -> usize {
        self.locked.iter().map(String::len).sum()
    }

    fn ranking_context(&self) -> String {
        format!("nine-key:{}:{}", self.digits, join_segments(&self.locked))
    }

    fn editable(&self, index: usize) -> bool {
        self.candidates.get(index).is_some_and(|item| !item.canonical_pinyin.is_empty() && matches!(item.source, Source::Database | Source::UserDatabase))
    }

    fn refresh(&mut self) {
        self.candidates.clear();
        self.spellings.clear();
        if !self.active() {
            return;
        }
        if self.dictionary.is_none() {
            self.dictionary = Some(QuanpinDictionary::open(Path::new(&self.main_dictionary_path()), &self.journal_path(), self.language_model));
        }
        let locked_length = self.locked_length();
        let remaining = self.digits[locked_length..].to_string();
        for syllable in INTACT {
            let code = encode(syllable);
            if !remaining.is_empty() && locked_length + remaining.len().max(code.len()) <= DIGIT_LIMIT && (starts(&remaining, &code) || starts(&code, &remaining)) {
                self.spellings.push(syllable.to_string());
            }
        }
        let remaining_len = remaining.len();
        self.spellings.sort_by(|a, b| {
            let (al, bl) = (a.len().min(remaining_len), b.len().min(remaining_len));
            if al != bl {
                bl.cmp(&al)
            } else {
                a.cmp(b)
            }
        });
        let mut alternatives = paths(&remaining);
        // 尾巴没写完或不成音节时，也要让开头那个音节能被选走。
        for spelling in &self.spellings {
            if spelling.len() <= remaining_len {
                alternatives.push(vec![spelling.clone()]);
            }
        }
        if remaining.is_empty() {
            alternatives = vec![Vec::new()];
        }
        let locked_key = join_segments(&self.locked);
        let mut queried: HashSet<String> = HashSet::new();
        for mut path in alternatives {
            for (at, part) in self.locked.iter().enumerate() {
                path.insert(at, part.clone());
            }
            let key = join_segments(&path);
            if key.is_empty() || !queried.insert(key.clone()) {
                continue;
            }
            let found = self.dictionary.as_mut().unwrap().query(&key, &key, 0, self.fuzzy_rules);
            for mut candidate in found {
                let canonical = if candidate.canonical_pinyin.is_empty() { candidate.pinyin.clone() } else { candidate.canonical_pinyin.clone() };
                if canonical.matches('\'').count() >= path.len() {
                    continue;
                }
                let matched = if candidate.fuzzy { candidate.pinyin.clone() } else { canonical.clone() };
                let code = encode(&matched);
                if code.is_empty() || (!starts(&code, &self.digits) && !starts(&self.digits, &code)) {
                    continue;
                }
                if !locked_key.is_empty() && matched != locked_key && !starts(&matched, &format!("{locked_key}'")) && !starts(&locked_key, &format!("{matched}'")) {
                    continue;
                }
                candidate.pinyin = self.digits[..code.len().min(self.digits.len())].to_string();
                candidate.canonical_pinyin = canonical;
                self.candidates.push(candidate);
            }
        }
        self.candidates.sort_by(|a, b| {
            if a.pinyin.len() != b.pinyin.len() {
                return b.pinyin.len().cmp(&a.pinyin.len());
            }
            if a.fuzzy != b.fuzzy {
                return a.fuzzy.cmp(&b.fuzzy);
            }
            b.weight.cmp(&a.weight)
        });
        let mut seen: HashSet<String> = HashSet::new();
        self.candidates.retain(|item| seen.insert(item.word.clone()));
        self.candidates.truncate(128);
        user_dictionary::apply_fixed_positions(&self.journal_path(), &self.ranking_context(), &mut self.candidates, false, None, false);
        // 最可能的读音放在最前，免得要横向滚动才看得到。
        if let Some(first) = self.candidates.first() {
            let canonical = first.canonical_pinyin.clone();
            let offset = if locked_key.is_empty() { 0 } else { locked_key.len() + 1 };
            if offset < canonical.len() {
                let end = canonical[offset..].find('\'').map_or(canonical.len(), |at| offset + at);
                let preferred = &canonical[offset..end];
                if let Some(found) = self.spellings.iter().position(|spelling| spelling == preferred) {
                    self.spellings[..=found].rotate_right(1);
                }
            }
        }
    }

    pub fn character(&mut self, digit: char) -> KeyResult {
        if !('2'..='9').contains(&digit) {
            return KeyResult::default();
        }
        if self.digits.len() >= DIGIT_LIMIT {
            return KeyResult::with(true, None, Some("九键最多输入 32 位，请先选择候选".to_string()));
        }
        self.digits.push(digit);
        self.refresh();
        KeyResult::handled()
    }

    pub fn select(&mut self, index: usize) -> KeyResult {
        if index >= self.candidates.len() {
            return KeyResult::default();
        }
        let selected = self.candidates[index].clone();
        let mut diagnostic = None;
        if self.learning && self.frequency.mode != FrequencyAdjustmentMode::Disabled && index != 0 && selected.fixed_position == 0 && self.editable(index) {
            diagnostic = self.adjust_frequency(index, false);
        }
        self.digits.drain(..selected.pinyin.len().min(self.digits.len()));
        let mut consumed = selected.pinyin.len();
        while !self.locked.is_empty() && consumed >= self.locked[0].len() {
            consumed -= self.locked[0].len();
            self.locked.remove(0);
        }
        self.refresh();
        KeyResult::with(true, Some(selected.word), diagnostic)
    }

    pub fn finish(&mut self, mut index: usize) -> KeyResult {
        if !self.active() {
            return KeyResult::default();
        }
        if index >= self.candidates.len() {
            return self.command(Command::CommitRaw);
        }
        let mut commit = String::new();
        let mut diagnostic: Option<String> = None;
        while self.active() {
            if self.candidates.is_empty() {
                commit.push_str(&self.digits);
                break;
            }
            let result = self.select(index);
            if diagnostic.is_none() {
                diagnostic = result.diagnostic;
            }
            if let Some(text) = result.commit {
                commit.push_str(&text);
            }
            index = 0;
        }
        self.command(Command::Cancel);
        KeyResult::with(true, Some(commit), diagnostic)
    }

    fn adjust_frequency(&mut self, index: usize, force_top: bool) -> Option<String> {
        let item = self.candidates[index].clone();
        let mut changed = false;
        let mode = if force_top { "pin" } else { self.frequency.mode.name() };
        let adjusted = user_dictionary::adjust_candidate_ranking(
            &self.main_dictionary_path(),
            &self.journal_path(),
            &self.ranking_context(),
            &to_user_items(&self.candidates),
            &item.canonical_pinyin,
            &item.word,
            mode,
            self.frequency.linear_step,
            self.frequency.trigger_count,
            force_top,
            Some(&mut changed),
            DictionaryKind::Pinyin,
        );
        if !adjusted {
            return Some("Unable to persist nine-key candidate frequency adjustment.".to_string());
        }
        if changed {
            if let Some(dictionary) = &self.dictionary {
                dictionary.reset_cache();
            }
        }
        None
    }

    pub fn pin(&mut self, index: usize) -> KeyResult {
        if !self.editable(index) {
            return KeyResult::default();
        }
        let diagnostic = self.adjust_frequency(index, true);
        self.refresh();
        KeyResult::with(true, None, diagnostic)
    }

    pub fn remove(&mut self, index: usize) -> KeyResult {
        if !self.editable(index) || helpcode::count_utf8_chars(&self.candidates[index].word) <= 1 {
            return KeyResult::default();
        }
        let item = self.candidates[index].clone();
        if !user_dictionary::delete_dictionary_candidate(&self.main_dictionary_path(), &self.journal_path(), DictionaryKind::Pinyin, &item.canonical_pinyin, &item.word) {
            return KeyResult::with(true, None, Some("Unable to persist nine-key candidate removal.".to_string()));
        }
        if let Some(dictionary) = &self.dictionary {
            dictionary.reset_cache();
        }
        self.refresh();
        KeyResult::handled()
    }

    pub fn set_position(&mut self, index: usize, position: i32) -> KeyResult {
        if !self.editable(index) || !(0..=5).contains(&position) {
            return KeyResult::default();
        }
        let item = &self.candidates[index];
        let journal = self.journal_path();
        let saved = if position == 0 {
            user_dictionary::clear_fixed_position(&journal, &self.ranking_context(), &item.canonical_pinyin, &item.word)
        } else {
            user_dictionary::set_fixed_position(&journal, &self.ranking_context(), &item.canonical_pinyin, &item.word, position)
        };
        if !saved {
            return KeyResult::with(true, None, Some("Unable to persist nine-key candidate position.".to_string()));
        }
        self.refresh();
        KeyResult::handled()
    }

    pub fn command(&mut self, command: Command) -> KeyResult {
        if !self.active() {
            return KeyResult::default();
        }
        match command {
            Command::CommitCandidate => return self.select(0),
            Command::CommitRaw => {
                let raw = self.digits.clone();
                self.command(Command::Cancel);
                return KeyResult::with(true, Some(raw), None);
            }
            Command::Cancel => {
                self.digits.clear();
                self.locked.clear();
            }
            Command::Backspace => {
                self.digits.pop();
                while self.locked_length() > self.digits.len() {
                    self.locked.pop();
                }
            }
            _ => return KeyResult::default(),
        }
        self.refresh();
        KeyResult::handled()
    }

    pub fn choose_spelling(&mut self, index: usize) -> KeyResult {
        if index >= self.spellings.len() {
            return KeyResult::default();
        }
        let spelling = self.spellings[index].clone();
        let offset = self.locked_length();
        let length = spelling.len().min(self.digits.len() - offset);
        self.digits.replace_range(offset..offset + length, &encode(&spelling));
        self.locked.push(spelling);
        self.refresh();
        KeyResult::handled()
    }

    pub fn snapshot(&self) -> NineKeySnapshot {
        let mut preedit = join_segments(&self.locked);
        let locked_length = self.locked_length();
        if self.active() && locked_length < self.digits.len() {
            if !preedit.is_empty() {
                preedit.push('\'');
            }
            preedit.push_str(&self.digits[locked_length..]);
        }
        NineKeySnapshot { preedit, editing_text: self.digits.clone(), caret_position: self.digits.len(), candidates: self.candidates.clone(), spellings: self.spellings.clone() }
    }
}
