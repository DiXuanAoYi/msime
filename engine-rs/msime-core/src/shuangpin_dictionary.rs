//! 双拼词库：把双拼输入串变成候选列表，移植自 `engine/shuangpin/shuangpin_dictionary.cpp`。
//!
//! 搬过来的是数据这一层：按双拼切分查全拼词库（整串、声母、前缀子串）、整句词格并入候选、辅助码筛选与重排、
//! 句中辅助码约束、造词/调频/删词的 SQL 与校验，以及“单字母候选扩到全部”。
//!
//! 整句来源：词格、Google 解码器（兜底整句与独立整句行，句中辅助码约束随解码）、两个神经模型的重排、万象语法搭配，
//! 由 `SentenceAssociation` 开关、`with_sentence_sources` 提供模型和解码器。
//!
//! 没搬的（C++ 里也各自是独立的来源或接线，Rust 这边还没有对应的块）：
//! - 按键状态机 `handleVkCode`、直接辅助码的 `resolve_direct_helpcode` 接线、`query_single_char_rows`；
//! - 各种结果缓存（只影响速度；写库后 C++ 会清缓存，这里每次都重新查）。

use crate::cache::{insert_dynamic_candidate, Cache};
use crate::dict::{Dictionary, KeyedEntry, QuerySource};
use crate::direct_lattice::DecodeOptions;
use crate::direct_resolver::{ResolveContext, Resolver};
use crate::request::QueryRequest;
use crate::spelling_graph::SpellingOptions;
use crate::domain_dictionary::{self, DomainDictionary};
use crate::gram::GrammarConfig;
use crate::helpcode::{self, Keymap, SingleMatch};
use crate::lattice::{self, CharConstraint, Item, LanguageModel, Lexeme, Source};
use crate::google_pinyin::DecoderConstraint;
use crate::quanpin_dictionary::LN_TO_LOG10;
use crate::quanpin_query::{
    cut_pinyin_by_mode, has_only_complete_pinyin_segments, is_complete_pinyin_input, join_segments, segments_to_jianpin, split_segments, CutMode, Segments,
};
use crate::quanpin_utils::to_google_spelling;
use crate::sentence_sources::{neural_reranker, SentenceSources};
use crate::shuangpin::{self, Profile};
use crate::shuangpin_query::{self, SyllableHelpcode};
use crate::table::table_name;
use crate::user_dictionary::{self, DictionaryKind};
use rusqlite::{Connection, OpenFlags};
use std::cell::{Cell, RefCell};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

pub const OK: i32 = 0;
pub const ERROR_CODE: i32 = -1;

/// 神经重排最多重打分的路径数，也是开了重排时词格要解出的 n-best 条数。
const NEURAL_NBEST: i32 = 12;
const INITIAL_CANDIDATE_LIMIT: usize = 24;
/// 领域词库 `query` 的默认条数上限。
const DOMAIN_QUERY_LIMIT: usize = 128;
/// 词库读连接等一次提交的时间，对应 C++ 的 `kDictionaryBusyTimeoutMs`。
const DICTIONARY_BUSY_TIMEOUT: Duration = Duration::from_millis(250);

/// 整句联想的开关，对应 C++ 的 `SentenceAssociationOptions`。全部默认关闭。
///
/// 整句候选最多四行：词格首选、Google 解码器，以及两个神经模型的首选各一条。神经模型不自己造句，只在词格
/// 解出的最多 12 条 n-best 里挑一条；它们不依赖 `word_lattice` 的显示开关。
#[derive(Debug, Clone, PartialEq)]
pub struct SentenceAssociation {
    /// kenlm 词格整句（`Source::Generated`）。
    pub word_lattice: bool,
    /// Google 解码器整句（`Source::Fallback`）。
    pub google: bool,
    /// 用高精度档神经模型重排词格整句。
    pub neural_desktop: bool,
    /// 用快速档神经模型重排词格整句。
    pub neural_keyboard: bool,
    /// 某来源首选被去重时，显示该来源下一条不同结果。
    pub show_next_on_duplicate: bool,
    /// 高精度档模型的 `.safetensors` 路径；空 = 资源目录里的旧位置。
    pub neural_desktop_model: String,
    /// 万象语法模型（octagram `.gram`）路径；空 = 关闭，路径无效时词格静默退回无搭配打分。
    pub collocation_model: String,
    /// 搭配加成的标定系数（log10 域）。
    pub collocation_weight: f64,
    /// 是否把解码最优路径作为〔万象〕整句行显示出来。
    pub collocation_rerank: bool,
    pub collocation_grammar: GrammarConfig,
}

impl Default for SentenceAssociation {
    fn default() -> Self {
        SentenceAssociation {
            word_lattice: false,
            google: false,
            neural_desktop: false,
            neural_keyboard: false,
            show_next_on_duplicate: false,
            neural_desktop_model: String::new(),
            collocation_model: String::new(),
            collocation_weight: 1.0,
            collocation_rerank: false,
            collocation_grammar: GrammarConfig::default(),
        }
    }
}

impl SentenceAssociation {
    /// 任一整句来源开着。纠错的上下文消解用它而不是只看 `word_lattice`：只开万象重排或神经模型时词格照样在内部解码。
    pub fn any_sentence_source(&self) -> bool {
        self.word_lattice || self.google || self.neural_desktop || self.neural_keyboard || self.collocation_rerank
    }
}

pub struct ShuangpinDictionary<'a> {
    profile: &'static Profile,
    db_path: PathBuf,
    journal_path: String,
    dictionary: Option<Dictionary>,
    keymap: Keymap,
    language_model: Option<&'a dyn LanguageModel>,
    association: SentenceAssociation,
    /// Google 解码器、神经模型、万象语法库和重排线程；没给就没有这些来源。
    sources: Option<Arc<SentenceSources>>,
    /// 上屏的前文，神经重排给模型看的条件前缀。
    rescoring_context: String,
    /// C++ 里的 `_pinyin_sequence`：整句兜底行的“读音”取它，而不是 `generate_series` 的参数。按键状态机搬过来之前由调用方设。
    typed_sequence: String,
    fuzzy_rules: u32,
    syllable_helpcodes: Vec<SyllableHelpcode>,
    domain: DomainDictionary,
    /// 整串结果缓存（C++ 的 `_cached_buffer_series`）和三个辅助码筛出来的缓存，容量各 128。
    series_cache: RefCell<Cache<Vec<Item>>>,
    single_helpcode_cache: RefCell<Cache<Vec<Item>>>,
    single_helpcode_reversed_cache: RefCell<Cache<Vec<Item>>>,
    double_helpcode_cache: RefCell<Cache<Vec<Item>>>,
    /// 词库上一次见到的 `PRAGMA data_version`，别的连接改过库就清缓存。
    data_version: Cell<i64>,
    /// 直接辅助码的解析器；第一次用到才建。
    direct_resolver: RefCell<Option<Resolver>>,
}

fn double_helpcode_cache_key(pinyin: &str, help_codes: &str) -> String {
    // 同一串小写输入可以带正常或反向的辅助码。
    format!("{pinyin}:{help_codes}")
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
        fuzzy: false,
        corrected_from: String::new(),
        scheme: Default::default(),
        fixed_position: 0,
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
            sources: None,
            rescoring_context: String::new(),
            typed_sequence: String::new(),
            fuzzy_rules: 0,
            syllable_helpcodes: Vec::new(),
            domain: DomainDictionary::new(&db_path.with_file_name("domain-input.db")),
            series_cache: RefCell::new(Cache::new(128)),
            single_helpcode_cache: RefCell::new(Cache::new(128)),
            single_helpcode_reversed_cache: RefCell::new(Cache::new(128)),
            double_helpcode_cache: RefCell::new(Cache::new(128)),
            data_version: Cell::new(-1),
            direct_resolver: RefCell::new(None),
        };
        dictionary.reopen();
        // C++ 的构造函数在打开词库后立刻记下 data_version；之后别的连接再写库，第一次缓存命中就会清缓存。
        dictionary.reset_cache_if_database_changed();
        dictionary
    }

    /// 领域词库默认取 `msime.db` 同目录的 `domain-input.db`；资源不在那里时用它指过去。
    pub fn with_domain_database(mut self, path: &Path) -> Self {
        self.domain = DomainDictionary::new(path);
        self
    }

    pub fn set_enabled_domain_dictionaries(&mut self, csv: &str) {
        if self.domain.set_enabled(csv) {
            self.reset_cache();
        }
    }

    pub fn enabled_domain_dictionaries(&self) -> &str {
        self.domain.enabled()
    }

    fn reopen(&mut self) {
        self.dictionary = Dictionary::open(&self.db_path).ok();
        self.reset_cache();
    }

    /// 清掉所有结果缓存（写库、换开关时）。
    pub fn reset_cache(&self) {
        if let Some(resolver) = self.direct_resolver.borrow_mut().as_mut() {
            resolver.reset_cache();
        }
        self.series_cache.borrow_mut().clear();
        self.single_helpcode_cache.borrow_mut().clear();
        self.single_helpcode_reversed_cache.borrow_mut().clear();
        self.double_helpcode_cache.borrow_mut().clear();
    }

    /// 只清含词格整句排序的缓存：辅助码的三个缓存是从整串结果里筛出来的，一起清。
    pub fn reset_sentence_cache(&self) {
        self.series_cache.borrow_mut().clear();
        self.single_helpcode_cache.borrow_mut().clear();
        self.single_helpcode_reversed_cache.borrow_mut().clear();
        self.double_helpcode_cache.borrow_mut().clear();
    }

    /// 别的连接改过词库就清缓存。
    fn reset_cache_if_database_changed(&self) {
        let Some(dictionary) = &self.dictionary else { return };
        let Ok(version) = dictionary.connection().query_row("PRAGMA data_version", [], |row| row.get::<_, i64>(0)) else { return };
        if self.data_version.get() >= 0 && version != self.data_version.get() {
            self.reset_cache();
        }
        self.data_version.set(version);
    }

    /// 句中辅助码约束改变整句来源的结果，签名必须进缓存键。
    fn helpcodes_signature(&self) -> String {
        shuangpin_query::syllable_helpcodes_signature(&self.syllable_helpcodes)
    }

    pub fn profile(&self) -> &'static Profile {
        self.profile
    }

    pub fn set_sentence_association(&mut self, association: SentenceAssociation) {
        if self.association == association {
            return;
        }
        self.association = association;
        // 开关变了就清缓存，否则打开/关闭要等缓存过期才见效。
        self.reset_cache();
    }

    /// 接上整句来源的共享资源（全拼、双拼词库可以共用同一份）。
    pub fn with_sentence_sources(mut self, sources: Arc<SentenceSources>) -> Self {
        self.sources = Some(sources);
        self
    }

    /// 上屏的前文；只有神经重排会读它。
    pub fn set_rescoring_context(&mut self, context: &str) {
        if self.rescoring_context == context {
            return;
        }
        self.rescoring_context = context.to_string();
        // 只有神经重排生效时上文才影响候选。
        let rescoring_active = self.sources.as_deref().is_some_and(|sources| {
            (self.association.neural_keyboard && sources.keyboard_model().is_some())
                || (self.association.neural_desktop && sources.desktop_model(&self.association.neural_desktop_model).is_some())
        });
        if rescoring_active {
            self.reset_sentence_cache();
        }
    }

    /// 当前敲的双拼串（C++ 的 `_pinyin_sequence`）。
    pub fn set_typed_sequence(&mut self, sequence: &str) {
        self.typed_sequence = sequence.to_string();
    }

    pub fn set_fuzzy_pinyin(&mut self, rules: u32) {
        if self.fuzzy_rules == rules {
            return;
        }
        self.fuzzy_rules = rules;
        self.reset_cache();
    }

    pub fn set_syllable_helpcodes(&mut self, helpcodes: Vec<SyllableHelpcode>) {
        self.syllable_helpcodes = helpcodes;
    }

    pub fn set_helpcode_keymap(&mut self, keymap: Keymap) {
        if let Some(resolver) = self.direct_resolver.borrow_mut().as_mut() {
            resolver.reset_keymap();
        }
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
        let mut result: Vec<Item> = rows.into_iter().map(|row| item(pinyin_sequence, &row.text, row.weight, Source::Database, &row.key)).collect();
        domain_dictionary::merge(&mut result, self.domain.query(&segments, pinyin_sequence, DOMAIN_QUERY_LIMIT, QuerySource::Shuangpin));
        result
    }

    fn query_initial_from_quanpin_database(&self, code: &str, limit: i64) -> Vec<Item> {
        let Some(dictionary) = &self.dictionary else { return Vec::new() };
        if code.len() != 1 || !code.as_bytes()[0].is_ascii_lowercase() {
            return Vec::new();
        }
        let initial = self.quanpin_segmentation(code);
        let rows = dictionary.query_initial(&initial, limit).unwrap_or_default();
        let mut result: Vec<Item> = rows.into_iter().map(|row| item(code, &row.text, row.weight, Source::Database, &row.key)).collect();
        domain_dictionary::merge(&mut result, self.domain.query(&[initial], code, limit.max(0) as usize, QuerySource::Shuangpin));
        result
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
        self.expand_initial_candidates_keyed(code, candidates, "")
    }

    /// 同上；`series_cache_key` 非空时把扩展后的列表写回整串结果缓存。
    pub fn expand_initial_candidates_keyed(&self, code: &str, candidates: &mut Vec<Item>, series_cache_key: &str) -> bool {
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
        if !series_cache_key.is_empty() {
            self.series_cache.borrow_mut().insert(series_cache_key, candidates.clone());
        }
        true
    }

    /// 词格的跨度查询：词库行加上模糊音变体，按折后权重稳定排序，截到 `limit`。
    fn lattice_lookup(&self, span: &[String], limit: i32) -> Vec<Lexeme> {
        let Some(dictionary) = &self.dictionary else { return Vec::new() };
        let mut rows = dictionary.lattice_lookup(span, limit, self.fuzzy_rules).unwrap_or_default();
        for row in self.domain.lattice(span, limit, self.fuzzy_rules) {
            match rows.iter_mut().find(|base| base.key == row.key && base.value == row.value) {
                None => rows.push(row),
                Some(duplicate) => duplicate.input_domains = row.input_domains,
            }
        }
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

    /// Google 解码器对全拼切分的整句。只有送进解码器的那份换 ü 的写法，音节数与起点都不变，句中辅助码的位置
    /// 直接按替换后的串算；约束让某一段走不通时解码器可能吐出残句，字数对不上或约束没满足就不出这一句。
    fn decode_google_sentence(&self, quanpin_segmentation: &str) -> String {
        let Some(decoder) = self.sources.as_ref().and_then(|sources| sources.google()) else { return String::new() };
        let google_input = to_google_spelling(quanpin_segmentation);
        if self.syllable_helpcodes.is_empty() {
            return decoder.sentence(&google_input, &[]);
        }
        let mut syllable_starts = vec![0usize];
        for (at, byte) in google_input.bytes().enumerate() {
            if byte == b'\'' {
                syllable_starts.push(at + 1);
            }
        }
        let accepts: Vec<(usize, Box<dyn Fn(&str) -> bool + '_>)> = self
            .syllable_helpcodes
            .iter()
            .filter(|helpcode| helpcode.syllable < syllable_starts.len())
            .map(|helpcode| {
                let helpcode = *helpcode;
                (syllable_starts[helpcode.syllable], Box::new(move |hanzi: &str| self.accepts_syllable_char(&helpcode, hanzi)) as Box<dyn Fn(&str) -> bool + '_>)
            })
            .collect();
        let constraints: Vec<DecoderConstraint> = accepts.iter().map(|(offset, accept)| DecoderConstraint { pinyin_offset: *offset, accept: accept.as_ref() }).collect();
        let sentence = decoder.sentence(&google_input, &constraints);
        if sentence.is_empty() || sentence.chars().count() != syllable_starts.len() || !self.satisfies_syllable_helpcodes(&sentence) {
            return String::new();
        }
        sentence
    }

    /// 直接辅助码：整句解码选出切分，把请求改写成句中辅助码的形状。改写了返回 true。
    pub fn resolve_direct_helpcode(&self, request: &mut QueryRequest) -> bool {
        self.reset_cache_if_database_changed();
        let mut resolver = self.direct_resolver.borrow_mut().take().unwrap_or_else(|| Resolver::new(self.profile));
        // 与 generate_series 的词格同一套查询：普通跨度在 SQL 里截到 32 行，带辅码约束的跨度不截，筛完约束再截。
        let lookup = |span: &[String], constrained: bool| self.lattice_lookup(span, if constrained { 4096 } else { 32 });
        let single_rows = || self.query_single_char_rows();
        let collocation = self.sources.as_deref().and_then(|s| s.gram(&self.association.collocation_model));
        let grammar = self.association.collocation_grammar;
        let collocation_score = |tail: &str, word: &str, is_rear: bool| -> f64 { collocation.as_ref().map_or(0.0, |db| db.query(tail, word, is_rear, &grammar, None) * LN_TO_LOG10) };
        let mut options = DecodeOptions { language_model: self.language_model, ..DecodeOptions::default() };
        if collocation.is_some() && self.association.collocation_weight != 0.0 {
            options.collocation_scorer = Some(&collocation_score);
            options.collocation_weight = self.association.collocation_weight;
        }
        let context = ResolveContext { lookup: Some(&lookup), single_char_rows: Some(&single_rows), keymap: Some(&self.keymap), options };
        let typed = if request.raw_input_with_cases.is_empty() { request.raw_input.clone() } else { request.raw_input_with_cases.clone() };
        let spelling_options = SpellingOptions { slash_marker: request.direct_helpcode_slash_marker, uppercase_marker: request.direct_helpcode_uppercase_marker };
        let resolved = resolver.resolve(&typed, &spelling_options, &context);
        *self.direct_resolver.borrow_mut() = Some(resolver);
        let Some(rewrite) = resolved.rewrite else { return false };
        request.raw_input_with_syllable_helpcodes = rewrite.raw_input_with_syllable_helpcodes;
        request.raw_input = rewrite.raw_input.clone();
        request.raw_input_with_cases = rewrite.raw_input;
        request.syllable_helpcodes = rewrite.syllable_helpcodes;
        request.enable_mid_sentence_helpcode = true;
        request.enable_shuangpin_helpcode = false;
        request.has_syllable_helpcode_layout = true;
        request.syllable_helpcode_source_index = rewrite.source_index;
        request.syllable_helpcode_decorations = rewrite.decorations;
        request.valid = rewrite.valid;
        if request.valid {
            request.raw_segmentation = rewrite.raw_segmentation;
            request.normalized_segmentation = rewrite.normalized_segmentation.clone();
            request.segmentation = rewrite.normalized_segmentation;
            request.normalized_input = rewrite.normalized_input;
        }
        true
    }

    /// 词库里全部单字行（全拼读音，字）；单字都在 `tbl_1_<首字母>` 里。
    fn query_single_char_rows(&self) -> Vec<(String, String)> {
        let mut rows = Vec::new();
        let Some(dictionary) = &self.dictionary else { return rows };
        for letter in b'a'..=b'z' {
            let Some(table) = table_name(&[(letter as char).to_string()]) else { continue };
            let Ok(mut statement) = dictionary.connection().prepare(&format!("SELECT key, value FROM \"{table}\"")) else { continue };
            let Ok(found) = statement.query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))) else { continue };
            rows.extend(found.filter_map(Result::ok));
        }
        rows
    }

    /// 双拼串对应的所有候选：整串精确命中的词在前，再是各个前缀子串的词，最后并进词格整句。
    pub fn generate_series(&self, pinyin_sequence: &str, pinyin_segmentation: &str) -> Vec<Item> {
        self.generate_series_keyed(pinyin_sequence, pinyin_segmentation, "")
    }

    /// 同上，缓存键用 `cache_key`（空则用输入串）；引擎层传带手动分隔符的原串。
    pub fn generate_series_keyed(&self, pinyin_sequence: &str, pinyin_segmentation: &str, cache_key: &str) -> Vec<Item> {
        if pinyin_sequence.len() < 2 {
            return self.generate_series_uncached(pinyin_sequence, pinyin_segmentation);
        }
        let key = format!("{}{}", if cache_key.is_empty() { pinyin_sequence } else { cache_key }, self.helpcodes_signature());
        if self.series_cache.borrow_mut().find(&key).is_some() {
            self.reset_cache_if_database_changed();
            if let Some(cached) = self.series_cache.borrow_mut().find(&key) {
                return cached;
            }
        }
        let candidates = self.generate_series_uncached(pinyin_sequence, pinyin_segmentation);
        self.series_cache.borrow_mut().insert(&key, candidates.clone());
        candidates
    }

    fn generate_series_uncached(&self, pinyin_sequence: &str, pinyin_segmentation: &str) -> Vec<Item> {
        let mut candidates: Vec<Item> = Vec::new();
        if pinyin_sequence.is_empty() {
            return candidates;
        }
        if pinyin_sequence.len() == 1 {
            return self.query_initial_from_quanpin_database(pinyin_sequence, INITIAL_CANDIDATE_LIMIT as i64);
        }

        // 严格对应当前拼音的数据；词库一行都没有时，Google 联想开着就补一条整句兜底。
        let current = self.generate(pinyin_sequence, pinyin_segmentation);
        if !current.is_empty() {
            candidates.extend(current);
        } else if self.association.google {
            let quanpin_str = self.quanpin_segmentation(pinyin_segmentation);
            // 只有送进解码器的那一份换 ü 的写法，quanpin_str 仍是词库那侧的 canonical 拼写，候选要靠它落库。
            let sentence = self.decode_google_sentence(&quanpin_str);
            if !sentence.is_empty() {
                let mut fallback = item(&self.typed_sequence, &sentence, 1, Source::Fallback, &quanpin_str);
                fallback.sentence_association = true;
                candidates.push(fallback);
            }
        }

        // 前缀子串，给接下来可能的造词用。
        let mut seg_pinyin = pinyin_segmentation.to_string();
        while let Some(pos) = seg_pinyin.rfind('\'') {
            seg_pinyin.truncate(pos);
            let pure_pinyin = remove_delimiters(&seg_pinyin);
            candidates.extend(self.generate(&pure_pinyin, &seg_pinyin));
        }

        let quanpin_segmentation = self.quanpin_segmentation(pinyin_segmentation);
        let quanpin_syllables = split_segments(&quanpin_segmentation);

        // 次序与全拼一致：词格在前，Google 解码器在后，都插在“整串精确命中词库”的候选之后。
        if self.association.google && quanpin_syllables.len() >= 2 && quanpin_segmentation.contains('\'') {
            let google_sentence = self.decode_google_sentence(&quanpin_segmentation);
            let duplicate = candidates.iter().any(|candidate| candidate.word == google_sentence);
            if !google_sentence.is_empty() && !duplicate {
                let insert_at = lattice::generated_sentence_insert_position(&candidates, &quanpin_syllables);
                let mut sentence = item(&self.typed_sequence, &google_sentence, 1, Source::Fallback, &quanpin_segmentation);
                sentence.sentence_association = true;
                candidates.insert(insert_at, sentence);
            }
        }

        // 词格给 Trigram 候选和神经模型共用；只开神经时仍在内部解出 n-best，但不显示词格首选。与全拼同构。
        let sources = self.sources.as_deref();
        let keyboard_model = sources.filter(|_| self.association.neural_keyboard).and_then(|s| s.keyboard_model());
        let desktop_model = sources.filter(|_| self.association.neural_desktop).and_then(|s| s.desktop_model(&self.association.neural_desktop_model));
        let keyboard_reranker = match (sources, &keyboard_model) {
            (Some(sources), Some(model)) => Some(neural_reranker(sources, model, &self.rescoring_context)),
            _ => None,
        };
        let desktop_reranker = match (sources, &desktop_model) {
            (Some(sources), Some(model)) => Some(neural_reranker(sources, model, &self.rescoring_context)),
            _ => None,
        };
        let mut rerankers: Vec<lattice::SourcedReranker> = Vec::new();
        if let Some(rerank) = &keyboard_reranker {
            rerankers.push(lattice::SourcedReranker { rerank, source: Source::NeuralKeyboard });
        }
        if let Some(rerank) = &desktop_reranker {
            rerankers.push(lattice::SourcedReranker { rerank, source: Source::NeuralDesktop });
        }

        // 万象语法：搭配分在解码期逐边叠加一次。
        let collocation = sources.and_then(|s| s.gram(&self.association.collocation_model));
        let has_collocation = collocation.is_some() && self.association.collocation_weight != 0.0;
        let grammar = self.association.collocation_grammar;
        let collocation_score = |tail: &str, word: &str, is_rear: bool| -> f64 {
            collocation.as_ref().map_or(0.0, |db| db.query(tail, word, is_rear, &grammar, None) * LN_TO_LOG10)
        };

        let needs_alternatives = !rerankers.is_empty() || (self.association.word_lattice && self.association.show_next_on_duplicate);
        let accepts: Vec<Box<dyn Fn(&str) -> bool + '_>> = self
            .syllable_helpcodes
            .iter()
            .map(|helpcode| {
                let helpcode = *helpcode;
                Box::new(move |hanzi: &str| self.accepts_syllable_char(&helpcode, hanzi)) as Box<dyn Fn(&str) -> bool + '_>
            })
            .collect();
        let mut options = lattice::Options {
            nbest: if needs_alternatives { NEURAL_NBEST } else { 1 },
            include_lattice_best: self.association.word_lattice,
            include_collocation_best: self.association.collocation_rerank && has_collocation,
            show_next_on_duplicate: self.association.show_next_on_duplicate,
            language_model: self.language_model,
            ..lattice::Options::default()
        };
        if has_collocation {
            options.collocation_scorer = Some(&collocation_score);
            options.collocation_weight = self.association.collocation_weight;
        }
        // 句中辅助码：词格建图时按约束筛边，受约束的跨度不截断地查词库（生僻字才筛得出来）。
        for (helpcode, accept) in self.syllable_helpcodes.iter().zip(&accepts) {
            options.char_constraints.push(CharConstraint { syllable: helpcode.syllable, accept: accept.as_ref() });
        }
        let constrained = |span: &[String]| self.lattice_lookup(span, 4096);
        if !options.char_constraints.is_empty() {
            options.constrained_lookup = Some(&constrained);
        }
        if self.association.word_lattice || !rerankers.is_empty() || options.include_collocation_best {
            let plain = |span: &[String]| self.lattice_lookup(span, options.span_limit);
            lattice::merge_lattice_candidates(&mut candidates, &quanpin_syllables, &plain, pinyin_sequence, &options, &rerankers);
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
        let reversed_single = help_codes.len() == 1 && help_codes.as_bytes()[0].is_ascii_uppercase();
        let signature = self.helpcodes_signature();
        let single_cache = if reversed_single { &self.single_helpcode_reversed_cache } else { &self.single_helpcode_cache };
        let single_key = format!("{pinyin_sequence}{signature}");
        let double_key = format!("{}{signature}", double_helpcode_cache_key(pinyin_sequence, help_codes));
        let cache = match help_codes.len() {
            1 => Some((single_cache, single_key.as_str())),
            2 => Some((&self.double_helpcode_cache, double_key.as_str())),
            _ => None,
        };
        if let Some((cache, key)) = cache {
            if cache.borrow_mut().find(key).is_some() {
                self.reset_cache_if_database_changed();
                if let Some(cached) = cache.borrow_mut().find(key) {
                    return cached;
                }
            }
        }
        let candidates = self.generate_series(pure_pinyin, pure_pinyin_segmentation);
        let mut result = Vec::new();
        match help_codes.len() {
            1 => self.filter_with_single_helpcode(&candidates, &mut result, help_codes, pinyin_sequence),
            2 => self.filter_with_double_helpcodes(&candidates, &mut result, help_codes),
            _ => {}
        }
        if let Some((cache, key)) = cache {
            cache.borrow_mut().insert(key, result.clone());
        }
        result
    }

    /// 云/AI 候选插进整串结果缓存里这个双拼串对应的那份列表（键带上当前句中辅助码签名）。
    pub fn insert_word_to_series_cache(&self, pinyin: &str, word: &str, source: Source) -> i32 {
        if pinyin.is_empty() || word.is_empty() {
            return -1;
        }
        let key = format!("{pinyin}{}", self.helpcodes_signature());
        let mut cache = self.series_cache.borrow_mut();
        let mut list = cache.get(&key).unwrap_or_default();
        insert_dynamic_candidate(&mut list, pinyin, word, source, true);
        cache.insert(&key, list);
        0
    }

    /// 同上，插进辅助码筛出来的缓存：缓存里没有这一项就返回 -1，不新建。
    pub fn insert_word_to_active_helpcode_cache(&self, pinyin: &str, word: &str, source: Source, double_helpcodes: &str) -> i32 {
        let signature = self.helpcodes_signature();
        let insert_into = |cache: &RefCell<Cache<Vec<Item>>>, key: &str| -> bool {
            let mut cache = cache.borrow_mut();
            let Some(mut list) = cache.get(key) else { return false };
            insert_dynamic_candidate(&mut list, pinyin, word, source, false);
            cache.insert(key, list);
            true
        };
        if !double_helpcodes.is_empty() {
            let key = format!("{}{signature}", double_helpcode_cache_key(pinyin, double_helpcodes));
            return if insert_into(&self.double_helpcode_cache, &key) { 0 } else { -1 };
        }
        let key = format!("{pinyin}{signature}");
        let updated_single = insert_into(&self.single_helpcode_cache, &key);
        let updated_reversed = insert_into(&self.single_helpcode_reversed_cache, &key);
        if updated_single || updated_reversed {
            0
        } else {
            -1
        }
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
