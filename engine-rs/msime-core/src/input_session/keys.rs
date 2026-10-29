//! 按键、命令、选词上屏、本地模式字符、学习与调频。对应 `engine/core/input_session.cpp` 的按键与学习部分。

use super::composition::{normalize_canonical_pinyin_for_word, MAX_LEARNED_SENTENCE_SYLLABLES};
use super::{CandidateEdge, Command, FrequencyAdjustmentMode, FrequencyAdjustmentOptions, InputSession, KeyResult, LocalInputMode};
use crate::helpcode;
use crate::input_contracts;
use crate::lattice::{Item, Source};
use crate::local;
use crate::mid_sentence;
use crate::quanpin_query::split_segments;
use crate::request::{key, SchemeType};
use crate::shuangpin_query as sq;
use crate::user_dictionary::{self, DictionaryKind, WordItem as UserItem};

fn to_user_item(item: &Item) -> UserItem {
    UserItem {
        pinyin: item.pinyin.clone(),
        canonical_pinyin: item.canonical_pinyin.clone(),
        word: item.word.clone(),
        weight: item.weight,
        source: item.source,
        fixed_position: item.fixed_position,
    }
}

fn to_user_items(items: &[Item]) -> Vec<UserItem> {
    items.iter().map(to_user_item).collect()
}

/// C++ 的 `char` 是字节：非 ASCII 的字符按一个既不是字母也不是标点的字节处理。
fn byte_of(character: char) -> u8 {
    if character.is_ascii() {
        character as u8
    } else {
        0x80
    }
}

impl<'a> InputSession<'a> {
    /// 喂一个小写 ASCII 字母或组合中的撇号。其他输入作为未处理返回，前端把它透传给客户端应用。
    pub fn handle_character(&mut self, character: char, shift_only: bool) -> KeyResult {
        let c = byte_of(character);
        if self.caret_position() < self.editing_text().len() {
            return self.insert_at_caret(c);
        }
        self.caret = None;
        if self.dedicated_english_mode {
            if !c.is_ascii_alphabetic() {
                return KeyResult::handled();
            }
            self.dedicated_english_preedit.push(c as char);
            self.update_dedicated_english_candidates();
            return KeyResult::handled();
        }
        if self.local_input_mode != LocalInputMode::None {
            return self.handle_local_character(c);
        }

        let pinyin_scheme = matches!(self.scheme(), SchemeType::Quanpin | SchemeType::Shuangpin);
        if shift_only && !self.has_composition() && pinyin_scheme {
            let options = self.local_mode_options;
            let trigger = match c {
                b'U' if options.unicode => Some(LocalInputMode::Unicode),
                b'T' if options.date_time => Some(LocalInputMode::DateTime),
                b'K' if options.quick_phrase => Some(LocalInputMode::QuickPhrase),
                b'E' if options.emoji => Some(LocalInputMode::Emoji),
                b'M' if options.kaomoji => Some(LocalInputMode::Kaomoji),
                b'J' if options.super_jianpin => Some(LocalInputMode::SuperJianpin),
                b'Y' if options.temporary_english => Some(LocalInputMode::TemporaryEnglish),
                b'R' if options.temporary_japanese => Some(LocalInputMode::TemporaryJapanese),
                _ => None,
            };
            if let Some(mode) = trigger {
                if mode == LocalInputMode::TemporaryJapanese {
                    self.temporary_original_scheme = Some(self.scheme());
                    self.engine.switch_scheme(SchemeType::JapaneseRomaji);
                }
                self.local_input_mode = mode;
                self.local_preedit = (c as char).to_string();
                self.local_candidates.clear();
                return KeyResult::handled();
            }
        }

        let lowercase_letter = c.is_ascii_lowercase();
        let semicolon_final = c == b';' && self.scheme() == SchemeType::Shuangpin && self.profile.uses_semicolon_final();
        let mid_sentence_marker = c == mid_sentence::MARKER && self.has_composition() && self.accepts_mid_sentence_helpcode_marker();
        // 句中辅助码：反引号后的第一码大小写都收，紧跟的大写字母是第二码，与辅助码开关无关。
        let typed_length = self.get_pinyin_sequence_with_cases().len();
        let mid_sentence_code = self.accepts_mid_sentence_code_at(typed_length, c);
        // 直接辅助码：辅码可以用大写敲（万象的大写变体），四码后接 / 终止。
        let direct_slash = c == b'/' && self.has_composition() && self.accepts_direct_helpcode_slash_at(typed_length);
        let active_helpcode = c.is_ascii_uppercase()
            && self.has_composition()
            && ((self.scheme() == SchemeType::Quanpin && self.quanpin_helpcode_enabled)
                || (self.scheme() == SchemeType::Shuangpin && (self.shuangpin_helpcode_enabled || self.direct_helpcode_enabled)));
        if !lowercase_letter && !active_helpcode && !mid_sentence_code && !mid_sentence_marker && c != b'\'' && !semicolon_final && !direct_slash {
            return KeyResult::default();
        }
        if c == b'\'' && !self.has_composition() {
            return KeyResult::default();
        }

        let previous_preedit = self.preedit().to_string();
        let key_code = if c == b'\'' {
            key::APOSTROPHE
        } else if direct_slash {
            key::SLASH
        } else if mid_sentence_marker {
            key::BACKQUOTE
        } else if semicolon_final {
            key::SEMICOLON
        } else {
            u32::from(c.to_ascii_uppercase())
        };
        self.engine.handle_key(key_code, 0, u16::from(c));
        self.update_mixed_candidates();
        let handled = self.preedit() != previous_preedit;
        if handled {
            self.online_requests.invalidate();
        }
        KeyResult::with(handled, None, None)
    }

    /// 宿主在决定是否把 / 当作编码键（四码后的终止键）之前问这一句。
    pub fn accepts_direct_helpcode_slash_at(&self, caret: usize) -> bool {
        if !self.direct_helpcode_enabled || !self.direct_helpcode_slash_marker || !self.is_shuangpin() || self.dedicated_english_mode || self.local_input_mode != LocalInputMode::None {
            return false;
        }
        input_contracts::direct_helpcode_accepts_slash_at(self.get_pinyin_sequence_with_cases().as_bytes(), caret)
    }

    /// 宿主在决定是否把反引号当作编码键之前问这一句：开关开着、双拼，且光标前这一节能接一段句中辅助码。
    pub fn accepts_mid_sentence_helpcode_marker(&self) -> bool {
        self.accepts_mid_sentence_helpcode_marker_at(self.caret_position())
    }

    pub fn accepts_mid_sentence_helpcode_marker_at(&self, caret: usize) -> bool {
        // 直接辅助码开着时反引号不再是编码键：两套辅码同时生效，同一个音节会被约束两遍。
        if !self.mid_sentence_helpcode_enabled || self.direct_helpcode_enabled || !self.is_shuangpin() || self.dedicated_english_mode || self.local_input_mode != LocalInputMode::None {
            return false;
        }
        sq::accepts_mid_sentence_helpcode_marker_at(self.get_pinyin_sequence_with_cases(), caret, self.engine.mid_sentence_uppercase_trigger_active())
    }

    /// 光标停在 `caret` 处时大写字母 `character` 能否作为句中辅助码收下：紧跟在反引号之后（第一码），
    /// 或紧跟在「反引号 + 第一码」之后（第二码）。
    pub(super) fn accepts_mid_sentence_code_at(&self, caret: usize, character: u8) -> bool {
        let typed = self.get_pinyin_sequence_with_cases();
        if !self.mid_sentence_helpcode_enabled || self.direct_helpcode_enabled || !self.is_shuangpin() || !character.is_ascii_uppercase() || caret == 0 || caret > typed.len() {
            return false;
        }
        let uppercase_trigger = self.engine.mid_sentence_uppercase_trigger_active();
        let before = typed.get(..caret).unwrap_or("");
        // 反引号后的第一码、只有第一码的段后的第二码，或（大写触发开着时）完整音节后自己开一段。
        before.as_bytes().last() == Some(&mid_sentence::MARKER)
            || sq::accepts_mid_sentence_second_code(before, character, uppercase_trigger)
            || (uppercase_trigger && mid_sentence::accepts_marker_at(typed.as_bytes(), caret, true))
    }

    /// 1-9 选词键。
    pub fn handle_candidate_key(&mut self, character: char) -> KeyResult {
        if !self.has_composition() || !('1'..='9').contains(&character) {
            return KeyResult::default();
        }
        self.select_candidate(character as usize - '1' as usize)
    }

    pub fn handle_punctuation(&mut self, character: char) -> KeyResult {
        if !self.chinese_punctuation_enabled {
            return KeyResult::default();
        }
        let Some(punctuation) = self.punctuation.translate(character) else { return KeyResult::default() };
        let mut result = self.finish_composition(0);
        result.handled = true;
        let mut text = result.commit.take().unwrap_or_default();
        text.push_str(punctuation);
        result.commit = Some(text);
        result
    }

    /// 作用在现有组合上的命令；没有组合时一律不处理，退格和退出键照常交给客户端应用。
    pub fn handle_command(&mut self, command: Command) -> KeyResult {
        if !self.has_composition() {
            return KeyResult::default();
        }
        match command {
            Command::MoveLeft | Command::MoveRight | Command::MoveHome | Command::MoveEnd | Command::DeleteForward => self.edit_at_caret(command),
            Command::Backspace => {
                if self.caret_position() < self.editing_text().len() {
                    return self.edit_at_caret(command);
                }
                self.caret = None;
                if self.dedicated_english_mode {
                    self.dedicated_english_preedit.pop();
                    self.update_dedicated_english_candidates();
                    return KeyResult::handled();
                }
                if self.local_input_mode != LocalInputMode::None {
                    let mut diagnostic = None;
                    if self.local_preedit.len() <= 1 {
                        self.reset_composition();
                    } else if self.local_input_mode == LocalInputMode::TemporaryJapanese {
                        self.engine.handle_key(key::BACKSPACE, 0, 0);
                        self.local_preedit = format!("R{}", self.engine.preedit());
                        self.local_candidates = self.engine.candidates().to_vec();
                    } else {
                        self.local_preedit.pop();
                        diagnostic = self.update_local_candidates();
                    }
                    return KeyResult::with(true, None, diagnostic);
                }
                self.engine.handle_key(key::BACKSPACE, 0, 0);
                self.update_mixed_candidates();
                self.discard_abandoned_phrase_progress();
                self.online_requests.invalidate();
                KeyResult::handled()
            }
            Command::CommitCandidate => self.commit(0),
            Command::CommitRaw => {
                let mut raw = self.preedit().to_string();
                if matches!(self.local_input_mode, LocalInputMode::TemporaryEnglish | LocalInputMode::TemporaryJapanese) && !raw.is_empty() {
                    raw.remove(0);
                }
                let mut diagnostic = None;
                if self.dedicated_english_mode
                    && self.candidate_learning_enabled
                    && !user_dictionary::learn_entered_english_word(&self.english_dictionary_path(), &self.journal_path(), &raw, 10)
                {
                    diagnostic = Some("English word could not be learned.".to_string());
                }
                self.reset_composition();
                KeyResult::with(true, Some(raw), diagnostic)
            }
            Command::Cancel => {
                self.reset_composition();
                KeyResult::handled()
            }
        }
    }

    /// 提交选中的前缀并保留未消耗的拼音；宿主上屏 `commit`，再从会话渲染剩下的预编辑。
    pub fn select_candidate(&mut self, index: usize) -> KeyResult {
        if !self.has_composition() || index >= self.candidates().len() {
            return KeyResult::default();
        }
        self.commit(index)
    }

    /// 标点、方案切换和宿主透传时把剩下的输入全部上屏。
    pub fn finish_composition(&mut self, mut first_index: usize) -> KeyResult {
        let mut result = KeyResult::default();
        while self.has_composition() {
            let part = self.commit(first_index);
            first_index = 0;
            result.handled = true;
            if let Some(text) = part.commit {
                result.commit.get_or_insert_with(String::new).push_str(&text);
            }
            if part.diagnostic.is_some() {
                result.diagnostic = part.diagnostic;
            }
        }
        result
    }

    pub fn select_candidate_by_word(&mut self, candidate: &str) -> KeyResult {
        let Some(index) = self.candidates().iter().position(|item| item.word == candidate) else { return KeyResult::default() };
        self.commit(index)
    }

    pub fn select_candidate_edge(&mut self, index: usize, edge: CandidateEdge) -> KeyResult {
        if !self.has_composition() || index >= self.candidates().len() {
            return KeyResult::default();
        }
        let candidate = &self.candidates()[index].word;
        let character = match edge {
            CandidateEdge::FirstHan => helpcode::first_han_char(candidate),
            CandidateEdge::LastHan => helpcode::last_han_char(candidate),
        };
        if character.is_empty() {
            return KeyResult::default();
        }
        self.reset_composition();
        KeyResult::with(true, Some(character), None)
    }

    pub fn pin_candidate(&mut self, index: usize) -> KeyResult {
        if index >= self.candidates().len() {
            return KeyResult::default();
        }
        let source = self.candidates()[index].source;
        if source != Source::EnglishDictionary && ((source != Source::Database && source != Source::UserDatabase) || self.scheme() == SchemeType::JapaneseRomaji) {
            return KeyResult::default();
        }
        // 手动置顶独立于自动学习的偏好，也从不上屏。
        let diagnostic = self.adjust_candidate_frequency(index, FrequencyAdjustmentOptions { mode: FrequencyAdjustmentMode::Pin, trigger_count: 1, linear_step: 1 }, true);
        if diagnostic.is_some() {
            return KeyResult::with(true, None, diagnostic);
        }
        self.reset_cache();
        if self.dedicated_english_mode {
            self.update_dedicated_english_candidates();
            return KeyResult::handled();
        }
        if self.local_input_mode != LocalInputMode::None {
            let diagnostic = self.update_local_candidates();
            return KeyResult::with(true, None, diagnostic);
        }
        self.recompute_candidates();
        KeyResult::handled()
    }

    fn commit(&mut self, index: usize) -> KeyResult {
        self.caret = None;
        let mut text = None;
        let mut selected = None;
        if index < self.candidates().len() {
            let item = self.candidates()[index].clone();
            text = Some(item.word.clone());
            selected = Some(item);
        } else if !(matches!(self.local_input_mode, LocalInputMode::TemporaryEnglish | LocalInputMode::TemporaryJapanese) && self.local_preedit.len() == 1) {
            text = Some(self.preedit().to_string());
        }
        let mut diagnostic = self.learn_candidate(index);
        if let Some(selected) = &selected {
            if self.local_input_mode == LocalInputMode::None
                && !self.dedicated_english_mode
                && !self.is_japanese()
                && !Self::is_wubi_native_candidate(selected)
                && (selected.source == Source::Database || selected.source == Source::UserDatabase)
            {
                // 从五笔混输组合里选拼音答案，把它的剩余部分交给拼音：尾巴也许是五笔表认识的码，但用户还在敲的拼写不能丢。
                // 五笔候选则一次消耗整串编码，由下面的 reset_composition 处理。
                let transition = self.advance_composition_after_selection(&selected.pinyin, &selected.word, &selected.canonical_pinyin, selected.scheme);
                let progress = self.update_creating_word_progress(&self.immediate_phrase_progress.pinyin, &self.immediate_phrase_progress.word, &selected.word, &transition);
                if transition.continues_composition {
                    self.immediate_phrase_progress = progress;
                    self.discard_abandoned_phrase_progress();
                    return KeyResult::with(true, text, diagnostic);
                }
                if !self.immediate_phrase_progress.word.is_empty()
                    && progress.can_store
                    && self.candidate_learning_enabled
                    && self.store_user_phrase_from_canonical_pinyin(&progress.pinyin, &progress.word) != 0
                {
                    diagnostic = Some("Unable to persist the composed phrase.".to_string());
                }
            }
        }
        self.reset_composition();
        KeyResult::with(true, text, diagnostic)
    }

    fn handle_local_character(&mut self, c: u8) -> KeyResult {
        let push_and_update = |session: &mut Self| {
            session.local_preedit.push(c as char);
            let diagnostic = session.update_local_candidates();
            KeyResult::with(true, None, diagnostic)
        };
        match self.local_input_mode {
            LocalInputMode::TemporaryEnglish => {
                if !c.is_ascii_alphabetic() {
                    return KeyResult::default();
                }
                push_and_update(self)
            }
            LocalInputMode::TemporaryJapanese => {
                if !c.is_ascii_alphabetic() && c != b'\'' {
                    return KeyResult::default();
                }
                let key_code = if c == b'\'' { key::APOSTROPHE } else { u32::from(c.to_ascii_uppercase()) };
                self.engine.handle_key(key_code, 0, u16::from(c));
                self.local_preedit = format!("R{}", self.engine.preedit());
                self.local_candidates = self.engine.candidates().to_vec();
                KeyResult::handled()
            }
            LocalInputMode::SuperJianpin => {
                if !c.is_ascii_alphabetic() {
                    return KeyResult::handled();
                }
                push_and_update(self)
            }
            LocalInputMode::Emoji | LocalInputMode::Kaomoji => {
                if !c.is_ascii_alphabetic() && c != b'\'' {
                    return KeyResult::handled();
                }
                push_and_update(self)
            }
            LocalInputMode::QuickPhrase => {
                if !c.is_ascii_lowercase() {
                    return KeyResult::handled();
                }
                push_and_update(self)
            }
            LocalInputMode::DateTime => {
                // 唤醒词只收小写字母；数字开头的指定日期时间收数字、/ 和 :，形状与 TSF、Server 共用 date_time_input.h。
                let keyword_letter = c.is_ascii_lowercase() && self.local_preedit.bytes().skip(1).all(|b| b.is_ascii_lowercase());
                if !keyword_letter && !input_contracts::date_time_accepts_at(self.local_preedit.as_bytes(), self.local_preedit.len(), c) {
                    return KeyResult::handled();
                }
                push_and_update(self)
            }
            LocalInputMode::Unicode => {
                let optional_plus = c == b'+' && self.local_preedit == "U";
                if !optional_plus && !c.is_ascii_hexdigit() {
                    return KeyResult::handled();
                }
                push_and_update(self)
            }
            LocalInputMode::None => KeyResult::default(),
        }
    }

    // ------------------------------------------------------------------------------------------
    // 学习与调频
    // ------------------------------------------------------------------------------------------

    fn learn_candidate(&mut self, index: usize) -> Option<String> {
        if !self.candidate_learning_enabled || index >= self.candidates().len() {
            return None;
        }
        let temporary_english = self.local_input_mode == LocalInputMode::TemporaryEnglish;
        if (self.dedicated_english_mode || temporary_english)
            && self.candidates()[index].source == Source::EnglishDictionary
            && self.frequency_adjustment_configured
            && self.frequency_adjustment.mode != FrequencyAdjustmentMode::Disabled
            && index != 0
        {
            return self.adjust_candidate_frequency(index, self.frequency_adjustment, false);
        }

        let selected = self.candidates()[index].clone();
        let quick_phrase_diagnostic = self.learn_quick_phrase_order(index);
        if selected.source == Source::QuickPhrase {
            return quick_phrase_diagnostic;
        }
        // 整句候选走造词，不走调频：它在词库里没有行可改。不受 frequency_adjustment 约束（那是候选词频策略），
        // 也不跳过 index == 0——首位整句同样没有落库，下次仍要靠猜，存下来才稳定。
        if matches!(selected.source, Source::Generated | Source::Fallback | Source::NeuralDesktop | Source::NeuralKeyboard | Source::Collocation) {
            return self.learn_sentence_candidate(&selected);
        }
        if !self.frequency_adjustment_configured {
            if selected.source == Source::Database || selected.source == Source::UserDatabase {
                let pinyin = if selected.canonical_pinyin.is_empty() { &selected.pinyin } else { &selected.canonical_pinyin };
                let _ = self.engine.update_weight_by_pinyin_and_word(selected.scheme, pinyin, &selected.word);
            }
            return None;
        }
        // 句中辅助码筛过的列表里排第一的词，不敲辅助码时未必排第一：排位参照换成不带约束的候选，选的是第一个也照样调。
        if self.frequency_adjustment.mode != FrequencyAdjustmentMode::Disabled
            && self.has_mid_sentence_helpcode()
            && (selected.source == Source::Database || selected.source == Source::UserDatabase)
        {
            let ranked = self.candidates_without_mid_sentence_helpcode();
            return self.adjust_pinyin_candidate_frequency(&selected, &ranked, self.frequency_adjustment, false);
        }
        if self.frequency_adjustment.mode == FrequencyAdjustmentMode::Disabled || index == 0 {
            return None;
        }
        if (selected.source != Source::Database && selected.source != Source::UserDatabase) || self.engine.current_scheme_type() == SchemeType::JapaneseRomaji {
            return None;
        }
        self.adjust_candidate_frequency(index, self.frequency_adjustment, false)
    }

    /// 词格 / Google 解码器猜出来的整句在词库里没有对应行，选中后落成一条用户词组。
    fn learn_sentence_candidate(&mut self, selected: &Item) -> Option<String> {
        // 本地模式和英文模式也用 Generated 装自己的候选，那些不是拼音整句，不能往拼音用户词库里塞。
        // 五笔码表候选也不进这里：混输追加的整句候选自带全拼方案，按候选自己的方案判断，不看会话方案。
        if self.local_input_mode != LocalInputMode::None || self.dedicated_english_mode || self.is_japanese() || Self::is_wubi_native_candidate(selected) {
            return None;
        }
        // 读音必须完整且音节数与字数对得上，否则落库的是个读音残缺的词条。双拼的整句候选同样带 canonical 全拼，这里统一按全拼键处理。
        let canonical = normalize_canonical_pinyin_for_word(&selected.canonical_pinyin, &selected.word);
        if canonical.is_empty() || split_segments(&canonical).len() > MAX_LEARNED_SENTENCE_SYLLABLES {
            return None;
        }
        if self.store_user_phrase_from_canonical_pinyin(&canonical, &selected.word) != 0 {
            return Some("Unable to persist the selected sentence.".to_string());
        }
        None
    }

    fn adjust_candidate_frequency(&mut self, index: usize, options: FrequencyAdjustmentOptions, force_top: bool) -> Option<String> {
        let selected = self.candidates()[index].clone();
        if selected.source == Source::EnglishDictionary {
            let context = if self.dedicated_english_mode {
                self.dedicated_english_preedit.clone()
            } else if self.local_input_mode == LocalInputMode::TemporaryEnglish {
                self.local_preedit.get(1..).unwrap_or("").to_string()
            } else {
                self.engine.request().raw_input.clone()
            };
            let context = context.to_ascii_lowercase();
            let ranked: Vec<UserItem> = self.candidates().iter().filter(|candidate| candidate.source == Source::EnglishDictionary).map(to_user_item).collect();
            let adjusted = user_dictionary::adjust_english_candidate_ranking(
                &self.english_dictionary_path(),
                &self.journal_path(),
                &format!("english:{context}"),
                &ranked,
                &selected.pinyin,
                &selected.word,
                options.mode.name(),
                options.linear_step,
                options.trigger_count,
                force_top,
                None,
            );
            return if adjusted { None } else { Some("English candidate frequency could not be persisted.".to_string()) };
        }
        let ranked = self.candidates().to_vec();
        self.adjust_pinyin_candidate_frequency(&selected, &ranked, options, force_top)
    }

    /// 拼音候选的调频，排位参照 `ranked` 而不是当前列表；句中辅助码组合用不带约束的候选作参照。
    fn adjust_pinyin_candidate_frequency(&mut self, selected: &Item, ranked: &[Item], options: FrequencyAdjustmentOptions, force_top: bool) -> Option<String> {
        let super_jianpin = self.local_input_mode == LocalInputMode::SuperJianpin;
        // 五笔表答不出的码带的是全拼词，按拼音排位、记键、存储；只有五笔表答了的码才按码本身排位。混输组合里两种候选同时在，
        // 所以按选中候选自己的方案走。五笔组合里的拼音候选复用固定位置写入时的上下文，否则置顶了的候选在这里认不出来。
        let wubi = Self::is_wubi_native_candidate(selected);
        let pinyin_candidate = self.is_wubi() && !wubi;
        let mut context_key = if super_jianpin {
            local::jianpin_ranking_context(self.local_preedit.get(1..).unwrap_or(""), self.scheme() == SchemeType::Shuangpin, self.profile)
        } else if wubi {
            self.engine.request().raw_input.clone()
        } else if pinyin_candidate {
            self.position_context(false, false)
        } else {
            self.engine.request().normalized_segmentation.clone()
        };
        if !super_jianpin && context_key.is_empty() {
            context_key = self.engine.request().segmentation.clone();
        }
        let entry_key = if wubi && !super_jianpin {
            selected.pinyin.clone()
        } else if selected.canonical_pinyin.is_empty() {
            context_key.clone()
        } else {
            selected.canonical_pinyin.clone()
        };
        let mut ranking_changed = false;
        let adjusted = user_dictionary::adjust_candidate_ranking(
            &self.main_dictionary_path(),
            &self.journal_path(),
            &context_key,
            &to_user_items(ranked),
            &entry_key,
            &selected.word,
            options.mode.name(),
            options.linear_step,
            options.trigger_count,
            force_top,
            Some(&mut ranking_changed),
            if wubi && !super_jianpin { DictionaryKind::Wubi } else { DictionaryKind::Pinyin },
        );
        if !adjusted {
            return Some("Unable to persist candidate frequency adjustment.".to_string());
        }
        if ranking_changed {
            self.engine.reset_cache();
        }
        None
    }
}

