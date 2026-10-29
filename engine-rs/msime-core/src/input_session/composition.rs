//! 预编辑与切分的读出、选词后的推进、云查询状态、造词进度。对应 `engine/core/input_session_composition.cpp`。

use super::{CloudQueryState, CreatingWordProgress, InputSession, LocalInputMode, SelectionTransition, ShuangpinPreeditForms};
use crate::autocorrect::{self, Cut};
use crate::helpcode;
use crate::lattice::Item;
use crate::quanpin_dictionary::{QuanpinDictionary, ERROR_CODE};
use crate::quanpin_query::{self, join_segments, split_segments};
use crate::quanpin_utils;
use crate::request::{QueryRequest, SchemeType};
use crate::romaji;
use crate::segment;
use crate::shuangpin::Profile;
use crate::shuangpin_query as sq;

/// 整句落库的长度上限。词库里的短语本身就不超过这个音节数，再长的整句只是这一次输入的产物。
pub(super) const MAX_LEARNED_SENTENCE_SYLLABLES: usize = 7;

fn remove_delimiters(segmented: &str) -> String {
    segmented.chars().filter(|c| *c != '\'').collect()
}

fn remove_consumed_leading_separators(raw_input: &mut String, raw_input_with_cases: &mut String) {
    let count = raw_input.bytes().take_while(|b| *b == b'\'').count();
    raw_input.drain(..count);
    let count = raw_input_with_cases.bytes().take_while(|b| *b == b'\'').count();
    raw_input_with_cases.drain(..count);
}

/// C++ `substr(start)`：起点越界时退成空串而不是抛异常。
fn tail(text: &str, start: usize) -> &str {
    text.get(start..).unwrap_or("")
}

/// C++ `substr(0, length)`，长度按字节，越界取到末尾。
pub(super) fn head(text: &str, length: usize) -> &str {
    let mut end = length.min(text.len());
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

fn slice(text: &str, start: usize, length: usize) -> &str {
    head(tail(text, start), length)
}

pub(super) fn normalize_canonical_pinyin_for_word(pinyin: &str, word: &str) -> String {
    if pinyin.is_empty() {
        return String::new();
    }
    let segments = split_segments(pinyin);
    if segments.is_empty() || segments.len() != helpcode::count_han_chars(word) {
        return String::new();
    }
    for segment in &segments {
        if segment.is_empty() || !quanpin_query::is_complete_pinyin_input(segment) {
            return String::new();
        }
    }
    join_segments(&segments)
}

fn append_canonical_pinyin(prefix: &str, suffix: &str) -> String {
    if prefix.is_empty() {
        return suffix.to_string();
    }
    if suffix.is_empty() {
        return String::new();
    }
    format!("{prefix}'{suffix}")
}

/// 句中辅助码：选词推进是在去掉反引号段的串上算的，剩下的部分要换回原串，后面音节上的约束才留得住。
struct MidSentenceRest {
    consumed: String,
    rest: String,
}

fn remap_mid_sentence_rest(request: &QueryRequest, typed: &str, mut clean_begin: usize, mut clean_end: usize, profile: &Profile) -> MidSentenceRest {
    // 方案层或直接辅助码的解析器已经把映射填进请求，直接用它；原串的段规则取决于当时的触发设置，这里不再反推。
    let (clean, source_index) = if request.has_syllable_helpcode_layout {
        (request.raw_input_with_cases.clone(), request.syllable_helpcode_source_index.clone())
    } else {
        let parsed = sq::parse_mid_sentence_helpcodes(typed, profile, false);
        (parsed.input, parsed.source_index)
    };
    clean_end = clean_end.min(clean.len());
    while clean_begin < clean_end && clean.as_bytes()[clean_begin] == b'\'' {
        clean_begin += 1;
    }
    if clean_begin >= clean_end {
        return MidSentenceRest { consumed: typed.to_string(), rest: String::new() };
    }
    let full_begin = source_index[clean_begin];
    let full_end = if clean_end >= clean.len() { typed.len() } else { source_index[clean_end] };
    MidSentenceRest { consumed: head(typed, full_begin).to_string(), rest: slice(typed, full_begin, full_end - full_begin).to_string() }
}

fn lowercase_letters(text: &str) -> String {
    text.to_ascii_lowercase()
}

struct ShuangpinCompositionBase {
    raw_input: String,
    raw_input_with_cases: String,
    effective_raw_input: String,
    effective_raw_input_with_cases: String,
    helpcode_length: usize,
}

fn resolve_shuangpin_composition_base(request: &QueryRequest, profile: &Profile) -> ShuangpinCompositionBase {
    let mut base = ShuangpinCompositionBase {
        raw_input: request.raw_input.clone(),
        raw_input_with_cases: if request.raw_input_with_cases.is_empty() { request.raw_input.clone() } else { request.raw_input_with_cases.clone() },
        effective_raw_input: String::new(),
        effective_raw_input_with_cases: String::new(),
        helpcode_length: 0,
    };
    base.effective_raw_input = sq::remove_manual_delimiters(&base.raw_input);
    base.effective_raw_input_with_cases = sq::remove_manual_delimiters(&base.raw_input_with_cases);

    if !request.enable_shuangpin_helpcode || base.effective_raw_input.is_empty() {
        return base;
    }

    let has_complete_unseparated_base = |base: &ShuangpinCompositionBase, helpcode_length: usize| {
        if base.effective_raw_input.len() <= helpcode_length {
            return false;
        }
        let pure_length = base.effective_raw_input.len() - helpcode_length;
        let raw_prefix_length = sq::raw_length_for_effective_prefix(&base.raw_input, pure_length);
        // 撇号紧挨在后缀前面，说明那段是用户自己分出来的拼音，不是辅助码。
        if raw_prefix_length < base.raw_input.len() && base.raw_input.as_bytes()[raw_prefix_length] == b'\'' {
            return false;
        }
        sq::is_complete_input(head(&base.raw_input, raw_prefix_length), profile)
    };

    if sq::detect_active_double_helpcode_length(&base.raw_input, &base.raw_input_with_cases, profile) == 2 {
        base.helpcode_length = 2;
        return base;
    }

    if base.effective_raw_input.len() % 2 == 1 && base.effective_raw_input.len() > 1 && has_complete_unseparated_base(&base, 1) {
        base.helpcode_length = 1;
    }
    base
}

fn has_active_quanpin_helpcode(request: &QueryRequest) -> bool {
    request.enable_quanpin_helpcode && quanpin_utils::detect_active_helpcode_length(&request.raw_input, &request.raw_input_with_cases) > 0
}

fn resolve_shuangpin_cloud_cache_key(request: &QueryRequest, profile: &Profile) -> String {
    let base = resolve_shuangpin_composition_base(request, profile);
    if base.helpcode_length > 0 && base.effective_raw_input.len() >= base.helpcode_length {
        let base_length = base.effective_raw_input.len() - base.helpcode_length;
        return head(&base.raw_input, sq::raw_length_for_effective_prefix(&base.raw_input, base_length)).to_string();
    }
    base.raw_input
}

fn resolve_quanpin_cloud_cache_key(request: &QueryRequest) -> String {
    quanpin_utils::strip_active_helpcodes(&request.raw_input, &request.raw_input_with_cases)
}

/// 请求里每个用户可见的开关一个布尔，开关 → 掩码的规则在 `autocorrect::autocorrect_types_from_switches`，
/// 预编辑重建因此和查询看到同一个纠错空间。
pub(super) fn quanpin_autocorrect_types(request: &QueryRequest) -> u32 {
    autocorrect::autocorrect_types_from_switches(
        request.enable_quanpin_autocorrect_transposition,
        request.enable_quanpin_autocorrect_neighbor,
        request.enable_quanpin_autocorrect_deletion,
        request.enable_quanpin_autocorrect_insertion,
    )
}

fn quanpin_letters_without_delimiters(text: &str) -> String {
    remove_delimiters(text)
}

/// 纠错显示比较前的折叠：转小写、去手动分隔符，u 的 ü 写法 v 并进 u。
fn fold_quanpin_autocorrect_letters(text: &str) -> String {
    text.chars()
        .filter(|c| *c != '\'')
        .map(|c| {
            let lower = c.to_ascii_lowercase();
            if lower == 'v' {
                'u'
            } else {
                lower
            }
        })
        .collect()
}

fn cut_syllable_letters(cut: &Cut) -> String {
    let letters: String = cut.segments.iter().map(|segment| segment.syllable.as_str()).collect();
    fold_quanpin_autocorrect_letters(&letters)
}

/// 用带大小写的输入字母重建预编辑，在列出的字母数之后加分隔符（升序，最后一段末尾不加）。
fn rebuild_quanpin_display_at_boundaries(cased_input: &str, boundaries: &[usize]) -> String {
    let mut display = String::with_capacity(cased_input.len() + boundaries.len());
    let mut letter_index = 0;
    let mut boundary_index = 0;
    for ch in cased_input.chars() {
        if ch == '\'' {
            continue;
        }
        display.push(ch);
        letter_index += 1;
        if boundary_index < boundaries.len() && letter_index == boundaries[boundary_index] {
            display.push('\'');
            boundary_index += 1;
        }
    }
    display
}

fn rebuild_quanpin_display_from_cut(cased_input: &str, cut: &Cut) -> String {
    let mut boundaries = Vec::new();
    for i in 0..cut.segments.len().saturating_sub(1) {
        boundaries.push(cut.segments[i].start + cut.segments[i].raw.len());
    }
    rebuild_quanpin_display_at_boundaries(cased_input, &boundaries)
}

/// 通过简拼守卫的拼写，按守卫的读法分隔：完整音节加至多一个末尾字母。读不出来时返回空串。
fn rebuild_quanpin_display_from_legal_spelling(cased_input: &str, raw_input: &str) -> String {
    let mut segments = segment::cut_min_segments(raw_input).unwrap_or_default();
    if segments.is_empty() && raw_input.len() > 1 {
        segments = segment::cut_min_segments(head(raw_input, raw_input.len() - 1)).unwrap_or_default();
        if !segments.is_empty() {
            segments.push(tail(raw_input, raw_input.len() - 1).to_string());
        }
    }
    if segments.is_empty() {
        return String::new();
    }
    let mut boundaries = Vec::new();
    let mut end = 0;
    for segment in segments.iter().take(segments.len() - 1) {
        end += segment.len();
        boundaries.push(end);
    }
    rebuild_quanpin_display_at_boundaries(cased_input, &boundaries)
}

/// 预编辑必须始终显示用户真正敲的字母。方案层的别名表（sahng -> shang）和词典层的纠错 BFS（shabg -> shang，
/// 只重新分隔）都可能改写字母，这里按原字母在实际切分处重建。
fn build_quanpin_autocorrect_display(request: &QueryRequest) -> String {
    let cased = if request.raw_input_with_cases.is_empty() { &request.raw_input } else { &request.raw_input_with_cases };
    let base = if request.raw_segmentation.is_empty() { cased } else { &request.raw_segmentation };
    if request.raw_input.is_empty() || cased.is_empty() {
        return base.clone();
    }

    let types = quanpin_autocorrect_types(request);
    let folded_input = fold_quanpin_autocorrect_letters(cased);
    let letters_rewritten = fold_quanpin_autocorrect_letters(&quanpin_letters_without_delimiters(base)) != folded_input;

    if !letters_rewritten && (types == 0 || quanpin_query::is_complete_pinyin_input(&request.raw_input)) {
        return base.clone();
    }

    if autocorrect::looks_like_jianpin_spelling(&request.raw_input) {
        if !letters_rewritten {
            return base.clone();
        }
        let display = rebuild_quanpin_display_from_legal_spelling(cased, &request.raw_input);
        return if display.is_empty() { cased.clone() } else { display };
    }

    let cut = autocorrect::autocorrect_cut_detail(&folded_input, types);
    if !cut.segments.is_empty() && (!letters_rewritten || cut_syllable_letters(&cut) == fold_quanpin_autocorrect_letters(&quanpin_letters_without_delimiters(base))) {
        return rebuild_quanpin_display_from_cut(cased, &cut);
    }
    if letters_rewritten {
        cased.clone()
    } else {
        base.clone()
    }
}

/// 首选是同一串字母的另一种字面切分时，预编辑跟着首选分，而不是停在方案层的最少音节切分上。
fn build_quanpin_display_from_literal_head(request: &QueryRequest, head_item: &Item) -> String {
    let cased = if request.raw_input_with_cases.is_empty() { &request.raw_input } else { &request.raw_input_with_cases };
    if !head_item.corrected_from.is_empty() || request.raw_input.contains('\'') || cased.len() != request.raw_input.len() {
        return String::new();
    }
    let syllables = split_segments(&head_item.pinyin);
    let scheme_segmentation = if request.normalized_segmentation.is_empty() { &request.segmentation } else { &request.normalized_segmentation };
    if syllables.len() < 2 || syllables.len() != split_segments(scheme_segmentation).len() {
        return String::new();
    }
    let mut boundaries = Vec::new();
    let mut end = 0;
    for syllable in &syllables {
        if request.raw_input.get(end..end + syllable.len()) != Some(syllable.as_str()) {
            return String::new();
        }
        end += syllable.len();
        boundaries.push(end);
    }
    if end != request.raw_input.len() {
        return String::new();
    }
    boundaries.pop();
    rebuild_quanpin_display_at_boundaries(cased, &boundaries)
}

/// 合法输入上的纠错领衔时，预编辑按领衔读法分隔，而不是字面切分。
fn build_quanpin_display_from_leading_correction(request: &QueryRequest, head_item: &Item) -> String {
    let cased = if request.raw_input_with_cases.is_empty() { &request.raw_input } else { &request.raw_input_with_cases };
    if head_item.corrected_from.is_empty() || request.raw_input.contains('\'') || cased.len() != request.raw_input.len() {
        return String::new();
    }
    let syllables = split_segments(&head_item.pinyin);
    if syllables.is_empty() {
        return String::new();
    }
    let mut boundaries = Vec::new();
    let mut end = 0;
    let cut = autocorrect::correction_cut_for_reading(&request.raw_input, &syllables, quanpin_autocorrect_types(request));
    match cut {
        Some(cut) if cut.segments.len() == syllables.len() => {
            for segment in &cut.segments {
                end = segment.start + segment.raw.len();
                boundaries.push(end);
            }
        }
        _ => {
            for syllable in &syllables {
                end += syllable.len();
                boundaries.push(end);
            }
        }
    }
    if end != request.raw_input.len() {
        return String::new();
    }
    boundaries.pop();
    rebuild_quanpin_display_at_boundaries(cased, &boundaries)
}

impl<'a> InputSession<'a> {
    pub fn handle_engine_key(&mut self, vk: u32, modifiers_down: u32, wch: u16) {
        self.engine.handle_key(vk, modifiers_down, wch);
        self.online_requests.invalidate();
        self.update_mixed_candidates();
    }

    pub fn recompute_candidates(&mut self) {
        if self.has_pending_pinyin_sequence || self.has_pending_pinyin_sequence_with_cases {
            self.apply_pending_sequence();
            return;
        }
        self.engine.handle_key(0, 0, 0);
        self.update_mixed_candidates();
    }

    pub fn get_pinyin_sequence(&self) -> &str {
        &self.request().raw_input
    }

    pub fn get_pinyin_sequence_with_cases(&self) -> &str {
        // 句中辅助码的反引号段只在原串里，宿主回读的必须是用户敲的原样。
        let request = self.request();
        if !request.raw_input_with_syllable_helpcodes.is_empty() {
            return &request.raw_input_with_syllable_helpcodes;
        }
        if request.raw_input_with_cases.is_empty() {
            &request.raw_input
        } else {
            &request.raw_input_with_cases
        }
    }

    pub fn get_pure_pinyin_sequence(&self) -> &str {
        &self.request().normalized_input
    }

    pub fn get_pinyin_segmentation(&self) -> &str {
        let request = self.request();
        if request.normalized_segmentation.is_empty() {
            &request.segmentation
        } else {
            &request.normalized_segmentation
        }
    }

    pub fn get_pinyin_segmentation_with_cases(&self) -> String {
        self.build_pinyin_segmentation_with_cases(self.shuangpin_preedit_uses_raw)
    }

    pub fn get_shuangpin_preedit_forms(&self) -> ShuangpinPreeditForms {
        if !self.is_shuangpin() {
            return ShuangpinPreeditForms::default();
        }
        ShuangpinPreeditForms { raw: self.build_pinyin_segmentation_with_cases(true), quanpin: self.build_pinyin_segmentation_with_cases(false) }
    }

    pub(super) fn build_pinyin_segmentation_with_cases(&self, shuangpin_raw: bool) -> String {
        let request = self.request();
        if self.is_wubi() {
            return request.raw_input.clone();
        }
        if self.is_japanese() {
            return if request.raw_input_with_cases.is_empty() { request.raw_input.clone() } else { request.raw_input_with_cases.clone() };
        }
        // 句中辅助码的反引号段在切分串里只是一个 '，显示时按原样接回对应音节后面；
        // 末尾补 ' 也要看用户敲的原串，否则 ulpb`x 会显示成 ul'pb`x'。
        let typed = self.get_pinyin_sequence_with_cases();
        let decorate = |segmentation: &str| -> String {
            if request.has_syllable_helpcode_layout {
                sq::decorate_segmentation(segmentation, &request.syllable_helpcode_decorations)
            } else {
                sq::decorate_mid_sentence_segmentation(segmentation, typed, self.profile, false)
            }
        };
        if self.is_shuangpin() && shuangpin_raw {
            let mut preedit = if request.raw_segmentation.is_empty() { request.raw_input.clone() } else { request.raw_segmentation.clone() };
            preedit = decorate(&preedit);
            if typed.ends_with('\'') && !preedit.ends_with('\'') {
                preedit.push('\'');
            }
            return preedit;
        }
        if self.current_scheme_type() == SchemeType::Quanpin {
            // 容错仲裁选中的读音与敲的字母不逐音节相同，下面按首选重建显示的路径会比对失败、退回字面切分；
            // 仲裁已经按原串大小写算好了分段显示，直接用。
            if !self.is_shuangpin() {
                let fault_display = self.engine.fault_segmentation_with_cases();
                if !fault_display.is_empty() {
                    return decorate(fault_display);
                }
            }
            if let Some(head_item) = self.candidates().first() {
                if quanpin_utils::literal_mixed_pinyin_match(&request.raw_input, &head_item.pinyin, &head_item.canonical_pinyin) != 0 {
                    let mut boundaries = Vec::new();
                    let mut consumed = 0;
                    for segment in split_segments(&head_item.pinyin) {
                        consumed += segment.len();
                        boundaries.push(consumed);
                    }
                    if consumed == request.raw_input.len() {
                        boundaries.pop();
                    }
                    return rebuild_quanpin_display_at_boundaries(typed, &boundaries);
                }
            }
            if let Some(head_item) = self.candidates().first() {
                let mut display = build_quanpin_display_from_literal_head(request, head_item);
                if display.is_empty() {
                    display = build_quanpin_display_from_leading_correction(request, head_item);
                }
                if !display.is_empty() {
                    return display;
                }
            }
            return build_quanpin_autocorrect_display(request);
        }
        let mut preedit = if request.normalized_segmentation.is_empty() { request.segmentation.clone() } else { request.normalized_segmentation.clone() };
        if self.is_shuangpin() {
            preedit = decorate(&preedit);
        }
        if typed.ends_with('\'') && !preedit.ends_with('\'') {
            preedit.push('\'');
        }
        preedit
    }

    pub fn get_quanpin(&self) -> &str {
        &self.request().normalized_input
    }

    pub fn is_all_complete_pure_pinyin(&self) -> bool {
        let request = self.request();
        if self.is_wubi() {
            return request.valid;
        }
        if self.is_japanese() {
            return romaji::convert(&request.raw_input).complete;
        }
        if self.is_shuangpin() {
            let base = resolve_shuangpin_composition_base(request, self.profile);
            if base.helpcode_length > 0 && base.effective_raw_input.len() >= base.helpcode_length {
                let base_length = base.effective_raw_input.len() - base.helpcode_length;
                return sq::is_complete_input(head(&base.raw_input, sq::raw_length_for_effective_prefix(&base.raw_input, base_length)), self.profile);
            }
            return sq::is_complete_input(&base.raw_input, self.profile);
        }
        let segmentation = self.get_pinyin_segmentation();
        !segmentation.is_empty() && quanpin_query::is_complete_pinyin_input(segmentation)
    }

    /// 整串输入能不能读成拼音：全拼按「完整音节或音节前缀」切完整串，双拼按完整音节码、末尾允许单独一个声母键。
    /// 不要求音节完整，也不看词库有没有词。
    pub fn reads_as_pinyin(&self) -> bool {
        if self.has_active_helpcode() {
            return true;
        }
        let raw = remove_delimiters(&self.request().raw_input).to_ascii_lowercase();
        if raw.is_empty() {
            return false;
        }
        if self.is_shuangpin() {
            // 双拼两键一个音节，打到一半时末尾会剩一个声母键。
            return raw.len() == 1 || sq::is_complete_input(&raw, self.profile) || sq::is_complete_input(head(&raw, raw.len() - 1), self.profile);
        }
        if self.current_scheme_type() == SchemeType::Quanpin {
            return segment::cut_min_segments_prefix(&raw).is_some();
        }
        false
    }

    /// 当前组合是五笔表恰好答了一行的完整四码。宿主据此决定要不要自动上屏；混输追加的拼音候选不算。
    pub fn wubi_unique_four_code(&self) -> bool {
        if self.dedicated_english_mode || self.local_input_mode != LocalInputMode::None {
            return false;
        }
        // 带 z 的码是通配猜测，不是用户认定的码：它背后「唯一的候选」离别的候选只差一个字母。
        if self.engine.request().raw_input.contains('z') {
            return false;
        }
        if !self.is_wubi() || !self.engine.wubi_code_is_complete() {
            return false;
        }
        self.wubi_native_candidate_count() == 1
    }

    /// 当前组合是五笔表答了的完整四码，不管有几个候选。宿主用它在用户敲过第四码继续敲时顶字上屏。
    pub fn wubi_four_code_is_complete(&self) -> bool {
        if self.dedicated_english_mode || self.local_input_mode != LocalInputMode::None {
            return false;
        }
        if self.engine.request().raw_input.contains('z') {
            return false;
        }
        self.is_wubi() && self.engine.wubi_code_is_complete() && self.wubi_native_candidate_count() > 0
    }

    pub fn has_active_helpcode(&self) -> bool {
        if self.is_wubi() || self.is_japanese() {
            return false;
        }
        if self.is_shuangpin() {
            return resolve_shuangpin_composition_base(self.request(), self.profile).helpcode_length > 0;
        }
        has_active_quanpin_helpcode(self.request())
    }

    pub fn set_pinyin_sequence(&mut self, pinyin_sequence: &str) {
        self.pending_pinyin_sequence = pinyin_sequence.to_string();
        self.has_pending_pinyin_sequence = true;
    }

    pub fn set_pinyin_sequence_with_cases(&mut self, pinyin_sequence: &str) {
        self.pending_pinyin_sequence_with_cases = pinyin_sequence.to_string();
        self.has_pending_pinyin_sequence_with_cases = true;
    }

    pub fn store_user_phrase(&mut self, pinyin: &str, word: &str) -> i32 {
        self.engine.create_word(pinyin, word)
    }

    pub fn store_user_phrase_from_canonical_pinyin(&mut self, pinyin: &str, word: &str) -> i32 {
        // 全拼和双拼最终共用同一份全拼词库：完整的全拼键不能再过一遍当前的双拼方案。
        if self.canonical_phrase_engine.is_none() {
            let engine = QuanpinDictionary::open(std::path::Path::new(&self.main_dictionary_path()), &self.journal_path(), None);
            self.canonical_phrase_engine = Some(engine);
        }
        self.canonical_phrase_engine.as_mut().map_or(ERROR_CODE, |engine| engine.create_word_from_canonical_pinyin(pinyin, word))
    }

    /// 混输组合里五笔与拼音候选共存，删除必须由调用方指明候选自己的方案，不能按会话方案。
    pub fn remove_candidate_entry(&mut self, pinyin: &str, word: &str, scheme: SchemeType) -> i32 {
        if !self.is_wubi() && remove_delimiters(&self.request().raw_input).len() == 1 {
            return -1;
        }
        self.engine.delete_by_pinyin_and_word(scheme, pinyin, word)
    }

    pub fn advance_composition_after_selection(&mut self, selected_pinyin: &str, selected_word: &str, selected_canonical_pinyin: &str, selected_scheme: SchemeType) -> SelectionTransition {
        let mut transition = SelectionTransition { selected_canonical_pinyin: selected_canonical_pinyin.to_string(), wubi_native: selected_scheme == SchemeType::Wubi, ..Default::default() };
        if self.is_japanese() {
            let request = self.request();
            transition.full_pure_pinyin = request.raw_input.clone();
            transition.current_segmentation = request.segmentation.clone();
            transition.current_segmentation_with_cases = request.raw_input_with_cases.clone();
            return transition;
        }
        if transition.wubi_native {
            let request = self.request();
            transition.full_pure_pinyin = request.normalized_input.clone();
            transition.current_segmentation = request.normalized_input.clone();
            transition.current_segmentation_with_cases = request.raw_input.clone();
            return transition;
        }
        if self.is_shuangpin() {
            let base = resolve_shuangpin_composition_base(self.request(), self.profile);
            let word_pinyin_length = helpcode::count_han_chars(selected_word) * 2;
            let total_input_length = base.effective_raw_input.len();

            transition.full_pure_pinyin = if base.helpcode_length > 0 && total_input_length >= base.helpcode_length {
                head(&base.effective_raw_input, total_input_length - base.helpcode_length).to_string()
            } else {
                base.effective_raw_input.clone()
            };

            let mut consumed_length = remove_delimiters(selected_pinyin).len();
            // 带句中辅助码时 base 是去掉反引号段的串，推进结果要映射回原串，见 remap_mid_sentence_rest。
            let typed = self.request().raw_input_with_syllable_helpcodes.clone();
            if base.helpcode_length > 0 {
                let rest_start = sq::raw_length_for_effective_prefix(&base.raw_input_with_cases, word_pinyin_length);
                transition.consumed_raw_input_with_cases = head(&base.raw_input_with_cases, rest_start).to_string();
                let required_length = word_pinyin_length + base.helpcode_length;
                transition.continues_composition = required_length < total_input_length && word_pinyin_length < total_input_length;

                if transition.continues_composition {
                    let rest_end = sq::raw_length_for_effective_prefix(&base.raw_input_with_cases, total_input_length - base.helpcode_length);
                    let rest_pinyin_sequence = slice(&base.raw_input, rest_start, rest_end.saturating_sub(rest_start));
                    let mut normalized_rest = rest_pinyin_sequence.to_string();
                    let mut cased_rest = slice(&base.raw_input_with_cases, rest_start, rest_end.saturating_sub(rest_start)).to_string();
                    remove_consumed_leading_separators(&mut normalized_rest, &mut cased_rest);
                    if !typed.is_empty() {
                        let remapped = remap_mid_sentence_rest(self.request(), &typed, rest_start, rest_end, self.profile);
                        transition.consumed_raw_input_with_cases = remapped.consumed;
                        cased_rest = remapped.rest;
                        normalized_rest = lowercase_letters(&cased_rest);
                    }
                    self.engine.replace_active_raw_input(&normalized_rest, &cased_rest);
                    self.online_requests.invalidate();
                    self.update_mixed_candidates();
                }
            } else {
                if consumed_length == 0 || consumed_length > base.effective_raw_input.len() {
                    consumed_length = word_pinyin_length.min(base.effective_raw_input.len());
                }

                let consumed_raw_length = sq::raw_length_for_effective_prefix(&base.raw_input_with_cases, consumed_length);
                transition.consumed_raw_input_with_cases = head(&base.raw_input_with_cases, consumed_raw_length).to_string();
                transition.continues_composition = consumed_length < transition.full_pure_pinyin.len();

                if transition.continues_composition {
                    let rest_pinyin_sequence = tail(&base.raw_input, consumed_raw_length);
                    let rest_pinyin_sequence_with_cases = tail(&base.raw_input_with_cases, consumed_raw_length);
                    let mut normalized_rest = rest_pinyin_sequence.to_string();
                    let mut cased_rest = rest_pinyin_sequence_with_cases.to_string();
                    remove_consumed_leading_separators(&mut normalized_rest, &mut cased_rest);
                    if !typed.is_empty() {
                        let remapped = remap_mid_sentence_rest(self.request(), &typed, consumed_raw_length, base.raw_input_with_cases.len(), self.profile);
                        transition.consumed_raw_input_with_cases = remapped.consumed;
                        cased_rest = remapped.rest;
                        normalized_rest = lowercase_letters(&cased_rest);
                    }
                    self.engine.replace_active_raw_input(&normalized_rest, &cased_rest);
                    self.online_requests.invalidate();
                    self.update_mixed_candidates();
                }
            }

            if !typed.is_empty() && !transition.continues_composition {
                // 整串上屏：撤销时要还原的是用户敲的原串，连同反引号段。
                transition.consumed_raw_input_with_cases = typed;
            }

            transition.current_segmentation = self.get_pinyin_segmentation().to_string();
            transition.current_segmentation_with_cases = self.get_pinyin_segmentation_with_cases();
            return transition;
        }

        let request = self.request();
        transition.full_pure_pinyin = request.normalized_input.clone();
        let current_segmentation = self.get_pinyin_segmentation().to_string();
        let current_segmentation_with_cases = self.get_pinyin_segmentation_with_cases();
        let selected_pure_pinyin = remove_delimiters(selected_pinyin);
        let raw_input_without_helpcodes = quanpin_utils::strip_active_helpcodes(&request.raw_input, &request.raw_input_with_cases);
        let raw_input_with_cases_without_helpcodes = quanpin_utils::strip_active_helpcodes_with_cases(&request.raw_input, &request.raw_input_with_cases);

        // 纠错读音按它在原始输入里盖住的字母消耗：多字、漏字纠错和原始字母不等长，按读音长度消耗会剩下或多吞字母。
        let mut consumed_letters = selected_pure_pinyin.len();
        let autocorrect_types = quanpin_autocorrect_types(request);
        let literal_mixed_prefix = self.current_scheme_type() == SchemeType::Quanpin
            && self
                .candidates()
                .first()
                .is_some_and(|front| quanpin_utils::literal_mixed_pinyin_match(&raw_input_without_helpcodes, &front.pinyin, &front.canonical_pinyin) != 0)
            && raw_input_without_helpcodes.get(..consumed_letters) == Some(selected_pure_pinyin.as_str());
        if autocorrect_types != 0 && !literal_mixed_prefix && !raw_input_without_helpcodes.contains('\'') {
            if let Some(aligned) = autocorrect::corrected_reading_raw_length(&raw_input_without_helpcodes, &split_segments(selected_pinyin), autocorrect_types) {
                consumed_letters = aligned;
            } else if let Some(fault_aligned) = autocorrect::fault_reading_raw_length(&raw_input_without_helpcodes, &split_segments(selected_pinyin)) {
                // 长句容错仲裁的读音（句中简拼展开、槽内漏字变体）不在 k-best 切分里，按槽位模型对回原始字母。
                consumed_letters = fault_aligned;
            }
        }

        let consumed_raw_length = sq::raw_length_for_effective_prefix(&raw_input_with_cases_without_helpcodes, consumed_letters);
        transition.consumed_raw_input_with_cases = head(&raw_input_with_cases_without_helpcodes, consumed_raw_length).to_string();

        transition.continues_composition =
            !selected_pure_pinyin.is_empty() && consumed_letters < transition.full_pure_pinyin.len() && consumed_raw_length < raw_input_without_helpcodes.len();

        if transition.continues_composition {
            let mut rest_raw_input = tail(&raw_input_without_helpcodes, consumed_raw_length).to_string();
            let mut rest_raw_input_with_cases = tail(&raw_input_with_cases_without_helpcodes, consumed_raw_length).to_string();
            remove_consumed_leading_separators(&mut rest_raw_input, &mut rest_raw_input_with_cases);
            self.engine.replace_active_raw_input(&rest_raw_input, &rest_raw_input_with_cases);
            self.online_requests.invalidate();
            self.update_mixed_candidates();
            transition.current_segmentation = self.get_pinyin_segmentation().to_string();
            transition.current_segmentation_with_cases = self.get_pinyin_segmentation_with_cases();
            return transition;
        }

        transition.current_segmentation = current_segmentation;
        transition.current_segmentation_with_cases = current_segmentation_with_cases;
        transition
    }

    pub fn get_cloud_query_state(&self) -> CloudQueryState {
        let request = self.request();
        let mut state = CloudQueryState::default();

        if self.is_japanese() {
            state.cache_key = request.raw_input.clone();
            state.committed_pinyin = request.raw_input.clone();
            state.should_query = !request.raw_input.is_empty();
            state.query_text = if state.should_query { request.raw_input.clone() } else { String::new() };
            return state;
        }

        if self.is_wubi() {
            state.cache_key = request.normalized_input.clone();
            state.committed_pinyin = request.normalized_input.clone();
            return state;
        }

        if self.is_shuangpin() {
            let base = resolve_shuangpin_composition_base(request, self.profile);
            state.cache_key = resolve_shuangpin_cloud_cache_key(request, self.profile);
            state.committed_pinyin = sq::remove_manual_delimiters(&state.cache_key);

            if self.has_active_helpcode() {
                return state;
            }

            let last = base.effective_raw_input_with_cases.bytes().last().unwrap_or(0);
            let ends_with_input_key = last.is_ascii_lowercase() || last == b';';
            state.should_query = ends_with_input_key && sq::is_complete_input(&base.effective_raw_input, self.profile);

            if state.should_query {
                // 云输入拿的是带撇号的全拼，但 ü 要换成 inputtools 认的写法（nue/lue），否则 nve'dai'dong'wu 会被拆成 nv + e。
                let quanpin_segmentation = sq::normalize_input_with_delimiters(&state.cache_key, self.profile);
                state.query_text = quanpin_utils::to_google_spelling(&quanpin_segmentation);
            }
            return state;
        }

        state.committed_pinyin = request.normalized_input.clone();
        state.cache_key = resolve_quanpin_cloud_cache_key(request);

        if self.has_active_helpcode() {
            return state;
        }

        state.should_query = !request.normalized_input.is_empty();
        // 云端要按用户看到的分词来查：手动把 qi'e'huan 分成三段，就该带着音节边界发给云接口。
        let segmentation = if request.normalized_segmentation.is_empty() { &request.normalized_input } else { &request.normalized_segmentation };
        state.query_text = quanpin_utils::to_google_spelling(segmentation);
        state
    }

    pub fn update_creating_word_progress(
        &self,
        current_pinyin: &str,
        current_word: &str,
        selected_word: &str,
        selection_transition: &SelectionTransition,
    ) -> CreatingWordProgress {
        let mut progress = CreatingWordProgress::default();
        if selection_transition.wubi_native {
            progress.pinyin = if current_pinyin.is_empty() { selection_transition.full_pure_pinyin.clone() } else { current_pinyin.to_string() };
            progress.word = format!("{current_word}{selected_word}");
            progress.preedit = progress.word.clone();
            progress.completed = true;
            progress.can_store = false;
            return progress;
        }

        let selected_canonical = normalize_canonical_pinyin_for_word(&selection_transition.selected_canonical_pinyin, selected_word);
        let prior_parts_are_storeable = current_word.is_empty() || !current_pinyin.is_empty();
        if prior_parts_are_storeable && !selected_canonical.is_empty() {
            progress.pinyin = append_canonical_pinyin(current_pinyin, &selected_canonical);
        }
        progress.word = format!("{current_word}{selected_word}");
        progress.preedit = format!("{}{}", progress.word, selection_transition.current_segmentation_with_cases);
        progress.completed = !selection_transition.continues_composition;
        progress.can_store = progress.completed && !normalize_canonical_pinyin_for_word(&progress.pinyin, &progress.word).is_empty();
        progress
    }

    pub(super) fn clear_pending_sequence(&mut self) {
        self.pending_pinyin_sequence.clear();
        self.pending_pinyin_sequence_with_cases.clear();
        self.has_pending_pinyin_sequence = false;
        self.has_pending_pinyin_sequence_with_cases = false;
    }

    pub(super) fn apply_pending_sequence(&mut self) {
        self.caret = None;
        let raw_input = if self.has_pending_pinyin_sequence { self.pending_pinyin_sequence.clone() } else { self.request().raw_input.clone() };
        let raw_input_with_cases = if self.has_pending_pinyin_sequence_with_cases { self.pending_pinyin_sequence_with_cases.clone() } else { raw_input.clone() };
        // 各方案的整串替换在 C++ 里分四个函数，只对匹配的方案生效；活动方案就是匹配的那个。
        self.engine.replace_active_raw_input(&raw_input, &raw_input_with_cases);
        self.clear_pending_sequence();
        self.online_requests.invalidate();
        self.update_mixed_candidates();
    }
}

