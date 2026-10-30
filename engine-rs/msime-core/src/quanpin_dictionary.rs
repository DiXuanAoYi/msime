//! 全拼词库：把全拼输入串变成候选列表，移植自 `engine/quanpin/quanpin_dictionary.cpp`。
//!
//! 搬过来的是不带纠错的那一半：按切分查词库（整串、前缀子串、稀疏拼音回退）、等音节数切分的备选并入、
//! 词格整句并入、字面混合简拼/整词领衔、合法输入上等音节数切分的整句仲裁、模糊音并入、别名改写的纠错标记、
//! 单字母候选扩到全部，以及造词/调频/删词。
//!
//! 整句来源：词格、Google 解码器（兜底整句与独立整句行）、两个神经模型的重排（后台线程，见 `sentence_sources`）、
//! 万象语法搭配，由 `SentenceAssociation` 开关、`with_sentence_sources` 提供模型和解码器。
//!
//! 没搬的：
//! - 按键状态机 `handleVkCode`、`insert_word_to_series_cache`；
//! - 逐前缀的词库行缓存、切分缓存、纠错裁决缓存、词格路径与跨度缓存（只影响速度）。整串的结果缓存（`series_cache`）
//!   是语义的一部分：云/AI 候选经 `insert_word_to_series_cache` 插进缓存里的那份列表，下一次同键查询原样取出，
//!   所以这一层照 C++ 搬了，清缓存的时机也一样（写库、换开关、换前文、词库被别的连接改过）。

use crate::dict::{Dictionary, KeyedEntry, QuerySource};
use crate::domain_dictionary::{self, DomainDictionary};
use crate::autocorrect::{self, correction_aliases_enabled, looks_like_jianpin_spelling, Cut, FaultSlotKind};
use crate::cache::Cache;
use crate::fuzzy;
use crate::helpcode;
use crate::lattice::{self, FaultEdge, Item, LanguageModel, Lexeme, Path as LatticePath, Source};
use crate::quanpin_query::{
    cut_pinyin_by_mode, has_only_complete_pinyin_segments, is_complete_pinyin_input, join_segments, segments_to_jianpin, split_segments, CutMode, Segments,
};
use crate::quanpin_utils::{
    build_syllable_graph, enumerate_complete_segmentations, enumerate_min_segmentations, fold_letters, literal_jianpin_pinyin_match,
    literal_mixed_pinyin_match, canonical_umlaut_spelling, normalize_umlaut_aliases, sparse_pinyin_fallback_segments, to_google_spelling,
};
use crate::sentence_sources::{neural_reranker, SentenceSources};
pub use crate::shuangpin_dictionary::{SentenceAssociation, ERROR_CODE, OK};
use crate::syllables::{is_intact, is_prefix};
use crate::table::table_name;
use crate::user_dictionary::{self, DictionaryKind};
use rusqlite::{Connection, OpenFlags};
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

/// 万象语法的 `query()` 是自然对数域，乘它换到词格的 log10 域。
pub(crate) const LN_TO_LOG10: f64 = 0.4342944819032518;
/// 神经重排最多重打分的路径数，也是开了重排时词格要解出的 n-best 条数（`RerankOptions::default().max_paths`）。
const NEURAL_NBEST: i32 = 12;

const SPARSE_PINYIN_FALLBACK_THRESHOLD: usize = 8;
const SYLLABLE_GRAPH_PATH_LIMIT: usize = 32;
const MAX_SYLLABLES_FOR_MULTIPLE_SEGMENTATIONS: usize = 4;
const ALTERNATIVE_SEGMENTATION_CANDIDATE_LIMIT: usize = 128;
const BEST_ALTERNATIVE_SEGMENTATION_MAX_INDEX: usize = 1;
/// 保护位只对“排在首页之外”的备选读音生效；默认页长 6。
const ALTERNATIVE_SEGMENTATION_FIRST_PAGE_SIZE: usize = 6;
/// 备选读音的最佳词权重达到主切分首位的 1/RATIO 以上才配保护位。
const ALTERNATIVE_SEGMENTATION_PROMOTION_RATIO: i64 = 100;
const INITIAL_CANDIDATE_LIMIT: usize = 24;
/// 领域词库 `query` / `exact` 的默认条数上限。
const DOMAIN_QUERY_LIMIT: usize = 128;
/// 词库读连接等一次提交的时间，对应 C++ 的 `kDictionaryBusyTimeoutMs`。
const DICTIONARY_BUSY_TIMEOUT: Duration = Duration::from_millis(250);
/// 合法输入的等音节数字面切分：最多枚举几条、最多多解几条词格。
const LITERAL_SEGMENTATION_ENUMERATE_LIMIT: usize = 8;
const LITERAL_SEGMENTATION_DECODE_LIMIT: usize = 3;
/// 原读法领衔时，另一种读法离它在这个分差以内才出现在第 2 位。
const LEGAL_INPUT_RUNNER_UP_GAP_LOG: f64 = 3.0;
/// 合法输入上每处纠正的手误代价，与词格路径分同单位（加载 sc.lm 时是 log10：2.0 = 纠错读法的句概率要高出 100 倍
/// 才抵得过一处纠正）。改变字母数的纠正（多字、漏字）贵一档：合法输入里第二个音节是 e/a/er/an 的真词很多，
/// 按多一个字母读都能拼出另一个读音。
const LEGAL_INPUT_TRANSPOSITION_PENALTY_LOG: f64 = 2.0;
const LEGAL_INPUT_LENGTH_CHANGE_PENALTY_LOG: f64 = 3.0;
/// 纠错读法进查询管线的条数：主切成为查询键，其余作为备选切分参与查询时的歧义消解。
const AUTOCORRECT_CUT_KBEST: usize = 9;
/// 合法输入上的纠错读法个数上限：每条都要在按键路径上多解一次词格。
const LEGAL_CORRECTION_CUT_LIMIT: usize = 3;
/// 纠错的上下文消解：解码预算（通过静态门槛且词库有行的同档备选最多 4 个；贵档读法另给 2 个）。
const AUTOCORRECT_CONTEXT_DECODE_LIMIT: usize = 4;
const COSTLIER_CONTEXT_DECODE_LIMIT: usize = 2;
/// 纠错表权重换算成手误代价（路径分单位）：换位 10 -> 2.0，漏字 11 -> 2.2，多字 12 -> 2.4，邻键 13 -> 2.6，生成式 15 -> 3.0。
const AUTOCORRECT_WEIGHT_TO_LOG: f64 = 0.2;
/// 接管边际，以打分器自身的对数单位计（词格 LM 打分是 log10，1.0 ≈ 10 倍句概率）。
const AUTOCORRECT_CONTEXT_MARGIN_LOG: f64 = 1.0;
/// 简拼展开边的惩罚折算（容错仲裁）。展开是用户意图（r 就是想打某个 r 开头的全拼）不是手误：系数刻意远小于
/// `AUTOCORRECT_WEIGHT_TO_LOG`，让语言模型在展开候选之间做全局裁决，只留一个平局序数。
const JIANPIN_EXPAND_PENALTY_LOG: f64 = 0.1;

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

/// 云/AI 候选：权重 1，没有规范读音。
pub(crate) fn dynamic_item(pinyin: &str, word: &str, source: Source) -> Item {
    item(pinyin, word, 1, source, "")
}

fn keyed_item(code: &str, row: &KeyedEntry) -> Item {
    item(code, &row.text, row.weight, Source::Database, &row.key)
}

fn is_dictionary_row(item: &Item) -> bool {
    matches!(item.source, Source::Database | Source::UserDatabase)
}

fn candidate_letters(item: &Item) -> String {
    fold_letters(if item.canonical_pinyin.is_empty() { &item.pinyin } else { &item.canonical_pinyin })
}

/// 字面整词：词库行、读音字母与敲的字母逐个相同，且匹配码覆盖完整输入（任一种切分都算）。
fn find_literal_whole_word(candidates: &[Item], raw_input: &str) -> Option<usize> {
    let typed = fold_letters(raw_input);
    candidates.iter().position(|item| is_dictionary_row(item) && candidate_letters(item) == typed && fold_letters(&item.pinyin) == typed)
}

/// 输入能切成词库里的真词时，首位留给这个字面整词。
fn keep_literal_whole_word_first(candidates: &mut [Item], raw_input: &str) {
    if candidates.is_empty() || candidate_letters(&candidates[0]) == fold_letters(raw_input) {
        return;
    }
    if let Some(literal) = find_literal_whole_word(candidates, raw_input) {
        candidates[..=literal].rotate_right(1);
    }
}

fn literal_mixed_match(item: &Item, raw_input: &str) -> u32 {
    if is_dictionary_row(item) {
        literal_mixed_pinyin_match(raw_input, &item.pinyin, &item.canonical_pinyin)
    } else {
        0
    }
}

fn literal_jianpin_match(item: &Item, raw_input: &str) -> bool {
    is_dictionary_row(item) && literal_jianpin_pinyin_match(raw_input, &item.pinyin, &item.canonical_pinyin)
}

/// 读音就是 key 这条切分的行（整键块）。整句行按敲的读音（`pinyin`）认。
fn reads_full_key(item: &Item, key: &str) -> bool {
    item.canonical_pinyin == key || (item.sentence_association && item.pinyin == key)
}

/// 整词（1）在前，带没打完尾巴的（2）在后，其余原样跟着；各组内部保持原先的次序。
fn keep_literal_mixed_words_first(candidates: &mut Vec<Item>, raw_input: &str) {
    let (mut whole, mut tailed, mut rest) = (Vec::new(), Vec::new(), Vec::new());
    for candidate in candidates.drain(..) {
        match literal_mixed_match(&candidate, raw_input) {
            1 => whole.push(candidate),
            2 => tailed.push(candidate),
            _ => rest.push(candidate),
        }
    }
    whole.extend(tailed);
    whole.extend(rest);
    *candidates = whole;
}

fn append_unique_words(result: &mut Vec<Item>, extra: &[Item]) {
    for candidate in extra {
        if !result.iter().any(|existing| existing.word == candidate.word) {
            result.push(candidate.clone());
        }
    }
}

/// 候选覆盖了多少个敲出的字母，单引号不算。
fn matched_letters(item: &Item) -> usize {
    item.pinyin.chars().filter(|c| *c != '\'').count()
}

/// 把模糊候选归并进精确候选，对应 C++ `quanpin::merge_fuzzy_candidates`（与 `fuzzy::merge_fuzzy_candidates` 同一规则，
/// 只是操作整条候选）。
fn merge_fuzzy_items(exact: Vec<Item>, fuzzy_items: Vec<Item>) -> Vec<Item> {
    merge_fuzzy_items_by(exact, fuzzy_items, matched_letters)
}

/// 同上，`group` 是候选覆盖的输入长度（大的在前）；双拼引擎按读音串的字节数分组。
pub(crate) fn merge_fuzzy_items_by(exact: Vec<Item>, mut fuzzy_items: Vec<Item>, group: impl Fn(&Item) -> usize) -> Vec<Item> {
    let mut seen: HashSet<String> = exact.iter().map(|c| c.word.clone()).collect();
    fuzzy_items.retain(|c| seen.insert(c.word.clone()));
    fuzzy_items.sort_by(|a, b| group(b).cmp(&group(a)).then(b.weight.cmp(&a.weight)));
    let ranked_by_weight = |c: &Item| !c.fuzzy && is_dictionary_row(c);
    let mut merged = Vec::with_capacity(exact.len() + fuzzy_items.len());
    let (mut exact, mut fuzzy_items) = (exact.into_iter().peekable(), fuzzy_items.into_iter().peekable());
    loop {
        let take_fuzzy = match (exact.peek(), fuzzy_items.peek()) {
            (None, None) => break,
            (None, Some(_)) => true,
            (Some(_), None) => false,
            (Some(e), Some(f)) if ranked_by_weight(e) => {
                let (ge, gf) = (group(e), group(f));
                if ge != gf {
                    gf > ge
                } else {
                    (e.weight as f64) < (f.weight as f64) * fuzzy::WEIGHT_RATIO
                }
            }
            (Some(_), Some(_)) => false,
        };
        merged.push(if take_fuzzy { fuzzy_items.next().unwrap() } else { exact.next().unwrap() });
    }
    merged
}

/// 一次查询的纠错裁决：主切的纠正切分，以及两档备选读法。
#[derive(Default)]
struct SeriesQueryResolution {
    segmentation: String,
    /// 整串结果缓存的键：纠错过的、合法输入上带纠错读法的各占一个槽，关掉纠错后不会读到它们。
    cache_key: String,
    corrected_segments: Segments,
    /// 与主切同一档手误代价（纠正边数和权重和都相等）的备选读法，和主切按词频竞争。
    alternative_corrected_cuts: Vec<Segments>,
    /// 严格更贵的读法：留在主切档之后，不让词频把更贵的纠正抬到便宜的上面。
    costlier_corrected_cuts: Vec<Segments>,
    /// 纠错表权重之和：主切所在档一个，贵档逐条一个（与 `costlier_corrected_cuts` 平行）。
    corrected_weight: i32,
    costlier_weights: Vec<i32>,
    /// 与 `costlier_corrected_cuts` 平行：这条贵档读法能否被上下文提到首位。含邻键替换或生成式纠正的读法不能。
    costlier_promotable: Vec<bool>,
    /// 合法输入上的手误读法，主切不动，与它按整句打分争领衔。
    legal_corrected_cuts: Vec<Cut>,
    corrected_input: bool,
}

fn cut_syllables_with_tail(cut: &Cut, jianpin_tail: &str) -> Segments {
    let mut segments = cut.syllables();
    if !jianpin_tail.is_empty() {
        segments.push(jianpin_tail.to_string());
    }
    segments
}

/// 把按代价从低到高排好的读法分进三处：主切、同档备选、贵档备选。
fn populate_from_cuts(result: &mut SeriesQueryResolution, cuts: &[Cut], jianpin_tail: &str) {
    let primary_cut = &cuts[0];
    result.corrected_segments = cut_syllables_with_tail(primary_cut, jianpin_tail);
    result.corrected_weight = primary_cut.weight;
    for cut in &cuts[1..] {
        if cut.same_cost_as(primary_cut) {
            result.alternative_corrected_cuts.push(cut_syllables_with_tail(cut, jianpin_tail));
        } else {
            result.costlier_corrected_cuts.push(cut_syllables_with_tail(cut, jianpin_tail));
            result.costlier_weights.push(cut.weight);
            result.costlier_promotable.push(cut.segments.iter().all(|segment| segment.weight <= autocorrect::WEIGHT_INSERTION));
        }
    }
}

fn resolve_series_query(raw_input: &str, segments: &[String], autocorrect_types: u32) -> SeriesQueryResolution {
    let mut result = SeriesQueryResolution::default();
    // 闸的次序：简拼形状的判断先于 k-best 搜索，被挡住的输入不付搜索的钱。两道闸都表达“用户没有敲错”。
    // 句首声母可能是不小心连敲了两次，所以句首单声母时先算纠正，再由 query_exact 里字面的混合简拼整词决定让不让。
    let leading_initial = segments.first().is_some_and(|first| first.len() == 1 && !is_intact(first));
    let eligible = autocorrect_types != 0
        && !raw_input.contains('\'')
        && !has_only_complete_pinyin_segments(segments)
        && (!looks_like_jianpin_spelling(raw_input) || leading_initial);
    if eligible {
        let cuts = autocorrect::autocorrect_cut_kbest(raw_input, autocorrect_types, AUTOCORRECT_CUT_KBEST);
        if !cuts.is_empty() {
            result.corrected_input = true;
            populate_from_cuts(&mut result, &cuts, "");
        } else {
            // 纠正加简拼尾：k-best 只在每一段都是完整音节时才走到末尾，hauzh（hau 手误 + zh 简拼）会整体失败。
            // 去掉末尾不完整音节再试一遍，最短的尾巴先试。head 用完整掩码（静态邻键进来：shabgz -> shang'z）。
            // 邻键位同时带着生成式远键，这些读法不组合：head 最优切分带 has_generated 时拒（k-best 静态优先，等价于
            // 没有静态读法能解释 head，挡 huanzg -> huang'g）。
            // 自洽明文读法优先：只要有一条「合法简拼尾 + 形如简拼的 head」的切分（tail_len 1..2），输入本身就有一个不靠
            // 猜测的明文读法，整个组合纠错让位（xish -> xia'h、xibd -> xin'd、huanzh -> huang'h 都因此不改写）。
            let mut composable_tails: Vec<usize> = Vec::new();
            for tail_len in 1..=2 {
                if tail_len >= raw_input.len() {
                    break;
                }
                // 简拼尾是不完整音节：合法的拼音前缀、自己又不是完整音节（zh、z、h）。
                let tail = &raw_input[raw_input.len() - tail_len..];
                if !is_prefix(tail) || is_intact(tail) {
                    continue;
                }
                if looks_like_jianpin_spelling(&raw_input[..raw_input.len() - tail_len]) {
                    composable_tails.clear();
                    break;
                }
                composable_tails.push(tail_len);
            }
            for tail_len in composable_tails {
                let tail = &raw_input[raw_input.len() - tail_len..];
                let head = &raw_input[..raw_input.len() - tail_len];
                let head_cuts = autocorrect::autocorrect_cut_kbest(head, autocorrect_types, AUTOCORRECT_CUT_KBEST);
                if head_cuts.is_empty() || head_cuts[0].has_generated {
                    continue;
                }
                result.corrected_input = true;
                populate_from_cuts(&mut result, &head_cuts, tail);
                break;
            }
        }
    } else if autocorrect_types != 0 {
        // 合法输入：上面的闸把它整个挡在纠错外。这里只列出纠错读法，主切不动，取舍在 arbitrate_legal_corrections。
        result.legal_corrected_cuts = autocorrect::legal_input_correction_cuts(raw_input, segments, autocorrect_types, LEGAL_CORRECTION_CUT_LIMIT);
    }
    result.segmentation = if result.corrected_input {
        join_segments(&result.corrected_segments)
    } else if segments.is_empty() {
        raw_input.to_string()
    } else {
        join_segments(segments)
    };
    // 合法输入带纠错读法时候选列表也变了，同样要另占缓存槽。
    let cache_prefix = if result.corrected_input {
        "C:"
    } else if result.legal_corrected_cuts.is_empty() {
        ""
    } else {
        "L:"
    };
    result.cache_key = format!("{cache_prefix}{}", series_cache_key(raw_input, &result.segmentation));
    // 字面兜底取决于敲的字母，哪怕两种手误共用一个纠错键。
    if result.corrected_input {
        result.cache_key.push('\x1f');
        result.cache_key.push_str(raw_input);
    }
    result
}

/// 整串结果缓存的键：手动分隔符的输入与没有的分开放。
fn series_cache_key(raw_input: &str, segmentation: &str) -> String {
    let prefix = if raw_input.contains('\'') { "M:" } else { "A:" };
    format!("{prefix}{}", if segmentation.is_empty() { raw_input } else { segmentation })
}

/// 容错读音的分段预编辑：读音与原始字母按槽对齐（与 `fault_reading_raw_length` 同一套槽模型），逐槽把读音音节拼回原字母，
/// 只在槽界插入分隔符。只有逐字母等长的槽才做对应，否则返回空串（调用方保持原切分）。单字母槽展开后会吃掉多个读音音节
/// （c -> ce、l -> liu），所以按槽跨度消费读音，而不是逐下标一一对应。
fn display_at_winning_slots(raw_input: &str, reading: &[String]) -> String {
    let slots = autocorrect::build_fault_slots(raw_input);
    if slots.is_empty() || reading.is_empty() {
        return String::new();
    }
    let cased: String = raw_input.chars().filter(|&c| c != '\'').collect();
    let mut display = String::with_capacity(cased.len() + slots.len());
    let mut consumed = 0;
    let mut read_index = 0;
    for (i, slot) in slots.iter().enumerate() {
        let letters = slot.raw_text.len();
        if consumed + letters > cased.len() || read_index >= reading.len() {
            return String::new();
        }
        match slot.kind {
            FaultSlotKind::Determining => {
                let syllable = &reading[read_index];
                // 字面读法直接过；漏字变体（loose scan 允许 yang 读 yan）按同一条形态约束放行。
                let literal = *syllable == slot.base_syllable || (syllable.len() == letters && canonical_umlaut_spelling(syllable) == canonical_umlaut_spelling(&slot.raw_text));
                if !literal && !autocorrect::is_deletion_variant(&slot.raw_text, syllable) {
                    return String::new();
                }
                read_index += 1;
            }
            _ if letters == 1 => {
                // 展开/纠错只改槽内读音：单字母槽显示用户敲的那个字母。展开后的音节全是该字母开头的全拼；至少消费一个。
                let initial = slot.raw_text.as_bytes()[0];
                loop {
                    if reading[read_index].as_bytes().first() != Some(&initial) {
                        return String::new();
                    }
                    read_index += 1;
                    let continues = read_index < reading.len()
                        && reading[read_index].as_bytes().first() == Some(&initial)
                        && reading[read_index].len() > 1
                        && i + 1 < slots.len()
                        && slots[i + 1].kind != FaultSlotKind::Determining
                        && slots[i + 1].raw_text.as_bytes()[0] == initial;
                    if !continues {
                        break;
                    }
                }
            }
            _ => {
                // 多字母非法槽：读音与原字母差一个字（漏/多字纠正）就对不上了。
                if letters != reading[read_index].len() {
                    return String::new();
                }
                read_index += 1;
            }
        }
        if i > 0 {
            display.push('\'');
        }
        display.push_str(&cased[consumed..consumed + letters]);
        consumed += letters;
    }
    if consumed != cased.len() || read_index != reading.len() || display.is_empty() {
        return String::new();
    }
    display
}

pub struct QuanpinDictionary<'a> {
    db_path: PathBuf,
    journal_path: String,
    dictionary: Option<Dictionary>,
    language_model: Option<&'a dyn LanguageModel>,
    association: SentenceAssociation,
    /// Google 解码器、神经模型、万象语法库和重排线程；没给就没有这些来源。
    sources: Option<Arc<SentenceSources>>,
    /// 上屏的前文，神经重排给模型看的条件前缀。
    rescoring_context: String,
    fuzzy_rules: u32,
    /// 整串结果缓存（C++ 的 `series_cache_`，容量 128），键见 `series_cache_key` 与 `SeriesQueryResolution::cache_key`。
    series_cache: RefCell<Cache<Vec<Item>>>,
    /// 多音单字各读音的权重和，词格第一次查跨度时载入，此后不随 reset_cache 重载。
    char_reading_totals: std::cell::OnceCell<crate::dict::CharReadingTotals>,
    /// 逐路径的结果缓存（C++ 的 `cache_`，容量 128）：单字母展开会把展开后的列表写进来，之后含这个声母的前缀都读到展开版。
    path_cache: RefCell<Cache<Vec<Item>>>,
    /// 词库上一次见到的 `PRAGMA data_version`，别的连接改过库就清缓存。
    data_version: Cell<i64>,
    /// 自己写库之前，别的连接已经改过词库；下一次缓存命中要清缓存。
    external_changes: Cell<bool>,
    pinyin_sequence: String,
    pinyin_segmentation: String,
    /// 纠错的备选读法（同档与贵档），给纠错标记用。
    pinyin_alternative_segmentations: Vec<String>,
    /// 手误别名层是否参与切分，由 `query` 按请求的纠错掩码更新，写回路径据此重切（见 `cut_for_write`）。还没查询过时保持历史行为（开）。
    correction_aliases_enabled: bool,
    /// 容错仲裁选中读音后的分段显示（原串大小写、含分隔符）。空表示当前串没有容错布局。
    fault_preedit_with_cases: String,
    domain: DomainDictionary,
}

impl<'a> QuanpinDictionary<'a> {
    /// `db_path` 是 `msime.db`，`journal_path` 是用户词库日志（`msime_user.db`）。词库打不开时所有查询都返回空。
    pub fn open(db_path: &Path, journal_path: &str, language_model: Option<&'a dyn LanguageModel>) -> Self {
        let mut dictionary = Self {
            db_path: db_path.to_path_buf(),
            journal_path: journal_path.to_string(),
            dictionary: None,
            language_model,
            association: SentenceAssociation::default(),
            sources: None,
            rescoring_context: String::new(),
            fuzzy_rules: 0,
            series_cache: RefCell::new(Cache::new(128)),
            char_reading_totals: std::cell::OnceCell::new(),
            path_cache: RefCell::new(Cache::new(128)),
            data_version: Cell::new(-1),
            external_changes: Cell::new(false),
            pinyin_sequence: String::new(),
            pinyin_segmentation: String::new(),
            pinyin_alternative_segmentations: Vec::new(),
            correction_aliases_enabled: true,
            fault_preedit_with_cases: String::new(),
            domain: DomainDictionary::new(&db_path.with_file_name("domain-input.db")),
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
        self.path_cache.borrow_mut().clear();
        self.series_cache.borrow_mut().clear();
    }

    /// 只清装着整句排序的那层：神经重排的结果与前文只影响整句的位置。
    pub fn reset_sentence_cache(&self) {
        self.series_cache.borrow_mut().clear();
    }

    /// 别的连接（设置页、另一个进程）改过词库就清缓存。
    fn reset_cache_if_database_changed(&self) {
        let Some(version) = self.current_data_version() else { return };
        let pending = self.external_changes.replace(false);
        if pending || (self.data_version.get() >= 0 && version != self.data_version.get()) {
            self.reset_cache();
        }
        self.data_version.set(version);
    }

    fn current_data_version(&self) -> Option<i64> {
        let dictionary = self.dictionary.as_ref()?;
        dictionary.connection().query_row("PRAGMA data_version", [], |row| row.get::<_, i64>(0)).ok()
    }

    /// 自己的写走另开的写连接，读连接会看到版本号变化；C++ 在自己的连接上写，不会。
    /// 写之前别的连接已经改过的先记成待清，写完再把版本号对齐，把自己这一次吸收掉。
    fn note_external_changes(&self) {
        if let Some(version) = self.current_data_version() {
            if self.data_version.get() >= 0 && version != self.data_version.get() {
                self.external_changes.set(true);
            }
        }
    }

    fn absorb_own_write(&self) {
        if let Some(version) = self.current_data_version() {
            self.data_version.set(version);
        }
    }

    pub fn set_sentence_association(&mut self, association: SentenceAssociation) {
        if self.association == association {
            return;
        }
        self.association = association;
        // 开关变了，之前缓存的候选列表按旧开关算出，必须清掉。
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
        // 上文只经神经重排进入候选：没有生效的神经模型时缓存与上文无关，不清（每次上屏都会换上文）。
        let rescoring_active = self.sources.as_deref().is_some_and(|sources| {
            (self.association.neural_keyboard && sources.keyboard_model().is_some())
                || (self.association.neural_desktop && sources.desktop_model(&self.association.neural_desktop_model).is_some())
        });
        if rescoring_active {
            self.reset_sentence_cache();
        }
    }

    /// 最近一次 `query` 的输入串。
    pub fn pinyin_sequence(&self) -> &str {
        &self.pinyin_sequence
    }

    /// 最近一次 `query` 定下的主切分（音节用 `'` 隔开）。
    /// 容错仲裁选中读音后的分段显示（大小写原样）。空表示没有容错布局，调用方照旧重建。
    pub fn fault_segmentation_with_cases(&self) -> &str {
        &self.fault_preedit_with_cases
    }

    pub fn pinyin_segmentation(&self) -> &str {
        &self.pinyin_segmentation
    }

    /// 任一整句来源开着。
    fn any_sentence_source(&self) -> bool {
        self.association.any_sentence_source()
    }

    /// Google 解码器的整句；没有解码器、词库缺失或解不出返回空串。
    fn search_sentence_from_ime_engine(&self, user_pinyin: &str) -> String {
        self.sources.as_ref().and_then(|sources| sources.google()).map(|decoder| decoder.sentence(user_pinyin, &[])).unwrap_or_default()
    }

    // ------------------------------------------------------------------------------------------
    // 查询
    // ------------------------------------------------------------------------------------------

    /// 对应 C++ `QuanpinDictionary::query`。`autocorrect_types` 是 [`autocorrect`] 里的类型位掩码，0 = 不纠错。
    pub fn query(&mut self, raw_input: &str, segmentation: &str, autocorrect_types: u32, fuzzy_rules: u32) -> Vec<Item> {
        // 词格要按同一套规则收模糊读音；规则变了，按旧规则算出的缓存作废。
        if self.fuzzy_rules != fuzzy_rules {
            self.reset_cache();
        }
        self.fuzzy_rules = fuzzy_rules;
        // 写回路径随后按这次查询的切分重切（cut_for_write），状态要和查询一致。
        self.correction_aliases_enabled = correction_aliases_enabled(autocorrect_types);
        let mut result = self.query_exact(raw_input, segmentation, autocorrect_types);
        if fuzzy_rules != 0 && !raw_input.is_empty() {
            let typed = if segmentation.is_empty() { join_segments(&self.resolve_segments(raw_input, segmentation, self.correction_aliases_enabled)) } else { segmentation.to_string() };
            let fuzzy_items = self.fuzzy_candidates(&typed, fuzzy_rules);
            result = merge_fuzzy_items(result, fuzzy_items);
        }
        if fuzzy_rules != 0 {
            keep_literal_mixed_words_first(&mut result, raw_input);
        }
        self.mark_autocorrect_candidates(&mut result, raw_input);
        result
    }

    fn query_exact(&mut self, raw_input: &str, segmentation: &str, autocorrect_types: u32) -> Vec<Item> {
        if raw_input.is_empty() {
            self.fault_preedit_with_cases.clear();
            return Vec::new();
        }
        // 每次查询先清掉上一次的容错布局：它只对当前这串有效。同一串的重复查询（整串缓存命中，仲裁不会重跑）不清。
        if self.pinyin_sequence != raw_input {
            self.fault_preedit_with_cases.clear();
        }
        self.pinyin_sequence = raw_input.to_string();
        let aliases = correction_aliases_enabled(autocorrect_types);
        let segments = self.resolve_segments(raw_input, segmentation, aliases);
        let resolution = resolve_series_query(raw_input, &segments, autocorrect_types);
        self.pinyin_segmentation = resolution.segmentation.clone();
        let primary_segmentation = self.pinyin_segmentation.clone();
        // 备选读法要发布给纠错标记：缓存命中时也一样，标记在每次查询之后都要跑。
        self.pinyin_alternative_segmentations = resolution
            .alternative_corrected_cuts
            .iter()
            .chain(&resolution.costlier_corrected_cuts)
            .map(|cut| join_segments(cut))
            .collect();

        // 纠错过的结果另占一个缓存槽，不会把兜底的尾巴带进拼对了的同键输入。
        if self.series_cache.borrow_mut().find(&resolution.cache_key).is_some() {
            self.reset_cache_if_database_changed();
            if let Some(cached) = self.series_cache.borrow_mut().find(&resolution.cache_key) {
                return cached;
            }
        }

        let mut alternative_segmentations: Vec<Segments> = Vec::new();
        let mut seen_segmentations: HashSet<String> = HashSet::from([primary_segmentation.clone()]);
        // 贵档读法不进“按词频竞争”的合并层，它们接在后面；先占住“已见”，免得纠错切分路径把它们带回合并层。
        for costlier in &resolution.costlier_corrected_cuts {
            seen_segmentations.insert(join_segments(costlier));
        }
        let mut append_alternative = |candidate: &Segments, alternatives: &mut Vec<Segments>| {
            let key = join_segments(candidate);
            if !key.is_empty() && seen_segmentations.insert(key) && alternatives.len() < SYLLABLE_GRAPH_PATH_LIMIT {
                alternatives.push(candidate.clone());
            }
        };

        // 纠错切分是纠错的一部分，要跟着开关走：没开纠错时不能因为拼错了就多出纠正后的候选。
        if autocorrect_types != 0 {
            // 纠错读法先进：它们解释了用户真正敲的字母，排在音形相近的别名读法之前。
            for candidate in &resolution.alternative_corrected_cuts {
                append_alternative(candidate, &mut alternative_segmentations);
            }
            for candidate in &cut_pinyin_by_mode(raw_input, CutMode::Correction, aliases) {
                append_alternative(candidate, &mut alternative_segmentations);
            }
        }
        if !raw_input.contains('\'') && segments.len() <= MAX_SYLLABLES_FOR_MULTIPLE_SEGMENTATIONS && has_only_complete_pinyin_segments(&segments) {
            for candidate in &enumerate_complete_segmentations(&build_syllable_graph(raw_input), SYLLABLE_GRAPH_PATH_LIMIT) {
                append_alternative(candidate, &mut alternative_segmentations);
            }
        }

        let mut result: Vec<Item>;
        let mut literal_mixed: Vec<Item> = Vec::new();
        // 字面整词提升只对两个及以上字母的输入成立：一个字母的输入任何单字都能整串吃下，提升只会把「嗯」「哦」顶到常用字前面。
        let promotes_literal_whole_word = fold_letters(raw_input).len() > 1;
        if resolution.corrected_input {
            result = self.query_series(raw_input, &primary_segmentation, &resolution.corrected_segments);
            let fallback_segmentation = if segmentation.is_empty() { join_segments(&segments) } else { segmentation.to_string() };
            let literal = self.query_series(raw_input, &fallback_segmentation, &segments);
            // 纯简拼字面整词与混合简拼同样领衔：词库里真有这个简拼读法时纠错读法只排在它后面，仍可选、仍带纠错标记。
            for candidate in &literal {
                if promotes_literal_whole_word && (literal_mixed_match(candidate, raw_input) != 0 || literal_jianpin_match(candidate, raw_input)) {
                    literal_mixed.push(candidate.clone());
                }
            }
            append_unique_words(&mut result, &literal);

            let context_reordered = self.reorder_by_context(raw_input, &primary_segmentation, &resolution, &alternative_segmentations, &mut result);
            // 同一处手误的几种读法按词频与主切竞争，最好的一条保住前排位置。
            if !context_reordered && !alternative_segmentations.is_empty() {
                result = self.merge_alternative_segmentations(raw_input, &primary_segmentation, &resolution.corrected_segments, &alternative_segmentations, result);
            }
            // 贵档读法接在整个主切档之后：高频的贵档纠正（gau → gai）不能只凭词频压过便宜的（gau → gua）。
            for costlier in &resolution.costlier_corrected_cuts {
                let extra = self.query_series(raw_input, &join_segments(costlier), costlier);
                append_unique_words(&mut result, &extra);
            }
        } else {
            result = self.query_series(raw_input, &primary_segmentation, &segments);
            if promotes_literal_whole_word {
                literal_mixed = result.iter().filter(|item| literal_mixed_match(item, raw_input) != 0).cloned().collect();
            }
            if !alternative_segmentations.is_empty() {
                result = self.merge_alternative_segmentations(raw_input, &primary_segmentation, &segments, &alternative_segmentations, result);
            }
            // 字面切分先争出领衔，纠错读法再与领衔的那条比：纠错胜出时第 2 位是得分最高的字面读法。
            let (arbitrated, leading_segments) = self.arbitrate_literal_segmentations(raw_input, &segments, result);
            result = arbitrated;
            if !resolution.legal_corrected_cuts.is_empty() {
                result = self.arbitrate_legal_corrections(raw_input, &leading_segments, &resolution.legal_corrected_cuts, result);
            }
            if promotes_literal_whole_word {
                keep_literal_whole_word_first(&mut result, raw_input);
            }
        }
        // k-best 空手而归或预算饱和的非法长句：容错槽位词格是唯一能长出正确切分的通道。短串（不超过 4 槽）不进。
        let correction_budget_saturated = resolution.corrected_input && resolution.corrected_segments.len() > autocorrect::MAX_SHORT_AUTOCORRECT_SYLLABLES;
        if !resolution.corrected_input || correction_budget_saturated {
            let (arbitrated, winning_segments, winning_display) = self.arbitrate_fault_slots(raw_input, autocorrect_types, result);
            result = arbitrated;
            // 预编辑必须跟着胜出读音走：展开读法永远过不了逐音节对回的比对，显示串在这里一并交出去。
            if !winning_segments.is_empty() && !winning_display.is_empty() {
                self.pinyin_segmentation = join_segments(&winning_segments);
                self.fault_preedit_with_cases = winning_display;
            }
        }
        if !literal_mixed.is_empty() {
            // 重排之后再并，同词去重时原有的元数据与词频次序说了算。
            keep_literal_mixed_words_first(&mut literal_mixed, raw_input);
            append_unique_words(&mut literal_mixed, &result);
            result = literal_mixed;
        }
        self.series_cache.borrow_mut().insert(&resolution.cache_key, result.clone());
        result
    }

    /// 上下文消解：任一整句来源开着时直接解出主切、同档备选与前几条贵档读法的最佳路径分，扣掉各自的手误代价
    /// 后裁决。胜出切分边际达标时领衔，返回是否接管了。整句来源全关或边际不足时什么都不改。
    fn reorder_by_context(
        &self,
        raw_input: &str,
        primary_segmentation: &str,
        resolution: &SeriesQueryResolution,
        alternative_segmentations: &[Segments],
        result: &mut Vec<Item>,
    ) -> bool {
        if !self.any_sentence_source()
            || (resolution.alternative_corrected_cuts.is_empty() && resolution.costlier_corrected_cuts.is_empty())
            || resolution.corrected_segments.len() < 2
            || !has_only_complete_pinyin_segments(&resolution.corrected_segments)
        {
            return false;
        }
        let Some(dictionary) = &self.dictionary else { return false };
        let best_path_score = |cut: &Segments| -> Option<f64> { if cut.len() < 2 { None } else { self.lattice_best_path_score(cut) } };
        let Some(primary_score) = best_path_score(&resolution.corrected_segments) else { return false };

        // 基准取正读整键命中的首位权重，没有整键行时取 0；不能取 result 的头部（可能是高权重前缀单字或生成的整句）。
        let primary_key = join_segments(&resolution.corrected_segments);
        let primary_full = self.query_single_path(raw_input, &primary_key, &resolution.corrected_segments);
        let primary_top_weight = primary_full.first().map_or(0, |candidate| candidate.weight);
        let alternative_items = dictionary.query_exact_paths(&resolution.alternative_corrected_cuts, ALTERNATIVE_SEGMENTATION_CANDIDATE_LIMIT).unwrap_or_default();
        // 每个备选切分的最佳词权重：行按权重降序返回，只留首行。
        let mut best_weight_by_key: HashMap<&str, i64> = HashMap::new();
        for row in &alternative_items {
            best_weight_by_key.entry(row.key.as_str()).or_insert(row.weight);
        }
        struct DecodeCandidate {
            top_weight: i64,
            cut: Segments,
            penalty: f64,
        }
        let primary_penalty = AUTOCORRECT_WEIGHT_TO_LOG * f64::from(resolution.corrected_weight);
        let mut decode_pool: Vec<DecodeCandidate> = Vec::new();
        for cut in &resolution.alternative_corrected_cuts {
            let Some(top_weight) = best_weight_by_key.get(join_segments(cut).as_str()).copied() else { continue };
            if top_weight * ALTERNATIVE_SEGMENTATION_PROMOTION_RATIO < primary_top_weight {
                continue;
            }
            decode_pool.push(DecodeCandidate { top_weight, cut: cut.clone(), penalty: primary_penalty });
        }
        decode_pool.sort_by(|lhs, rhs| rhs.top_weight.cmp(&lhs.top_weight));
        decode_pool.truncate(AUTOCORRECT_CONTEXT_DECODE_LIMIT);
        // 贵档读法按 k-best 次序再解几条，不要求词库有整词；多付的手误代价按纠错表权重差计入总分。
        let mut costlier_decoded = 0;
        for (i, cut) in resolution.costlier_corrected_cuts.iter().enumerate() {
            if costlier_decoded >= COSTLIER_CONTEXT_DECODE_LIMIT {
                break;
            }
            if !resolution.costlier_promotable[i] {
                continue;
            }
            costlier_decoded += 1;
            let top_weight = best_weight_by_key.get(join_segments(cut).as_str()).copied().unwrap_or(0);
            decode_pool.push(DecodeCandidate { top_weight, cut: cut.clone(), penalty: AUTOCORRECT_WEIGHT_TO_LOG * f64::from(resolution.costlier_weights[i]) });
        }
        // 第一遍只解路径分选胜者。总分 = 路径分 - 手误代价，同档读法代价相同，比的仍是纯上下文差。
        let mut winner_total: Option<f64> = None;
        let mut winner_top_weight = 0;
        let mut winner_segments: Segments = Vec::new();
        for candidate in &decode_pool {
            let Some(cut_score) = best_path_score(&candidate.cut) else { continue };
            let total = cut_score - candidate.penalty;
            if winner_total.is_none_or(|best| total > best) {
                winner_total = Some(total);
                winner_top_weight = candidate.top_weight;
                winner_segments = candidate.cut.clone();
            }
        }
        // 用户在主切读音上的选择优先于上下文：主切整键首位权重不低于胜出切分首位、且主切键有用户 upsert。
        let Some(winner_total) = winner_total else { return false };
        if winner_total - (primary_score - primary_penalty) < AUTOCORRECT_CONTEXT_MARGIN_LOG {
            return false;
        }
        if primary_top_weight >= winner_top_weight && self.user_prefers_reading(&primary_key) {
            return false;
        }

        // 胜出切分领衔。merge_alternative_segmentations 假定传入 result 的头部是主切词库行，所以仍以主切为锚组装
        // 其余列表（从合并池摘掉胜出切分），再把胜出列表接到最前。
        let winner_key = join_segments(&winner_segments);
        let remaining_alternatives: Vec<Segments> = alternative_segmentations.iter().filter(|alternative| join_segments(alternative) != winner_key).cloned().collect();
        let rest = self.merge_alternative_segmentations(raw_input, primary_segmentation, &resolution.corrected_segments, &remaining_alternatives, std::mem::take(result));
        // 整键块、rest、前缀块：整句与词格行跟整键行同键，按 canonical_pinyin == winner_key 能把前缀行摘出去。
        let (mut winner_full, mut winner_prefix): (Vec<Item>, Vec<Item>) = (Vec::new(), Vec::new());
        for candidate in self.query_series(raw_input, &winner_key, &winner_segments) {
            if reads_full_key(&candidate, &winner_key) {
                winner_full.push(candidate);
            } else {
                winner_prefix.push(candidate);
            }
        }
        // 贵档胜者没有整词、也没有整句行时整键块是空的；由胜者的最长前缀词领衔，不能让主切的行顶回首位。
        if winner_full.is_empty() {
            std::mem::swap(&mut winner_full, &mut winner_prefix);
        }
        *result = winner_full;
        append_unique_words(result, &rest);
        append_unique_words(result, &winner_prefix);
        true
    }

    /// 合法输入上的手误（ziazheliya、jioa、jiuzheeyang、nia）按噪声信道整句打分：
    /// 总分 = 词格最佳路径分 - 每处纠正的手误代价（换位 2.0，多字/漏字 3.0），原读法代价为 0。
    /// 最高分领衔，另一方的最佳读法占第 2 位：纠错领衔时原读法永远在第 2 位；原读法领衔时，纠错读法只在分差不超过
    /// `LEGAL_INPUT_RUNNER_UP_GAP_LOG` 时出现。用户在原读法键上调过频或造过词时，纠错不抢领衔。
    fn arbitrate_legal_corrections(&self, raw_input: &str, plain_segments: &[String], corrected_cuts: &[Cut], result: Vec<Item>) -> Vec<Item> {
        // 字面切分本身就是真词时（邪恶 xie'e、答案 da'an），不认改变字母数的读法。
        let literal_is_word = find_literal_whole_word(&result, raw_input).is_some();
        let mut best_total: Option<f64> = None;
        let mut best_cut: Option<&Cut> = None;
        for cut in corrected_cuts {
            let mut penalty = 0.0;
            let mut changes_length = false;
            for segment in cut.segments.iter().filter(|segment| segment.corrected) {
                let same_length = segment.raw.len() == segment.syllable.len();
                changes_length = changes_length || !same_length;
                penalty += if same_length { LEGAL_INPUT_TRANSPOSITION_PENALTY_LOG } else { LEGAL_INPUT_LENGTH_CHANGE_PENALTY_LOG };
            }
            if literal_is_word && changes_length {
                continue;
            }
            let Some(score) = self.lattice_best_path_score(&cut.syllables()) else { continue };
            let total = score - penalty;
            if best_total.is_none_or(|best| total > best) {
                best_total = Some(total);
                best_cut = Some(cut);
            }
        }
        let (Some(best_total), Some(best_cut)) = (best_total, best_cut) else { return result };
        let plain_total = self.lattice_best_path_score(plain_segments);

        let corrected_segments = best_cut.syllables();
        let corrected_key = join_segments(&corrected_segments);
        let mut corrected = self.query_series(raw_input, &corrected_key, &corrected_segments);
        if corrected.is_empty() {
            return result;
        }
        // 只给纠错读法的整读音候选打标（读音字母等于纠错读法、又不同于敲的字母），前缀单字不标。
        let typed_letters = fold_letters(raw_input);
        let corrected_letters = fold_letters(&corrected_key);
        for candidate in &mut corrected {
            if candidate.corrected_from.is_empty() && fold_letters(&candidate.pinyin) == corrected_letters && corrected_letters != typed_letters {
                candidate.corrected_from = typed_letters.clone();
            }
        }

        let correction_leads = plain_total.is_none_or(|plain| best_total > plain) && !self.user_prefers_reading(&join_segments(plain_segments));
        if !correction_leads && plain_total.is_none_or(|plain| plain - best_total > LEGAL_INPUT_RUNNER_UP_GAP_LOG) {
            return result;
        }
        let (leader, runner) = if correction_leads { (corrected, result) } else { (result, corrected) };
        let mut merged: Vec<Item> = Vec::new();
        if let Some(first) = leader.first() {
            merged.push(first.clone());
        }
        if let Some(first) = runner.first() {
            append_unique_words(&mut merged, std::slice::from_ref(first));
        }
        append_unique_words(&mut merged, &leader);
        append_unique_words(&mut merged, &runner);
        merged
    }

    fn resolve_segments(&self, raw_input: &str, segmentation: &str, include_correction_aliases: bool) -> Segments {
        let segments = if segmentation.is_empty() {
            cut_pinyin_by_mode(raw_input, CutMode::Correction, include_correction_aliases).into_iter().next().unwrap_or_default()
        } else {
            split_segments(segmentation)
        };
        normalize_umlaut_aliases(segments)
    }

    fn query_series(&self, raw_input: &str, segmentation: &str, segments: &[String]) -> Vec<Item> {
        if segments.is_empty() {
            return self.query_single_path(raw_input, segmentation, segments);
        }
        let mut result: Vec<Item> = Vec::new();
        for count in (1..=segments.len()).rev() {
            let partial_segments = &segments[..count];
            let partial_segmentation = join_segments(partial_segments);
            let partial_input = remove_delimiters(&partial_segmentation);
            // 整句联想只对完整输入出一句：前缀只取词库里真有的词，查不到就交给更短的前缀。
            if count < segments.len() {
                result.extend(self.query_prefix_path(&partial_input, &partial_segmentation, partial_segments));
            } else {
                result.extend(self.query_single_path(&partial_input, &partial_segmentation, partial_segments));
            }
        }

        if segments.len() >= 2 && has_only_complete_pinyin_segments(segments) {
            let typed = if segmentation.is_empty() { raw_input } else { segmentation };
            // Google 整句（Fallback）：词格在前、Google 在后，都插在“整串精确命中词库”的候选之后。
            // 手打了分隔符时原样传下去（撇号是解码器认的音节分隔符），否则喂裸串，让它自己重新切分；
            // 送进去之前 ü 的写法要换成它认的。
            if self.association.google {
                let google_input = if raw_input.contains('\'') { to_google_spelling(raw_input) } else { remove_delimiters(&to_google_spelling(typed)) };
                let google_sentence = self.search_sentence_from_ime_engine(&google_input);
                if !google_sentence.is_empty() && !result.iter().any(|candidate| candidate.word == google_sentence) {
                    // 整句 fallback 要带 canonical 读音，否则以它结尾的造词无法落库。
                    let insert_at = lattice::generated_sentence_insert_position(&result, segments);
                    let mut sentence = item(typed, &google_sentence, 1, Source::Fallback, segmentation);
                    sentence.sentence_association = true;
                    result.insert(insert_at, sentence);
                }
            }

            // 词格给 Trigram 候选和神经模型共用：只开神经时仍在内部解出 n-best 供它重排，但不显示词格自己的首选。
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

            // 万象语法：搭配分在解码期逐边叠加一次，〔万象〕行就是解码最优路径。
            let collocation = sources.and_then(|s| s.gram(&self.association.collocation_model));
            let has_collocation = collocation.is_some() && self.association.collocation_weight != 0.0;
            let grammar = self.association.collocation_grammar;
            let collocation_score = |tail: &str, word: &str, is_rear: bool| -> f64 {
                collocation.as_ref().map_or(0.0, |db| db.query(tail, word, is_rear, &grammar, None) * LN_TO_LOG10)
            };

            // 神经模型和去重补位都需要完整候选池；否则只解最优的一条。
            let needs_alternatives = !rerankers.is_empty() || (self.association.word_lattice && self.association.show_next_on_duplicate);
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
            if self.association.word_lattice || !rerankers.is_empty() || options.include_collocation_best {
                let plain = |span: &[String]| self.lattice_lookup(span, options.span_limit, true);
                lattice::merge_lattice_candidates(&mut result, segments, &plain, typed, &options, &rerankers);
            }
        }

        if result.len() < SPARSE_PINYIN_FALLBACK_THRESHOLD {
            result = self.append_sparse_pinyin_fallbacks(segments, result);
        }
        result
    }

    /// 查库，词库一行都没有时补一条 Google 兜底整句（随“Google 整句联想”开关一起关闭）。
    fn query_single_path(&self, raw_input: &str, segmentation: &str, segments: &[String]) -> Vec<Item> {
        let cache_key = if segmentation.is_empty() { raw_input } else { segmentation };
        if let Some(cached) = self.path_cache.borrow_mut().find(cache_key) {
            return cached;
        }
        let result = self.query_database(segments, segmentation);
        let result = self.append_ime_fallback(raw_input, segmentation, result);
        self.path_cache.borrow_mut().insert(cache_key, result.clone());
        result
    }

    /// 前缀只取词库里真有的词；查到才写缓存（兜底整句只在词库查不到时才补，所以这正是 `query_single_path` 会写入的值）。
    fn query_prefix_path(&self, raw_input: &str, segmentation: &str, segments: &[String]) -> Vec<Item> {
        let cache_key = if segmentation.is_empty() { raw_input } else { segmentation };
        if let Some(cached) = self.path_cache.borrow_mut().find(cache_key) {
            return cached;
        }
        let result = self.query_database(segments, segmentation);
        if !result.is_empty() {
            self.path_cache.borrow_mut().insert(cache_key, result.clone());
        }
        result
    }

    fn append_ime_fallback(&self, raw_input: &str, segmentation: &str, mut result: Vec<Item>) -> Vec<Item> {
        if !result.is_empty() || !self.association.google {
            return result;
        }
        let typed = if segmentation.is_empty() { raw_input } else { segmentation };
        let sentence = self.search_sentence_from_ime_engine(&remove_delimiters(typed));
        if sentence.is_empty() {
            return result;
        }
        // 同上，整句 fallback 需要 canonical 读音才能参与造词落库。
        let mut fallback = item(typed, &sentence, 1, Source::Fallback, segmentation);
        fallback.sentence_association = true;
        result.push(fallback);
        result
    }

    fn query_database(&self, segments: &[String], segmentation: &str) -> Vec<Item> {
        let Some(dictionary) = &self.dictionary else { return Vec::new() };
        if segments.len() == 1 && segments[0].len() == 1 {
            let rows = dictionary.query_initial(&segments[0], INITIAL_CANDIDATE_LIMIT as i64).unwrap_or_default();
            let matched_code = if segmentation.is_empty() { segments[0].as_str() } else { segmentation };
            let mut result: Vec<Item> = rows.iter().map(|row| item(matched_code, &row.text, row.weight, Source::Database, &row.key)).collect();
            domain_dictionary::merge(&mut result, self.domain.query(segments, matched_code, INITIAL_CANDIDATE_LIMIT, QuerySource::Quanpin));
            return result;
        }
        let rows = dictionary.query_keyed_flat(segments, i32::MAX as usize, QuerySource::Quanpin).unwrap_or_default();
        let code = if segmentation.is_empty() { join_segments(segments) } else { segmentation.to_string() };
        let mut result: Vec<Item> = rows.iter().map(|row| keyed_item(&code, row)).collect();
        domain_dictionary::merge(&mut result, self.domain.query(segments, &code, DOMAIN_QUERY_LIMIT, QuerySource::Quanpin));
        result
    }

    /// 单字母输入只取了 24 条时，把它们换成全部（并回原来的位置）。返回是否扩展了。
    pub fn expand_initial_candidates(&self, code: &str, candidates: &mut Vec<Item>) -> bool {
        if code.len() != 1 {
            return false;
        }
        let is_limited_initial = |candidate: &Item| candidate.source == Source::Database && candidate.pinyin == code;
        let limited_count = candidates.iter().filter(|candidate| is_limited_initial(candidate)).count();
        if limited_count != INITIAL_CANDIDATE_LIMIT {
            return false;
        }
        let Some(dictionary) = &self.dictionary else { return false };
        let rows = dictionary.query_initial(code, i64::from(i32::MAX)).unwrap_or_default();
        let expanded: Vec<Item> = rows.iter().map(|row| item(code, &row.text, row.weight, Source::Database, &row.key)).collect();
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
        self.path_cache.borrow_mut().insert(code, expanded);
        self.series_cache.borrow_mut().insert(&series_cache_key(&self.pinyin_sequence, &self.pinyin_segmentation), candidates.clone());
        true
    }

    /// 词库里这个键下这个词的权重。
    pub fn find_candidate(&self, key: &str, value: &str) -> Option<Item> {
        let dictionary = self.dictionary.as_ref()?;
        let table = table_name(&split_segments(key))?;
        let sql = format!("SELECT weight FROM \"{table}\" WHERE key=?1 AND value=?2 LIMIT 1");
        let weight: i64 = dictionary.connection().query_row(&sql, rusqlite::params![key, value], |row| row.get(0)).ok()?;
        Some(item(key, value, weight, Source::Database, key))
    }

    /// 模糊候选：从整串往短试，每个长度取近音路径的词，路径预算 128 条（每次最多 64 条）。
    pub fn fuzzy_candidates(&self, segmentation: &str, rules: u32) -> Vec<Item> {
        let mut result = Vec::new();
        let Some(dictionary) = &self.dictionary else { return result };
        if rules == 0 {
            return result;
        }
        self.reset_cache_if_database_changed();
        let cache_key = format!("fuzzy:{rules}:{segmentation}");
        if let Some(cached) = self.series_cache.borrow_mut().find(&cache_key) {
            return cached;
        }
        let segments = split_segments(segmentation);
        let mut budget = 128usize;
        for count in (1..=segments.len()).rev() {
            if budget <= 1 {
                break;
            }
            let prefix = &segments[..count];
            let paths = fuzzy::fuzzy_segmentations(prefix, rules, budget.min(64));
            if paths.is_empty() {
                continue;
            }
            budget -= paths.len();
            let rows = dictionary.query_exact_paths(&paths, 128).unwrap_or_default();
            let typed = join_segments(prefix);
            for row in &rows {
                let mut candidate = keyed_item(&typed, row);
                candidate.fuzzy = true;
                result.push(candidate);
            }
            let mut domain_rows = self.domain.exact(&paths, DOMAIN_QUERY_LIMIT);
            for row in &mut domain_rows {
                row.pinyin = typed.clone();
                row.fuzzy = true;
            }
            domain_dictionary::merge(&mut result, domain_rows);
        }
        self.series_cache.borrow_mut().insert(&cache_key, result.clone());
        result
    }

    /// 词格的跨度查询：词库行加上模糊音变体，按折后权重稳定排序，截到 `limit`。`with_fuzzy` 为假时不收模糊读音。
    fn lattice_lookup(&self, span: &[String], limit: i32, with_fuzzy: bool) -> Vec<Lexeme> {
        let Some(dictionary) = &self.dictionary else { return Vec::new() };
        let rules = if with_fuzzy { self.fuzzy_rules } else { 0 };
        let totals = self.char_reading_totals.get_or_init(|| dictionary.load_char_reading_totals());
        let mut rows = dictionary.lattice_lookup_with(span, limit, rules, Some(totals)).unwrap_or_default();
        for row in self.domain.lattice(span, limit, rules) {
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

    /// 长句容错仲裁。字面贪心切分对带错的长句切出 shu'r'fa / c / l 这类「每段都有解释」的槽位，k-best 纠错被 3 边预算卡死返回空。
    /// 这里把槽位图交给词格：合法槽固定读音（走普通跨度查询），单字母槽由词库区间扫描现算简拼展开候选，Fault 槽用纠错表目标作前缀扫描。
    /// 裁决只看语言模型，路径分扣手误代价——纠错表目标按 `AUTOCORRECT_WEIGHT_TO_LOG`、简拼展开按 `JIANPIN_EXPAND_PENALTY_LOG` 折算。
    /// 返回（候选，胜出读音，胜出读音的分段显示）；不接管或词格解不出路径时原样返回候选、后两者为空。
    fn arbitrate_fault_slots(&self, raw_input: &str, autocorrect_types: u32, result: Vec<Item>) -> (Vec<Item>, Segments, String) {
        let slots = autocorrect::build_fault_slots(raw_input);
        if !autocorrect::fault_slots_eligible(&slots, autocorrect_types) {
            return (result, Vec::new(), String::new());
        }
        let Some(dictionary) = &self.dictionary else { return (result, Vec::new(), String::new()) };

        // 与 lattice_best_path 同参：不带模糊音，裁决只认敲定的读音。跨度行数与 n-best 收窄：这条解码只为选出胜出读音。
        let options = lattice::Options { language_model: self.language_model, span_limit: 16, nbest: 3, ..lattice::Options::default() };
        let lookup = |span: &[String]| self.lattice_lookup(span, options.span_limit, false);

        let slot_count = slots.len();
        const MAX_FAULT_SPAN_SLOTS: usize = 5;
        // 一个跨度至多盖住两个可展开槽：更多的组合面指数涨，有界放弃。
        const MAX_EXPANDABLE_SLOTS_PER_SPAN: usize = 2;
        const MAX_FAULT_TARGET_SCANS: usize = 4;
        // 字面槽变体扫描（漏字母：ti'yang -> ti'yan）每跨度至多几个。
        const MAX_LOOSE_DETERMINING_SCANS: usize = 2;
        let span_pieces = |start: usize, end: usize| -> Segments { (start..end).map(|i| slots[i].raw_text.clone()).collect() };
        // 末字母 +1 作开区间上界；前缀末位必是字母。
        let upper_bound = |prefix: &str| -> String {
            let mut upper = prefix.to_string();
            let last = upper.pop().unwrap_or('a');
            upper.push((last as u8 + 1) as char);
            upper
        };
        // 同一词条只留罚得最少的那份。
        let better = |candidate: &FaultEdge, current: &FaultEdge| -> bool {
            current.word.is_empty() || candidate.weight as f64 * 10f64.powf(-candidate.penalty) > current.weight as f64 * 10f64.powf(-current.penalty)
        };

        let mut edges: Vec<Vec<FaultEdge>> = (0..slot_count).map(|_| Vec::new()).collect();
        for start in 0..slot_count {
            let max_end = slot_count.min(start + MAX_FAULT_SPAN_SLOTS);
            for end in start + 1..=max_end {
                let mut first_expandable = end;
                let mut expandable = 0;
                for i in start..end {
                    if slots[i].kind != FaultSlotKind::Determining {
                        if first_expandable == end {
                            first_expandable = i;
                        }
                        expandable += 1;
                    }
                }
                if expandable > MAX_EXPANDABLE_SLOTS_PER_SPAN {
                    continue;
                }

                let raw_key = (start..end).map(|i| slots[i].raw_text.as_str()).collect::<Vec<_>>().join("'");
                // 表按跨度槽数 + 首槽首字母选。
                let table = table_name(&span_pieces(start, end)).unwrap_or_default();

                let mut best_edges: Vec<FaultEdge> = Vec::new();
                let mut best_index: HashMap<String, usize> = HashMap::new();
                if expandable == 0 {
                    // 全 Determining：无展开读音，走普通精确跨度查询。ü 归一后的键可能与字面差一个别名写法，那正是词库存的写法。
                    for row in lookup(&span_pieces(start, end)) {
                        let edge = FaultEdge {
                            start,
                            end,
                            word: row.value.clone(),
                            key: if row.key.is_empty() { raw_key.clone() } else { row.key.clone() },
                            weight: row.weight,
                            penalty: row.penalty,
                        };
                        match best_index.get(&row.value) {
                            Some(&at) => best_edges[at] = edge,
                            None => {
                                best_index.insert(row.value.clone(), best_edges.len());
                                best_edges.push(edge);
                            }
                        }
                    }
                } else {
                    // 扫描前缀集合：SingleInitial 槽一个前缀（槽首字母），Fault 槽每个纠错目标一个（各有界）。
                    let mut head = (start..first_expandable).map(|i| slots[i].raw_text.as_str()).collect::<Vec<_>>().join("'");
                    if first_expandable > start {
                        head.push('\'');
                    }
                    let mut scan_prefixes: Vec<String> = Vec::new();
                    let first = &slots[first_expandable];
                    if first.kind == FaultSlotKind::SingleInitial {
                        scan_prefixes.push(format!("{head}{}", &first.raw_text[..1]));
                    } else {
                        for target in autocorrect::fault_slot_correction_targets(&first.raw_text, autocorrect_types) {
                            scan_prefixes.push(format!("{head}{}", target.0));
                            if scan_prefixes.len() >= MAX_FAULT_TARGET_SCANS {
                                break;
                            }
                        }
                    }

                    for scan_prefix in &scan_prefixes {
                        let upper = upper_bound(scan_prefix);
                        let keyed = dictionary.query_key_range_keyed(&table, scan_prefix, &upper, options.span_limit).unwrap_or_default();
                        for row in &keyed {
                            let syllables = split_segments(&row.key);
                            if syllables.len() != end - start {
                                continue;
                            }
                            let mut penalty = 0.0;
                            let mut valid = true;
                            for i in start..end {
                                if !valid {
                                    break;
                                }
                                let slot = &slots[i];
                                let reading = &syllables[i - start];
                                match slot.kind {
                                    // 前面槽的展开会移动该槽的键位置，只有逐槽等于字面（ü 归一后）才不是跨边界重切。
                                    FaultSlotKind::Determining => {
                                        if canonical_umlaut_spelling(reading) != canonical_umlaut_spelling(&slot.base_syllable) {
                                            valid = false;
                                        }
                                    }
                                    FaultSlotKind::SingleInitial => {
                                        if *reading != slot.raw_text {
                                            penalty += JIANPIN_EXPAND_PENALTY_LOG;
                                        }
                                    }
                                    FaultSlotKind::Fault => {
                                        let targets = autocorrect::fault_slot_correction_targets(&slot.raw_text, autocorrect_types);
                                        match targets.iter().find(|entry| entry.0 == *reading) {
                                            None => valid = false,
                                            Some(found) => penalty += f64::from(found.1) * AUTOCORRECT_WEIGHT_TO_LOG,
                                        }
                                    }
                                }
                            }
                            if !valid {
                                continue;
                            }
                            let candidate = FaultEdge { start, end, word: row.text.clone(), key: row.key.clone(), weight: row.weight, penalty };
                            let at = *best_index.entry(row.text.clone()).or_insert_with(|| {
                                best_edges.push(FaultEdge::default());
                                best_edges.len() - 1
                            });
                            if better(&candidate, &best_edges[at]) {
                                best_edges[at] = candidate;
                            }
                        }
                    }
                }
                edges[start].append(&mut best_edges);
                best_index.clear();

                // 字面槽的容错变体扫描：跨度内某个 Determining 槽读成同首字母的别的合法音节（ti'yang 的 yang 漏了 g -> ti'yan 体验）。
                // 扫描前缀 = 前面全部字面槽 + 该槽首字母，只有前面槽全部 Determining 时才定得下来；每跨度至多两个。
                let mut loose_scans = 0;
                let mut i = start + 1;
                while i < end && loose_scans < MAX_LOOSE_DETERMINING_SCANS {
                    let at_i = i;
                    i += 1;
                    if slots[at_i].kind != FaultSlotKind::Determining {
                        continue;
                    }
                    if !(start..at_i).all(|j| slots[j].kind == FaultSlotKind::Determining) {
                        continue;
                    }
                    let mut loose_prefix = (start..at_i).map(|j| slots[j].raw_text.as_str()).collect::<Vec<_>>().join("'");
                    // 循环从 start + 1 起，前面至少有一个字面槽，分隔符恒要补。
                    loose_prefix.push('\'');
                    loose_prefix.push_str(&slots[at_i].raw_text[..1]);
                    let loose_upper = upper_bound(&loose_prefix);
                    loose_scans += 1;
                    let keyed = dictionary.query_key_range_keyed(&table, &loose_prefix, &loose_upper, options.span_limit).unwrap_or_default();
                    for row in &keyed {
                        let syllables = split_segments(&row.key);
                        if syllables.len() != end - start {
                            continue;
                        }
                        let mut penalty = 0.0;
                        let mut valid = true;
                        for j in start..end {
                            if !valid {
                                break;
                            }
                            let slot = &slots[j];
                            let reading = &syllables[j - start];
                            if j == at_i {
                                // 漏字变体形态约束：字面读法不罚；变体必须能从槽原文删一个字母得到（shu -> sh/su、yang -> yan/yag）。
                                if *reading == canonical_umlaut_spelling(&slot.base_syllable) {
                                    continue;
                                }
                                if !autocorrect::is_deletion_variant(&slot.raw_text, reading) {
                                    valid = false;
                                    break;
                                }
                                penalty += f64::from(autocorrect::WEIGHT_DELETION) * AUTOCORRECT_WEIGHT_TO_LOG;
                                continue;
                            }
                            if canonical_umlaut_spelling(reading) != canonical_umlaut_spelling(&slot.base_syllable) {
                                valid = false;
                            }
                        }
                        if !valid {
                            continue;
                        }
                        let candidate = FaultEdge { start, end, word: row.text.clone(), key: row.key.clone(), weight: row.weight, penalty };
                        let at = *best_index.entry(row.text.clone()).or_insert_with(|| {
                            best_edges.push(FaultEdge::default());
                            best_edges.len() - 1
                        });
                        if better(&candidate, &best_edges[at]) {
                            best_edges[at] = candidate;
                        }
                    }
                }
                edges[start].append(&mut best_edges);
            }
        }

        let paths = lattice::decode_fault_lattice(slot_count, &edges, &options);
        let Some(top) = paths.first() else { return (result, Vec::new(), String::new()) };
        // 路径已按「路径分 - 手误代价」排好；胜出读音的键就是实际读法（含展开与纠错目标）。
        let winner_segments = split_segments(&top.key);
        if winner_segments.is_empty() || !has_only_complete_pinyin_segments(&winner_segments) {
            return (result, Vec::new(), String::new());
        }
        let winner_key = join_segments(&winner_segments);
        let mut corrected = self.query_series(raw_input, &winner_key, &winner_segments);
        if corrected.is_empty() {
            return (result, Vec::new(), String::new());
        }
        // 与 arbitrate_legal_corrections 同判据打纠错标：读音字母不同于敲的字母才标。
        let typed_letters = fold_letters(raw_input);
        let corrected_letters = fold_letters(&winner_key);
        if corrected_letters != typed_letters {
            for candidate in &mut corrected {
                if candidate.corrected_from.is_empty() && fold_letters(&candidate.pinyin) == corrected_letters {
                    candidate.corrected_from = typed_letters.clone();
                }
            }
        }

        let winning_display = display_at_winning_slots(raw_input, &winner_segments);
        // 胜出读音的整键块领衔，原字面候选紧随其后（纠错可能误判，原候选留在手边）。
        let (mut winner_full, mut winner_prefix): (Vec<Item>, Vec<Item>) = (Vec::new(), Vec::new());
        for candidate in corrected {
            if reads_full_key(&candidate, &winner_key) {
                winner_full.push(candidate);
            } else {
                winner_prefix.push(candidate);
            }
        }
        if winner_full.is_empty() {
            std::mem::swap(&mut winner_full, &mut winner_prefix);
        }
        let mut merged: Vec<Item> = Vec::new();
        if let Some(first) = winner_full.first() {
            merged.push(first.clone());
        }
        append_unique_words(&mut merged, &winner_full);
        append_unique_words(&mut merged, &result);
        append_unique_words(&mut merged, &winner_prefix);
        (merged, winner_segments, winning_display)
    }

    /// 词格最佳路径：只用语言模型、不带模糊音，给切分裁决打分。
    fn lattice_best_path(&self, cut: &[String]) -> Option<LatticePath> {
        if cut.is_empty() || !has_only_complete_pinyin_segments(cut) {
            return None;
        }
        let options = lattice::Options { language_model: self.language_model, ..lattice::Options::default() };
        let lookup = |span: &[String]| self.lattice_lookup(span, options.span_limit, false);
        lattice::decode_word_lattice(cut, &lookup, &options).into_iter().next()
    }

    fn lattice_best_path_score(&self, cut: &[String]) -> Option<f64> {
        self.lattice_best_path(cut).map(|path| path.log_prob)
    }

    /// 用户在这个读音键上有过调频或造词记录。
    fn user_prefers_reading(&self, key: &str) -> bool {
        user_dictionary::has_user_upsert_for_key(&self.journal_path, DictionaryKind::Pinyin, key)
    }

    fn append_sparse_pinyin_fallbacks(&self, segments: &[String], mut result: Vec<Item>) -> Vec<Item> {
        for fallback_segments in sparse_pinyin_fallback_segments(segments) {
            if fallback_segments.is_empty() {
                continue;
            }
            let fallback_segmentation = join_segments(&fallback_segments);
            let fallback_input = remove_delimiters(&fallback_segmentation);
            let fallback_result = self.query_single_path(&fallback_input, &fallback_segmentation, &fallback_segments);
            append_unique_words(&mut result, &fallback_result);
        }
        result
    }

    // ------------------------------------------------------------------------------------------
    // 备选切分与仲裁
    // ------------------------------------------------------------------------------------------

    fn merge_alternative_segmentations(
        &self,
        raw_input: &str,
        primary_segmentation: &str,
        primary_segments: &[String],
        alternative_segmentations: &[Segments],
        result: Vec<Item>,
    ) -> Vec<Item> {
        let Some(dictionary) = &self.dictionary else { return result };
        let alternative_items = dictionary.query_exact_paths(alternative_segmentations, ALTERNATIVE_SEGMENTATION_CANDIDATE_LIMIT).unwrap_or_default();
        let domain_alternatives = self.domain.exact(alternative_segmentations, DOMAIN_QUERY_LIMIT);
        if alternative_items.is_empty() && domain_alternatives.is_empty() {
            return result;
        }
        let primary_full = self.query_single_path(raw_input, primary_segmentation, primary_segments);
        let mut alternative_full: Vec<Item> = alternative_items.iter().map(|row| item(&row.key, &row.text, row.weight, Source::Database, &row.key)).collect();
        domain_dictionary::merge(&mut alternative_full, domain_alternatives);

        let mut merged_full = primary_full.clone();
        merged_full.extend(alternative_full);
        merged_full.sort_by(|lhs, rhs| rhs.weight.cmp(&lhs.weight));
        let mut seen_full_words = HashSet::new();
        merged_full.retain(|candidate| seen_full_words.insert(candidate.word.clone()));

        // 权重在同一个读音键内可比，跨切分不可直接比：备选读音的最佳词不能被主切分首位压得太远才配保护位。
        let primary_top_weight = primary_full.first().map_or(0, |candidate| candidate.weight);
        // 词库没有备选整词、只有领域词库有时，C++ 在这里取空向量的 front()（未定义行为）；这里当作没有保护位。
        let best = alternative_items.first();
        let promote_alternative = best.is_some_and(|row| row.weight * ALTERNATIVE_SEGMENTATION_PROMOTION_RATIO >= primary_top_weight);
        let best_alternative = best.and_then(|row| merged_full.iter().position(|candidate| candidate.word == row.text));
        // 只把首页外的备选拉进首页；已经排进首页的候选位置是权重排出来的，不再钉一次。
        if let Some(index) = best_alternative {
            if promote_alternative && index >= ALTERNATIVE_SEGMENTATION_FIRST_PAGE_SIZE {
                let promoted = merged_full.remove(index);
                merged_full.insert(BEST_ALTERNATIVE_SEGMENTATION_MAX_INDEX, promoted);
            }
        }

        let mut merged = merged_full;
        let primary_full_count = primary_full.len().min(result.len());
        append_unique_words(&mut merged, &result[primary_full_count..]);
        merged
    }

    /// 合法输入常有不止一种等音节数的切分（yibananzhao = yi'ba'nan'zhao / yi'ban'an'zhao），切分是在查词之前定的，
    /// 没有上下文，这里把它交给整句打分。任一条切分在词库里有整词时不介入；否则路径分最高的领衔，
    /// 输的一方分差不超过 `LEGAL_INPUT_RUNNER_UP_GAP_LOG` 时它的整句占第 2 位。
    fn arbitrate_literal_segmentations(&self, raw_input: &str, segments: &[String], result: Vec<Item>) -> (Vec<Item>, Segments) {
        let unchanged = |result: Vec<Item>| (result, segments.to_vec());
        if !self.any_sentence_source() || result.is_empty() || segments.len() < 2 || raw_input.contains('\'') || !has_only_complete_pinyin_segments(segments) {
            return unchanged(result);
        }
        let Some(dictionary) = &self.dictionary else { return unchanged(result) };

        // 主切分必须就是字面切分之一（别名改写过字母的读法不算），备选是其余等音节数的切分。
        let mut primary_is_literal = false;
        let mut alternatives: Vec<Segments> = Vec::new();
        for cut in enumerate_min_segmentations(raw_input, LITERAL_SEGMENTATION_ENUMERATE_LIMIT) {
            let cut = normalize_umlaut_aliases(cut);
            if cut == segments {
                primary_is_literal = true;
            } else if alternatives.len() < LITERAL_SEGMENTATION_DECODE_LIMIT {
                alternatives.push(cut);
            }
        }
        if !primary_is_literal || alternatives.is_empty() {
            return unchanged(result);
        }

        let mut all_cuts = alternatives.clone();
        all_cuts.push(segments.to_vec());
        if !dictionary.query_exact_paths(&all_cuts, 1).unwrap_or_default().is_empty() || !self.domain.exact(&all_cuts, 1).is_empty() {
            return unchanged(result);
        }

        let Some(primary_score) = self.lattice_best_path_score(segments) else { return unchanged(result) };
        let mut best_score: Option<f64> = None;
        let mut best_cut: Option<&Segments> = None;
        for cut in &alternatives {
            if let Some(score) = self.lattice_best_path_score(cut) {
                if best_score.is_none_or(|best| score > best) {
                    best_score = Some(score);
                    best_cut = Some(cut);
                }
            }
        }
        let (Some(best_score), Some(best_cut)) = (best_score, best_cut) else { return unchanged(result) };

        let primary_key = join_segments(segments);
        let best_key = join_segments(best_cut);
        let alternative_leads = best_score > primary_score && !self.user_prefers_reading(&primary_key);
        let gap = if alternative_leads { best_score - primary_score } else { primary_score - best_score };
        if !alternative_leads {
            if gap > LEGAL_INPUT_RUNNER_UP_GAP_LOG {
                return unchanged(result);
            }
            // 第 2 位只要一句。词格开着时直接用打分时解出的那条最优路径；只开其他来源时词格行不显示，
            // 照常查一遍，取它的整键块首行。
            let runner = if self.association.word_lattice {
                self.lattice_best_path(best_cut).map(|path| {
                    let mut runner = item(&best_key, &path.sentence, (path.log_prob * 1000.0) as i64, Source::Generated, &path.key);
                    runner.sentence_association = true;
                    runner.input_domains = path.input_domains.clone();
                    runner
                })
            } else {
                self.query_series(raw_input, &best_key, best_cut).into_iter().find(|candidate| reads_full_key(candidate, &best_key))
            };
            let Some(runner) = runner else { return unchanged(result) };
            let mut merged = vec![result[0].clone()];
            append_unique_words(&mut merged, &[runner]);
            append_unique_words(&mut merged, &result);
            return (merged, segments.to_vec());
        }

        // 胜出切分的整键块（整句行）领衔，其余照旧，胜出切分的前缀词垫底；整键块为空时由前缀词领衔。
        let (mut winner_full, mut winner_prefix): (Vec<Item>, Vec<Item>) = (Vec::new(), Vec::new());
        for candidate in self.query_series(raw_input, &best_key, best_cut) {
            if reads_full_key(&candidate, &best_key) {
                winner_full.push(candidate);
            } else {
                winner_prefix.push(candidate);
            }
        }
        if winner_full.is_empty() {
            std::mem::swap(&mut winner_full, &mut winner_prefix);
        }
        if winner_full.is_empty() {
            return unchanged(result);
        }
        // 原切分的前缀区间扫描会捞出胜出切分的词，读音却记在原切分上：同一个词两边都有时换成胜出切分那一行，位置不变。
        let mut winner_prefix_by_word: HashMap<&str, &Item> = HashMap::new();
        for candidate in &winner_prefix {
            winner_prefix_by_word.entry(candidate.word.as_str()).or_insert(candidate);
        }
        let mut primary_sentences: Vec<Item> = Vec::new();
        let mut rest: Vec<Item> = Vec::new();
        for candidate in result {
            if reads_full_key(&candidate, &primary_key) {
                primary_sentences.push(candidate);
                continue;
            }
            match winner_prefix_by_word.get(candidate.word.as_str()) {
                Some(same_word) => rest.push((*same_word).clone()),
                None => rest.push(candidate),
            }
        }
        let mut merged = vec![winner_full[0].clone()];
        if gap <= LEGAL_INPUT_RUNNER_UP_GAP_LOG && !primary_sentences.is_empty() {
            append_unique_words(&mut merged, &primary_sentences[..1]);
            append_unique_words(&mut merged, &winner_full);
            append_unique_words(&mut merged, &primary_sentences);
        } else {
            append_unique_words(&mut merged, &winner_full);
        }
        append_unique_words(&mut merged, &rest);
        append_unique_words(&mut merged, &winner_prefix);
        (merged, best_cut.clone())
    }

    /// 候选来自纠错解释，当且仅当它的读音字母等于某条纠错切分（主切或排名靠前的备选）的字母、而这些字母又不同于敲的字母。
    /// 只比字母会把拼对了的前缀候选（keneng → ke、单字母简拼展开）也扫进来，两条规则合起来才不会。ü 的别名改写
    /// （nue → nve、jv → ju）也是标记来源：它不看纠错开关，改写过的查询用敲的拼法给候选打标。
    fn mark_autocorrect_candidates(&self, candidates: &mut [Item], raw_input: &str) {
        let mut corrected_letter_sets: Vec<String> = Vec::with_capacity(1 + self.pinyin_alternative_segmentations.len());
        corrected_letter_sets.push(fold_letters(&self.pinyin_segmentation));
        corrected_letter_sets.extend(self.pinyin_alternative_segmentations.iter().map(|alternative| fold_letters(alternative)));
        corrected_letter_sets.retain(|letters| !letters.is_empty());
        let raw_letters = fold_letters(raw_input);
        if corrected_letter_sets.is_empty() || corrected_letter_sets.iter().all(|letters| *letters == raw_letters) {
            return;
        }
        for candidate in candidates.iter_mut() {
            if !candidate.corrected_from.is_empty() || literal_mixed_match(candidate, raw_input) != 0 {
                continue;
            }
            let item_letters = fold_letters(&candidate.pinyin);
            // 只和真的不同于敲的字母的读法比：某条备选切分折叠后可能恰好等于 raw_letters，不能给拼对了的候选打标。
            if corrected_letter_sets.iter().any(|letters| *letters != raw_letters && item_letters == *letters) {
                candidate.corrected_from = raw_letters.clone();
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    // 造词、调频、删词
    // ------------------------------------------------------------------------------------------

    /// 写回路径（调频、造词、删词、写整句缓存、建 SQL、校验）去掉分隔符后的重切：按最近一次 `query` 的纠错状态切，
    /// 写回才会落在查询出的同一个键上。
    fn cut_for_write(&self, pinyin: &str) -> Vec<Segments> {
        cut_pinyin_by_mode(pinyin, CutMode::Correction, self.correction_aliases_enabled)
    }

    fn build_sql_for_checking_word(&self, key: &str, value: &str) -> String {
        let cuts = self.cut_for_write(key);
        let Some(cut) = cuts.first() else { return String::new() };
        let table = table_name(cut).unwrap_or_default();
        format!("select 1 from {table} where key = '{}' and value = '{}';", escape_sql_text(key), escape_sql_text(value))
    }

    fn build_sql_for_inserting_word(&self, key: &str, jp: &str, value: &str) -> String {
        let cuts = self.cut_for_write(key);
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

    fn build_sql_for_updating_word(&self, pinyin: &str, word: &str) -> String {
        let cuts = self.cut_for_write(&remove_delimiters(pinyin));
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
        self.cut_for_write(&pure_key).first().is_some_and(|cut| cut.len() == han_count)
    }

    fn writable(&self) -> Option<Connection> {
        let connection = Connection::open_with_flags(&self.db_path, OpenFlags::SQLITE_OPEN_READ_WRITE).ok()?;
        connection.busy_timeout(DICTIONARY_BUSY_TIMEOUT).ok()?;
        Some(connection)
    }

    /// 执行一条语句；空串和 C++ 里对空指针语句的 `sqlite3_step` 一样算失败。
    fn execute_one(&self, sql: &str) -> i32 {
        if sql.is_empty() {
            return ERROR_CODE;
        }
        self.note_external_changes();
        let Some(connection) = self.writable() else { return ERROR_CODE };
        let written = connection.execute(sql, []).is_ok();
        drop(connection);
        self.absorb_own_write();
        if !written {
            return ERROR_CODE;
        }
        OK
    }

    fn row_exists(&self, sql: &str) -> bool {
        let Some(dictionary) = &self.dictionary else { return false };
        let Ok(mut statement) = dictionary.connection().prepare(sql) else { return false };
        statement.exists([]).unwrap_or(false)
    }

    fn insert_new_word(&mut self, pinyin: &str, jp: &str, word: &str) -> i32 {
        if self.row_exists(&self.build_sql_for_checking_word(pinyin, word)) {
            return OK;
        }
        if self.execute_one(&self.build_sql_for_inserting_word(pinyin, jp, word)) != OK {
            return ERROR_CODE;
        }
        let _ = user_dictionary::record_user_insert(&self.journal_path, DictionaryKind::Pinyin, pinyin, word, 10000, "");
        self.reset_cache();
        OK
    }

    /// 把云/AI 候选插进这个读音对应的缓存列表（读音按纠错切分重新切）。
    pub fn insert_word_to_series_cache(&self, pinyin: &str, word: &str, source: Source) -> i32 {
        if pinyin.is_empty() || word.is_empty() {
            return ERROR_CODE;
        }
        let cuts = self.cut_for_write(pinyin);
        let segmentation = cuts.first().map_or_else(|| pinyin.to_string(), |cut| join_segments(cut));
        self.insert_word_to_series_cache_key(&series_cache_key(pinyin, &segmentation), pinyin, word, source)
    }

    /// 同上，但键与这次请求的查询一致（纠错、合法输入上的纠错读法各占一个槽）。
    pub fn insert_word_to_series_cache_for_request(&self, raw_input: &str, segmentation: &str, autocorrect_types: u32, word: &str, source: Source) -> i32 {
        if raw_input.is_empty() || word.is_empty() {
            return ERROR_CODE;
        }
        let segments = self.resolve_segments(raw_input, segmentation, correction_aliases_enabled(autocorrect_types));
        let resolution = resolve_series_query(raw_input, &segments, autocorrect_types);
        self.insert_word_to_series_cache_key(&resolution.cache_key, raw_input, word, source)
    }

    fn insert_word_to_series_cache_key(&self, cache_key: &str, pinyin: &str, word: &str, source: Source) -> i32 {
        let mut cache = self.series_cache.borrow_mut();
        let mut list = cache.get(cache_key).unwrap_or_default();
        crate::cache::insert_dynamic_candidate(&mut list, pinyin, word, source, true);
        cache.insert(cache_key, list);
        OK
    }

    pub fn create_word(&mut self, pinyin: &str, word: &str) -> i32 {
        let pinyin = remove_delimiters(pinyin);
        let cuts = self.cut_for_write(&pinyin);
        let Some(cut) = cuts.first() else { return ERROR_CODE };
        let pinyin = join_segments(cut);
        let jp = segments_to_jianpin(cut);
        if !self.do_validate(&pinyin, &jp, word) {
            return ERROR_CODE;
        }
        self.insert_new_word(&pinyin, &jp, word)
    }

    /// 音节边界由调用方（canonical 键）显式给定，只校验段数与逐段完整性，不再重切。
    pub fn create_word_from_canonical_pinyin(&mut self, pinyin: &str, word: &str) -> i32 {
        let segments = split_segments(pinyin);
        let han_count = helpcode::count_han_chars(word);
        if segments.is_empty() || segments.len() != han_count || segments.iter().any(|segment| segment.is_empty() || !is_complete_pinyin_input(segment)) {
            return ERROR_CODE;
        }
        let pinyin = join_segments(&segments);
        let jp = segments_to_jianpin(&segments);
        self.insert_new_word(&pinyin, &jp, word)
    }

    /// 按最近一次 `query` 的主切分调频。
    pub fn update_weight_by_word(&mut self, word: &str) -> i32 {
        let pinyin = remove_delimiters(&self.pinyin_segmentation);
        self.update_weight_by_pinyin_and_word(&pinyin, word)
    }

    /// 把词的权重提到这个读音下的最高 + 1。
    pub fn update_weight_by_pinyin_and_word(&mut self, pinyin: &str, word: &str) -> i32 {
        let pinyin = remove_delimiters(pinyin);
        let cuts = self.cut_for_write(&pinyin);
        let Some(cut) = cuts.first() else { return ERROR_CODE };
        // 别名拼写（nue/lue 等词库不存在的键）不得承载用户调频数据：改写为标准键。
        let mut segments = normalize_umlaut_aliases(cut.clone());
        let han_count = helpcode::count_han_chars(word);
        if segments.len() > han_count {
            segments.truncate(han_count);
        }
        let normalized = join_segments(&segments);
        if self.execute_one(&self.build_sql_for_updating_word(&normalized, word)) != OK {
            return ERROR_CODE;
        }
        let _ = user_dictionary::record_pinyin_upsert_from_database(&self.db_path.to_string_lossy(), &normalized, word, &self.journal_path);
        self.reset_cache();
        OK
    }

    pub fn delete_by_pinyin_and_word(&mut self, pinyin: &str, word: &str) -> i32 {
        let pinyin = remove_delimiters(pinyin);
        let cuts = self.cut_for_write(&pinyin);
        let Some(cut) = cuts.first() else { return ERROR_CODE };
        let normalized = join_segments(cut);
        if !user_dictionary::delete_dictionary_candidate(&self.db_path.to_string_lossy(), &self.journal_path, DictionaryKind::Pinyin, &normalized, word) {
            return ERROR_CODE;
        }
        self.reset_cache();
        OK
    }
}
