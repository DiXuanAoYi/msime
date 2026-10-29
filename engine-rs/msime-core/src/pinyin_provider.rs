//! 拼音候选提供者：全拼引擎和双拼引擎，对应 `engine/quanpin/engine.cpp`、`engine/shuangpin/engine.cpp`、
//! `engine/providers/pinyin_candidate_provider.cpp`。
//!
//! 引擎层把请求里的开关下发给词典层，处理末尾辅助码（全拼大写字母、双拼单码/双码），再把候选标上产出它的方案。
//! 没搬的：双拼直接辅助码的解析器接线（`resolve_direct_helpcode`）。

use crate::helpcode::{self, Keymap};
use crate::lattice::{Item, LanguageModel, Source};
use crate::quanpin_dictionary::{merge_fuzzy_items_by, QuanpinDictionary};
use crate::quanpin_query::{cut_pinyin_by_mode, join_segments, split_segments, CutMode};
use crate::quanpin_utils::{detect_active_helpcode_length, strip_active_helpcodes};
use crate::request::{QueryRequest, SchemeType};
use crate::sentence_sources::SentenceSources;
use crate::shuangpin::{self, Profile};
use crate::shuangpin_dictionary::ShuangpinDictionary;
use crate::shuangpin_query as sq;
use std::path::{Path, PathBuf};
use std::sync::Arc;

/// 请求里每种纠错一个开关，词典层用一个类型掩码。任一开关开着就连带打开删除位和插入位：
/// 用户想让手误被纠正，漏敲和多敲也在其中；都关着就是 0。
fn autocorrect_types_from_request(request: &QueryRequest) -> u32 {
    use crate::autocorrect::{DELETION, INSERTION, NEIGHBOR, TRANSPOSITION};
    let legacy = (if request.enable_quanpin_autocorrect_transposition { TRANSPOSITION } else { 0 }) | (if request.enable_quanpin_autocorrect_neighbor { NEIGHBOR } else { 0 });
    if legacy == 0 {
        0
    } else {
        legacy | DELETION | INSERTION
    }
}

struct HelpcodeQuery {
    base_pure_input: String,
    base_segmentation: String,
    help_codes: String,
}

fn segmented_parts_are_all_two_chars(segmentation: &str) -> bool {
    !segmentation.is_empty() && segmentation.split('\'').all(|part| part.len() == 2)
}

fn build_full_helpcode_query(raw_input: &str, raw_input_with_cases: &str, profile: &Profile) -> Option<HelpcodeQuery> {
    if sq::detect_active_double_helpcode_length(raw_input, raw_input_with_cases, profile) != 2 {
        return None;
    }
    let base_raw_input = sq::trim_trailing_letters_preserve_delimiters(raw_input, 2);
    Some(HelpcodeQuery {
        base_pure_input: sq::remove_manual_delimiters(&base_raw_input),
        base_segmentation: sq::segment_input(&base_raw_input, profile),
        help_codes: shuangpin::full_help_codes(&sq::remove_manual_delimiters(raw_input_with_cases)),
    })
}

fn build_single_helpcode_query(raw_input: &str, pure_input_with_cases: &str, profile: &Profile) -> Option<HelpcodeQuery> {
    if pure_input_with_cases.len() <= 1 || pure_input_with_cases.len() % 2 == 0 {
        return None;
    }
    let base_raw_input = sq::trim_trailing_letters_preserve_delimiters(raw_input, 1);
    let base_pure_input = sq::remove_manual_delimiters(&base_raw_input);
    let base_segmentation = sq::segment_input(&base_raw_input, profile);
    if raw_input.contains('\'') && !segmented_parts_are_all_two_chars(&base_segmentation) {
        return None;
    }
    if !shuangpin::all_units_complete(&base_pure_input, &base_segmentation) {
        return None;
    }
    Some(HelpcodeQuery { base_pure_input, base_segmentation, help_codes: pure_input_with_cases[pure_input_with_cases.len() - 1..].to_string() })
}

pub struct PinyinProvider<'a> {
    profile: &'static Profile,
    quanpin: QuanpinDictionary<'a>,
    shuangpin: ShuangpinDictionary<'a>,
    /// 双拼的模糊音候选借全拼词库查，用到才开。
    fuzzy_dictionary: Option<QuanpinDictionary<'a>>,
    keymap: Keymap,
    db_path: PathBuf,
    journal_path: String,
    language_model: Option<&'a dyn LanguageModel>,
}

impl<'a> PinyinProvider<'a> {
    pub fn open(profile: &'static Profile, db_path: &Path, journal_path: &str, keymap: Keymap, language_model: Option<&'a dyn LanguageModel>) -> Self {
        PinyinProvider {
            profile,
            quanpin: QuanpinDictionary::open(db_path, journal_path, language_model),
            shuangpin: ShuangpinDictionary::open(profile, db_path, journal_path, keymap.clone(), language_model),
            fuzzy_dictionary: None,
            keymap,
            db_path: db_path.to_path_buf(),
            journal_path: journal_path.to_string(),
            language_model,
        }
    }

    pub fn with_sentence_sources(mut self, sources: Arc<SentenceSources>) -> Self {
        self.quanpin = self.quanpin.with_sentence_sources(sources.clone());
        self.shuangpin = self.shuangpin.with_sentence_sources(sources);
        self
    }

    pub fn with_domain_database(mut self, path: &Path) -> Self {
        self.quanpin = self.quanpin.with_domain_database(path);
        self.shuangpin = self.shuangpin.with_domain_database(path);
        self
    }

    pub fn query(&mut self, request: &QueryRequest) -> Vec<Item> {
        if !request.valid {
            return Vec::new();
        }
        let mut candidates = match request.scheme {
            SchemeType::Shuangpin => self.query_shuangpin(request),
            SchemeType::Quanpin => self.query_quanpin(request),
            _ => return Vec::new(),
        };
        // 候选带上产出它的方案：混输组合里五笔与拼音候选共存，删除与调频按候选自己的方案路由。
        for item in &mut candidates {
            item.scheme = request.scheme;
        }
        candidates
    }

    // ------------------------------------------------------------------------------------------
    // 全拼
    // ------------------------------------------------------------------------------------------

    fn query_quanpin(&mut self, request: &QueryRequest) -> Vec<Item> {
        // 整句联想开关、领域词库、前文随请求下发；换了前文，按旧前文重排出来的缓存顺序作废。
        self.quanpin.set_sentence_association(request.sentence_association.clone());
        self.quanpin.set_enabled_domain_dictionaries(&request.enabled_domain_dictionaries);
        self.quanpin.set_rescoring_context(&request.rescoring_context);

        let helpcode_length = if request.enable_quanpin_helpcode { detect_active_helpcode_length(&request.raw_input, &request.raw_input_with_cases) } else { 0 };
        if helpcode_length == 1 || helpcode_length == 2 {
            let base_raw_input = strip_active_helpcodes(&request.raw_input, &request.raw_input_with_cases);
            let base_segmentation = cut_pinyin_by_mode(&base_raw_input, CutMode::Correction).first().map(|cut| join_segments(cut)).unwrap_or_default();
            let help_codes = &request.raw_input[request.raw_input.len() - helpcode_length..];
            let base = self.quanpin.query(&base_raw_input, &base_segmentation, autocorrect_types_from_request(request), request.fuzzy_pinyin);
            return if helpcode_length == 2 {
                helpcode::filter_with_double_helpcodes(&base, |item| &item.word, help_codes, &self.keymap)
            } else {
                helpcode::reorder_with_single_helpcode(&base, |item| &item.word, help_codes, &self.keymap)
            };
        }
        self.quanpin.query(&request.raw_input, &request.segmentation, autocorrect_types_from_request(request), request.fuzzy_pinyin)
    }

    // ------------------------------------------------------------------------------------------
    // 双拼
    // ------------------------------------------------------------------------------------------

    fn query_shuangpin(&mut self, request: &QueryRequest) -> Vec<Item> {
        self.shuangpin.set_sentence_association(request.sentence_association.clone());
        self.shuangpin.set_enabled_domain_dictionaries(&request.enabled_domain_dictionaries);
        self.shuangpin.set_rescoring_context(&request.rescoring_context);
        // 句中辅助码约束也随请求下发；开关关着时不筛。
        self.shuangpin.set_syllable_helpcodes(if request.enable_mid_sentence_helpcode { request.syllable_helpcodes.clone() } else { Vec::new() });
        self.shuangpin.set_fuzzy_pinyin(request.fuzzy_pinyin);
        let mut candidates = self.query_shuangpin_unfiltered(request);
        self.shuangpin.filter_by_syllable_helpcodes(&mut candidates);
        candidates
    }

    fn query_shuangpin_unfiltered(&mut self, request: &QueryRequest) -> Vec<Item> {
        let raw_input = request.raw_input.as_str();
        let raw_input_with_cases = if request.raw_input_with_cases.is_empty() { raw_input } else { request.raw_input_with_cases.as_str() };
        let pure_input = sq::remove_manual_delimiters(raw_input);
        let pure_input_with_cases = sq::remove_manual_delimiters(raw_input_with_cases);
        if pure_input.is_empty() {
            return Vec::new();
        }

        if request.enable_shuangpin_helpcode {
            // 双码辅助，再是单码辅助；都不满足就当普通输入（cls -> c'ls）。
            let query = build_full_helpcode_query(raw_input, raw_input_with_cases, self.profile).or_else(|| build_single_helpcode_query(raw_input, &pure_input_with_cases, self.profile));
            if let Some(query) = query {
                let exact = self.shuangpin.generate_with_helpcodes(&query.base_pure_input, &query.base_segmentation, raw_input, &query.help_codes);
                return self.append_fuzzy(exact, &query.base_segmentation, request, &query.help_codes);
            }
        }
        let segmentation = sq::segment_input(raw_input, self.profile);
        let exact = self.shuangpin.generate_series_keyed(&pure_input, &segmentation, raw_input);
        self.append_fuzzy(exact, &segmentation, request, "")
    }

    fn append_fuzzy(&mut self, exact: Vec<Item>, raw_segmentation: &str, request: &QueryRequest, help_codes: &str) -> Vec<Item> {
        if request.fuzzy_pinyin == 0 {
            return exact;
        }
        let (db_path, journal_path, language_model) = (self.db_path.clone(), self.journal_path.clone(), self.language_model);
        let fuzzy_dictionary = self.fuzzy_dictionary.get_or_insert_with(|| QuanpinDictionary::open(&db_path, &journal_path, language_model));
        fuzzy_dictionary.set_enabled_domain_dictionaries(self.shuangpin.enabled_domain_dictionaries());
        let typed = split_segments(raw_segmentation);
        let mut fuzzy = fuzzy_dictionary.fuzzy_candidates(&sq::to_quanpin_segmentation(raw_segmentation, self.profile), request.fuzzy_pinyin);
        for item in &mut fuzzy {
            let count = split_segments(&item.pinyin).len();
            if count > typed.len() {
                continue;
            }
            item.pinyin = join_segments(&typed[..count]);
        }
        let fuzzy = match help_codes.len() {
            2 => helpcode::filter_with_double_helpcodes(&fuzzy, |item| &item.word, help_codes, &self.keymap),
            1 => helpcode::reorder_with_single_helpcode(&fuzzy, |item| &item.word, help_codes, &self.keymap),
            _ => fuzzy,
        };
        merge_fuzzy_items_by(exact, fuzzy, |item| item.pinyin.len())
    }

    // ------------------------------------------------------------------------------------------
    // 其余入口
    // ------------------------------------------------------------------------------------------

    /// 单字母输入只取了 24 条时，把它们换成全部。展开出来的候选同样要带上方案。
    pub fn expand_initial_candidates(&mut self, request: &QueryRequest, candidates: &mut Vec<Item>) -> bool {
        let expanded = match request.scheme {
            SchemeType::Shuangpin => {
                let segmentation = sq::segment_input(&request.raw_input, self.profile);
                let initial = segmentation.split('\'').next().unwrap_or("").to_string();
                initial.len() == 1 && self.shuangpin.expand_initial_candidates_keyed(&initial, candidates, &request.raw_input)
            }
            SchemeType::Quanpin => {
                if request.enable_quanpin_helpcode && detect_active_helpcode_length(&request.raw_input, &request.raw_input_with_cases) > 0 {
                    false
                } else {
                    let segments = split_segments(&request.segmentation);
                    match segments.first() {
                        Some(first) if first.len() == 1 => self.quanpin.expand_initial_candidates(first, candidates),
                        _ => false,
                    }
                }
            }
            _ => false,
        };
        if expanded {
            for item in candidates.iter_mut() {
                item.scheme = request.scheme;
            }
        }
        expanded
    }

    pub fn find_candidate(&self, scheme: SchemeType, key: &str, value: &str) -> Option<Item> {
        let mut found = if scheme == SchemeType::Shuangpin { self.shuangpin.find_candidate(key, value) } else { self.quanpin.find_candidate(key, value) };
        // 固定位置补入的候选不经过 query，同样按查它的方案打标，调频与删除才会回到同一个引擎。
        if let Some(item) = &mut found {
            item.scheme = if scheme == SchemeType::Shuangpin { SchemeType::Shuangpin } else { SchemeType::Quanpin };
        }
        found
    }

    pub fn reset_cache(&self) {
        self.quanpin.reset_cache();
        self.shuangpin.reset_cache();
        if let Some(dictionary) = &self.fuzzy_dictionary {
            dictionary.reset_cache();
        }
    }

    pub fn reset_sentence_cache(&self) {
        self.quanpin.reset_sentence_cache();
        self.shuangpin.reset_sentence_cache();
        if let Some(dictionary) = &self.fuzzy_dictionary {
            dictionary.reset_sentence_cache();
        }
    }

    pub fn create_word(&mut self, scheme: SchemeType, pinyin: &str, word: &str) -> i32 {
        if scheme == SchemeType::Shuangpin {
            self.shuangpin.create_word(pinyin, word)
        } else {
            self.quanpin.create_word(pinyin, word)
        }
    }

    pub fn update_weight_by_pinyin_and_word(&mut self, scheme: SchemeType, pinyin: &str, word: &str) -> i32 {
        if scheme == SchemeType::Shuangpin {
            self.shuangpin.update_weight_by_pinyin_and_word(pinyin, word)
        } else {
            self.quanpin.update_weight_by_pinyin_and_word(pinyin, word)
        }
    }

    pub fn delete_by_pinyin_and_word(&mut self, scheme: SchemeType, pinyin: &str, word: &str) -> i32 {
        if scheme == SchemeType::Shuangpin {
            self.shuangpin.delete_by_pinyin_and_word(pinyin, word)
        } else {
            self.quanpin.delete_by_pinyin_and_word(pinyin, word)
        }
    }

    /// 云/AI 候选缓存到这个读音对应的候选列表里。
    pub fn cache_dynamic_candidate(&self, scheme: SchemeType, pinyin: &str, word: &str, source: Source) -> i32 {
        if scheme == SchemeType::Shuangpin {
            self.shuangpin.insert_word_to_series_cache(pinyin, word, source)
        } else {
            self.quanpin.insert_word_to_series_cache(pinyin, word, source)
        }
    }

    /// 同上，键与这次请求的查询一致。
    pub fn cache_dynamic_candidate_for_request(&self, request: &QueryRequest, word: &str, source: Source) -> i32 {
        match request.scheme {
            SchemeType::Quanpin => self.quanpin.insert_word_to_series_cache_for_request(&request.raw_input, &request.segmentation, autocorrect_types_from_request(request), word, source),
            SchemeType::Shuangpin => {
                if !request.enable_shuangpin_helpcode {
                    return self.shuangpin.insert_word_to_series_cache(&request.raw_input, word, source);
                }
                let raw_input_with_cases = if request.raw_input_with_cases.is_empty() { &request.raw_input } else { &request.raw_input_with_cases };
                let pure_input = sq::remove_manual_delimiters(&request.raw_input);
                let pure_input_with_cases = sq::remove_manual_delimiters(raw_input_with_cases);
                if shuangpin::is_full_help_mode(&pure_input_with_cases, self.profile) {
                    return self.shuangpin.insert_word_to_active_helpcode_cache(&request.raw_input, word, source, &shuangpin::full_help_codes(&pure_input_with_cases));
                }
                if pure_input.len() % 2 == 1 && pure_input.len() > 1 {
                    let base_raw_input = &pure_input[..pure_input.len() - 1];
                    let base_segmentation = sq::segment_input(base_raw_input, self.profile);
                    if shuangpin::all_units_complete(base_raw_input, &base_segmentation) {
                        return self.shuangpin.insert_word_to_active_helpcode_cache(&request.raw_input, word, source, "");
                    }
                }
                self.shuangpin.insert_word_to_series_cache(&request.raw_input, word, source)
            }
            _ => -1,
        }
    }
}
