//! 混排、固定位置、本地模式候选、专用英文候选、光标前缀候选。对应 `engine/core/input_session_candidates.cpp`
//! 和 `input_session.cpp` 里的 `update_mixed_candidates` 等。

use super::composition::head;
use super::{InputSession, KeyResult, LocalInputMode, QUICK_PHRASE_MAX_SLOT};
use crate::candidate_queries::convert_all;
use crate::helpcode;
use crate::lattice::{Item, Source};
use crate::local;
use crate::quanpin_query::{self, join_segments, CutMode};
use crate::request::SchemeType;
use crate::user_dictionary::{self, DictionaryKind};
use std::path::Path;

impl<'a> InputSession<'a> {
    pub fn enable_fixed_positions(&mut self) {
        self.fixed_positions_enabled = true;
        self.update_mixed_candidates();
    }

    /// `candidates()` 返回混排列表（固定位置、英文/表情/颜文字混排任一开启）的条件。
    pub(super) fn mixed_candidates_active(&self) -> bool {
        self.fixed_positions_enabled
            || ((self.english_input_options.mixed_candidates || self.mixed_expressive_options.emoji_candidates || self.mixed_expressive_options.kaomoji_candidates)
                && (self.scheme() == SchemeType::Quanpin || self.scheme() == SchemeType::Shuangpin))
    }

    pub(super) fn update_mixed_candidates(&mut self) {
        self.refresh_prefix_candidates();
        // 只有混排列表才会被 candidates() 返回；混排开关、方案与固定位置每次变动都会重新调用本函数，所以清空后不会读到旧列表。
        if !self.mixed_candidates_active() {
            self.mixed_candidates.clear();
            return;
        }
        let decoded: Vec<Item> = if self.prefix_candidates_active { self.prefix_candidates.clone() } else { self.engine.candidates().to_vec() };
        // 联想（英文/表情）跟着正在转换的那段走：光标前缀激活时是前缀，否则是整个原始输入。
        let association_prefix = if self.prefix_candidates_active { self.prefix_query_input.clone() } else { self.engine.request().raw_input.clone() };
        let scheme = self.scheme();
        let mut mixed = self.candidate_queries.mixed(
            decoded,
            &association_prefix,
            scheme,
            self.english_input_options,
            self.mixed_expressive_options,
            self.dedicated_english_mode,
            self.local_input_mode,
        );
        self.apply_candidate_positions(&mut mixed);
        let Some(code) = self.quick_phrase_code() else {
            self.mixed_candidates = mixed;
            return;
        };
        let phrases = local::query_quick_phrases_by_code(&code, Path::new(&self.main_dictionary_path()), 20);
        if phrases.items.is_empty() {
            self.mixed_candidates = mixed;
            return;
        }
        let mut slot = 0usize;
        if self.local_mode_options.quick_phrase_frequency && self.frequency_adjustment_configured && self.frequency_adjustment.mode != super::FrequencyAdjustmentMode::Disabled {
            let stored = user_dictionary::quick_phrase_slot(&self.journal_path(), &code);
            slot = stored.clamp(0, QUICK_PHRASE_MAX_SLOT) as usize;
        }
        local::place_quick_phrases(&mut mixed, convert_all(phrases.items), slot);
        self.mixed_candidates = mixed;
    }

    /// 当前组合可以混入快捷短语时返回它的编码：全拼/双拼、非本地模式、未造词、无光标前缀和辅助码，且原始输入全是小写字母。
    /// 不看辅助码：双拼辅助码会把「完整音节 + 一个字母」的编码（dia）都认成带辅助码，整串精确等于用户编码时按短语处理。
    pub(super) fn quick_phrase_code(&self) -> Option<String> {
        if !self.local_mode_options.quick_phrase_candidates
            || self.local_input_mode != LocalInputMode::None
            || self.dedicated_english_mode
            || (self.scheme() != SchemeType::Quanpin && self.scheme() != SchemeType::Shuangpin)
            || self.prefix_candidates_active
            || !self.immediate_phrase_progress.word.is_empty()
        {
            return None;
        }
        let request = self.engine.request();
        let code = if request.raw_input_with_cases.is_empty() { &request.raw_input } else { &request.raw_input_with_cases };
        if code.is_empty() || !code.bytes().all(|b| b.is_ascii_lowercase()) {
            return None;
        }
        Some(code.clone())
    }

    /// 选中快捷短语时把组前移，越过组选普通候选时把组后移。
    pub(super) fn learn_quick_phrase_order(&self, index: usize) -> Option<String> {
        if !self.local_mode_options.quick_phrase_frequency || !self.frequency_adjustment_configured || self.frequency_adjustment.mode == super::FrequencyAdjustmentMode::Disabled {
            return None;
        }
        let code = self.quick_phrase_code()?;
        let list = self.candidates();
        let selected = &list[index];
        let main_db = self.main_dictionary_path();
        let user_db = self.journal_path();
        let mode = self.frequency_adjustment.mode.name();
        let failure = || Some("Unable to persist quick phrase frequency adjustment.".to_string());
        if selected.source == Source::QuickPhrase {
            let first = list.iter().position(|item| item.source == Source::QuickPhrase);
            let first_in_group = first == Some(index);
            if !user_dictionary::learn_quick_phrase_selection(
                &main_db,
                &user_db,
                &code,
                &selected.word,
                first_in_group,
                mode,
                self.frequency_adjustment.linear_step,
                self.frequency_adjustment.trigger_count,
            ) {
                return failure();
            }
            return None;
        }
        if !local::counts_toward_quick_phrase_slot(selected) || !list.iter().any(|item| item.source == Source::QuickPhrase) {
            return None;
        }
        let ordinary_rank = list[..index].iter().filter(|item| local::counts_toward_quick_phrase_slot(*item)).count();
        if !user_dictionary::learn_quick_phrase_bypass(
            &user_db,
            &code,
            ordinary_rank as i32,
            mode,
            self.frequency_adjustment.linear_step,
            self.frequency_adjustment.trigger_count,
            QUICK_PHRASE_MAX_SLOT,
        ) {
            return failure();
        }
        None
    }

    pub(super) fn position_context(&self, english: bool, wubi: bool) -> String {
        if english {
            let input = if self.dedicated_english_mode {
                self.dedicated_english_preedit.clone()
            } else if self.local_input_mode == LocalInputMode::TemporaryEnglish {
                self.local_preedit.get(1..).unwrap_or("").to_string()
            } else {
                self.engine.request().raw_input_with_cases.clone()
            };
            return format!("english:{}", input.to_ascii_lowercase());
        }
        if self.local_input_mode == LocalInputMode::SuperJianpin {
            return local::jianpin_ranking_context(self.local_preedit.get(1..).unwrap_or(""), self.scheme() == SchemeType::Shuangpin, self.profile);
        }
        if wubi {
            return self.engine.request().raw_input.clone();
        }
        let mut context = self.get_quanpin().to_string();
        if context.is_empty() {
            context = self.get_pinyin_segmentation().to_string();
        }
        if self.engine.request().raw_input.len() == 1 {
            return context;
        }
        let plain: String = context.chars().filter(|c| *c != '\'').collect();
        let cuts = quanpin_query::cut_pinyin_by_mode(&plain, CutMode::Correction);
        match cuts.first() {
            Some(first) => join_segments(first),
            None => context,
        }
    }

    pub(super) fn apply_candidate_positions(&self, items: &mut Vec<Item>) {
        if !self.fixed_positions_enabled || items.is_empty() {
            return;
        }
        let journal = self.journal_path();
        let super_jianpin = self.local_input_mode == LocalInputMode::SuperJianpin;
        let regular = self.local_input_mode == LocalInputMode::None && !self.dedicated_english_mode && self.scheme() != SchemeType::JapaneseRomaji;
        let keep_dynamic = self.has_active_helpcode();
        if regular && self.is_wubi() {
            // 混输组合里五笔与拼音候选共存，固定位置必须按候选自己的上下文分别结算：五笔候选写的是码，拼音候选写的是全拼，
            // 混进一个 context 会互相错配。两个分区各自排序后按「五笔在前、拼音在后」拼回，主方案（五笔）的位置语义原样保留。
            let mut wubi_items: Vec<Item> = Vec::new();
            let mut pinyin_items: Vec<Item> = Vec::new();
            for item in items.drain(..) {
                if Self::is_wubi_native_candidate(&item) {
                    wubi_items.push(item);
                } else {
                    pinyin_items.push(item);
                }
            }
            let include_missing = self.engine.request().raw_input.len() == 1;
            if !wubi_items.is_empty() {
                let find = |key: &str, word: &str| self.engine.find_candidate(SchemeType::Wubi, key, word);
                user_dictionary::apply_fixed_positions(&journal, &self.position_context(false, true), &mut wubi_items, include_missing, Some(&find), keep_dynamic);
            }
            if !pinyin_items.is_empty() {
                let find = |key: &str, word: &str| self.engine.find_candidate(SchemeType::Quanpin, key, word);
                user_dictionary::apply_fixed_positions(&journal, &self.position_context(false, false), &mut pinyin_items, include_missing, Some(&find), keep_dynamic);
            }
            // include_missing 会把固定在拼音上下文的词从词库补回来，它可能正是五笔分区已经列出的词。
            // 合并时同词只留五笔那份，和 ImeSession::refresh_candidates 的去重规则一致。
            pinyin_items.retain(|pinyin_item| !wubi_items.iter().any(|wubi_item| wubi_item.word == pinyin_item.word));
            items.extend(wubi_items);
            items.extend(pinyin_items);
        } else if regular {
            let scheme = self.scheme();
            let find = |key: &str, word: &str| self.engine.find_candidate(scheme, key, word);
            user_dictionary::apply_fixed_positions(&journal, &self.position_context(false, false), items, self.engine.request().raw_input.len() == 1, Some(&find), keep_dynamic);
        } else if super_jianpin {
            user_dictionary::apply_fixed_positions(&journal, &self.position_context(false, false), items, false, None, false);
        }
        if items.iter().any(|item| item.source == Source::EnglishDictionary) {
            user_dictionary::apply_fixed_positions(&journal, &self.position_context(true, false), items, false, None, true);
        }
    }

    pub fn set_candidate_position(&mut self, index: usize, position: i32) -> KeyResult {
        if !(0..=5).contains(&position) || index >= self.candidates().len() {
            return KeyResult::default();
        }
        let selected = self.candidates()[index].clone();
        let english = selected.source == Source::EnglishDictionary;
        if !english && ((selected.source != Source::Database && selected.source != Source::UserDatabase) || self.scheme() == SchemeType::JapaneseRomaji) {
            return KeyResult::default();
        }
        let wubi = Self::is_wubi_native_candidate(&selected) && self.local_input_mode != LocalInputMode::SuperJianpin;
        let context = self.position_context(english, wubi);
        let key = if english || wubi {
            selected.pinyin.clone()
        } else if selected.canonical_pinyin.is_empty() {
            selected.pinyin.clone()
        } else {
            selected.canonical_pinyin.clone()
        };
        if context.is_empty() || key.is_empty() {
            return KeyResult::default();
        }
        let journal = self.journal_path();
        let ok = if position == 0 {
            user_dictionary::clear_fixed_position(&journal, &context, &key, &selected.word)
        } else {
            user_dictionary::set_fixed_position(&journal, &context, &key, &selected.word, position)
        };
        if !ok {
            return KeyResult::with(true, None, Some("Unable to persist candidate position.".to_string()));
        }
        self.reset_cache();
        if self.dedicated_english_mode {
            self.update_dedicated_english_candidates();
        } else if self.local_input_mode != LocalInputMode::None {
            return KeyResult::with(true, None, self.update_local_candidates());
        } else {
            self.recompute_candidates();
        }
        KeyResult::handled()
    }

    pub fn remove_candidate(&mut self, index: usize) -> KeyResult {
        if index >= self.candidates().len() {
            return KeyResult::default();
        }
        let selected = self.candidates()[index].clone();
        let english = selected.source == Source::EnglishDictionary;
        if !english
            && ((selected.source != Source::Database && selected.source != Source::UserDatabase)
                || self.scheme() == SchemeType::JapaneseRomaji
                || helpcode::count_utf8_chars(&selected.word) <= 1)
        {
            return KeyResult::default();
        }

        let wubi = Self::is_wubi_native_candidate(&selected) && self.local_input_mode != LocalInputMode::SuperJianpin;
        let kind = if english {
            DictionaryKind::English
        } else if wubi {
            DictionaryKind::Wubi
        } else {
            DictionaryKind::Pinyin
        };
        // 展示出来的拼音候选已经带着精确的词库键。重新切分（或再展开一次双拼）可能删掉同一个词的另一个读音。
        let key = if english || wubi { &selected.pinyin } else { &selected.canonical_pinyin };
        if key.is_empty() {
            return KeyResult::default();
        }
        let dictionary = if english { self.english_dictionary_path() } else { self.main_dictionary_path() };
        if !user_dictionary::delete_dictionary_candidate(&dictionary, &self.journal_path(), kind, key, &selected.word) {
            return KeyResult::with(true, None, Some("Unable to persist candidate removal.".to_string()));
        }

        self.reset_cache();
        if self.dedicated_english_mode {
            self.update_dedicated_english_candidates();
        } else if self.local_input_mode != LocalInputMode::None {
            return KeyResult::with(true, None, self.update_local_candidates());
        } else {
            self.recompute_candidates();
        }
        KeyResult::handled()
    }

    pub(super) fn update_local_candidates(&mut self) -> Option<String> {
        let scheme = self.scheme();
        let provider = &self.local_date_time_provider;
        let clock = || match provider {
            Some(provider) => provider(),
            None => crate::datetime::current_local_date_time(),
        };
        let result = self.candidate_queries.local(self.local_input_mode, &self.local_preedit, scheme, &clock, self.engine.candidates());
        let mut candidates = result.candidates;
        self.apply_candidate_positions(&mut candidates);
        self.local_candidates = candidates;
        result.diagnostic
    }

    pub(super) fn update_dedicated_english_candidates(&mut self) {
        self.dedicated_english_candidates.clear();
        if self.dedicated_english_preedit.is_empty() {
            return;
        }
        let prefix = self.dedicated_english_preedit.to_ascii_lowercase();
        let mut candidates = convert_all(self.candidate_queries.english_dictionary().query_prefix(&prefix, 1000));
        self.apply_candidate_positions(&mut candidates);
        if candidates.is_empty() {
            candidates.push(Item {
                pinyin: String::new(),
                canonical_pinyin: String::new(),
                word: self.dedicated_english_preedit.clone(),
                weight: 0,
                source: Source::Generated,
                sentence_association: false,
                input_domains: Vec::new(),
                fuzzy: false,
                corrected_from: String::new(),
                scheme: SchemeType::Quanpin,
                fixed_position: 0,
            });
        }
        self.dedicated_english_candidates = candidates;
    }

    /// 把光标前缀解码进 `prefix_candidates`：前缀严格短于原串才解，且按前缀缓存，没变的按键跳过多余的查询。
    pub(super) fn refresh_prefix_candidates(&mut self) {
        self.prefix_candidates_active = false;
        if self.caret.is_none() || self.dedicated_english_mode || self.local_input_mode != LocalInputMode::None {
            self.prefix_candidates.clear();
            self.prefix_query_input.clear();
            return;
        }
        let raw_with_cases = self.get_pinyin_sequence_with_cases().to_string();
        let end = self.quantized_prefix_end();
        if end >= raw_with_cases.len() {
            // 光标停在最后一个边界或方案没有单元模型：手头已有的整串解码就是答案。
            self.prefix_candidates.clear();
            self.prefix_query_input.clear();
            return;
        }
        let cased_prefix = head(&raw_with_cases, end).to_string();
        let prefix = cased_prefix.to_ascii_lowercase();
        if self.prefix_query_input != prefix {
            // 带大小写的前缀保留方案期望的显示大小写；小写副本兼作缓存键（查询键本身总是小写）。
            self.prefix_candidates = self.engine.query_raw_candidates(&prefix, &cased_prefix);
            self.prefix_query_input = prefix;
        }
        // 空前缀（光标在第一个边界或之前）有意解出空候选：宿主隐藏候选窗，预编辑照常显示完整拼音。
        self.prefix_candidates_active = true;
    }

    /// 当前输入带着生效的句中辅助码约束：候选是筛过的，排位不能当调频的参照。
    pub fn has_mid_sentence_helpcode(&self) -> bool {
        (self.mid_sentence_helpcode_enabled || self.direct_helpcode_enabled)
            && !self.dedicated_english_mode
            && self.local_input_mode == LocalInputMode::None
            && !self.request().syllable_helpcodes.is_empty()
    }

    /// 同一输入去掉句中辅助码约束后的引擎候选。用户下次不敲辅助码时看到的就是这份排序，调频要在这份里给选中的词挪位置。
    pub fn candidates_without_mid_sentence_helpcode(&mut self) -> Vec<Item> {
        if !self.has_mid_sentence_helpcode() {
            return self.engine.candidates().to_vec();
        }
        self.engine.query_without_syllable_helpcodes()
    }
}
