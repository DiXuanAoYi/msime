//! 整句词格：在词库给出的“哪些词能盖住哪段音节”上做 Viterbi 束搜索，挑出 n-best 整句，
//! 再按来源规则并进已有的候选列表。
//!
//! 移植自 `engine/quanpin/word_lattice.{h,cpp}`。图（哪些词盖哪段音节）由调用方通过 `lookup` 提供；
//! 路径分来自三个可叠加的部分：
//! - 语言模型（C++ 里是 kenlm 三元模型）：每条边按 `log10 P(词 | 前两个词)` 打分，每个假设带着模型状态；
//!   词库权重只留一个很小的平局项，读音先验替模型补上「这个读音对不对」；
//! - 没有可用模型时退回启发式的字频打分；
//! - 字级搭配回调（octagram）和句中辅助码的字约束。
//!
//! **这里没有 kenlm 的读取器**：[`LanguageModel`] 是 trait，调用方提供实现。黄金样本里的 C++ 也不带模型
//! （`LanguageModel` 在 C++ 里不能被替换），所以带模型的那条分支靠单元测试里的玩具模型验证结构，
//! 没有和真实 `sc.lm` 的打分对过。

use std::collections::{HashMap, HashSet};

/// 词库给出的一个词条。
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Lexeme {
    /// 词条的拼音键；空表示用跨度的键。模糊音的行键和跨度不同。
    pub key: String,
    pub value: String,
    pub weight: i64,
    /// 从边分里扣掉的分，让模糊读音的行让位于敲出的读音。
    pub penalty: f64,
    pub input_domains: Vec<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Path {
    pub sentence: String,
    pub key: String,
    pub log_prob: f64,
    pub words: Vec<String>,
    pub input_domains: Vec<String>,
}

/// 句中辅助码：第 `syllable` 个音节只能解成 `accept` 认可的汉字。
pub struct CharConstraint<'a> {
    pub syllable: usize,
    pub accept: &'a dyn Fn(&str) -> bool,
}

pub type Lookup<'a> = &'a dyn Fn(&[String]) -> Vec<Lexeme>;
pub type CollocationScorer<'a> = &'a dyn Fn(&str, &str, bool) -> f64;

/// 打分模型的状态：最近几个词的下标，由实现自己解释。
pub type ModelState = Vec<u32>;

/// 词格用的语言模型。分数是 log10 概率；未登录词的罚分由实现负责。
pub trait LanguageModel {
    fn is_valid(&self) -> bool;
    /// 无上下文的起点。不用句首状态：出货的 sc.lm 不带 `<s>`，句首上下文只会带进一个没有任何 n-gram 的词。
    fn null_state(&self) -> ModelState;
    fn index(&self, word: &str) -> u32;
    fn score(&self, state: &ModelState, index: u32) -> (f32, ModelState);
}

pub struct Options<'a> {
    pub beam: i32,
    pub nbest: i32,
    pub include_lattice_best: bool,
    pub include_collocation_best: bool,
    pub show_next_on_duplicate: bool,
    pub span_limit: i32,
    pub max_phrase_syllables: i32,
    pub language_model: Option<&'a dyn LanguageModel>,
    pub dictionary_tiebreak: f64,
    pub reading_prior: f64,
    pub reading_prior_share: f64,
    pub reading_prior_floor: f64,
    pub unigram_z: f64,
    pub phrase_length_bonus: f64,
    pub char_constraints: Vec<CharConstraint<'a>>,
    pub constrained_lookup: Option<Lookup<'a>>,
    pub collocation_scorer: Option<CollocationScorer<'a>>,
    pub collocation_weight: f64,
    pub precomputed_paths: Vec<Path>,
}

impl Default for Options<'_> {
    fn default() -> Self {
        Self {
            beam: 32,
            nbest: 5,
            include_lattice_best: true,
            include_collocation_best: false,
            show_next_on_duplicate: false,
            span_limit: 32,
            max_phrase_syllables: 7,
            language_model: None,
            dictionary_tiebreak: 0.01,
            reading_prior: 1.0,
            reading_prior_share: 0.01,
            reading_prior_floor: 5.0,
            unigram_z: 1e6,
            phrase_length_bonus: 3.0,
            char_constraints: Vec::new(),
            constrained_lookup: None,
            collocation_scorer: None,
            collocation_weight: 0.0,
            precomputed_paths: Vec::new(),
        }
    }
}

/// 搭配打分的上下文窗口：.gram 的查询键最多编入 8 个码点。
const COLLOCATION_TAIL_CODEPOINTS: usize = 8;

fn join_span(span: &[String]) -> String {
    span.join("'")
}

/// 无模型时的字频启发式（自然对数）。单字行的权重是语料计数，多音节行是量级小得多的短语权重，
/// 所以单字按 `unigram_z` 压下去，短语按音节数加分。从来没有标定过。
fn heuristic_log_prob(weight: i64, syllables: usize, options: &Options) -> f64 {
    let w = if weight > 0 { weight as f64 } else { 1.0 };
    let z = if options.unigram_z > 1.0 { options.unigram_z } else { 1e6 };
    let lp = w.ln();
    if syllables <= 1 {
        return lp - z.ln();
    }
    lp + options.phrase_length_bonus * syllables as f64
}

/// 词库权重缩成的平局项（log10）。
fn dictionary_tiebreak(weight: i64, options: &Options) -> f64 {
    let w = if weight > 0 { weight as f64 } else { 1.0 };
    options.dictionary_tiebreak * w.log10()
}

/// log10 P(词|拼音)，用跨度内同读音的权重占比近似；占比高于免罚线不扣分。
fn reading_prior(weight: i64, span_total: i64, options: &Options) -> f64 {
    if options.reading_prior <= 0.0 || span_total <= 0 {
        return 0.0;
    }
    let w = if weight > 0 { weight as f64 } else { 0.0 };
    let share = (w + 1.0) / (span_total as f64 + 1.0);
    let threshold = if options.reading_prior_share > 0.0 { options.reading_prior_share } else { 1.0 };
    if share >= threshold {
        return 0.0;
    }
    let floor = if options.reading_prior_floor > 0.0 { -options.reading_prior_floor } else { f64::NEG_INFINITY };
    options.reading_prior * (share / threshold).log10().max(floor)
}

struct Edge {
    end: usize,
    word: String,
    key: String,
    input_domains: Vec<String>,
    base_score: f64,
    index: u32,
}

struct Hyp {
    score: f64,
    prev: Option<(usize, usize)>,
    word: String,
    key: String,
    input_domains: Vec<String>,
    state: ModelState,
    collocation_tail: String,
}

fn active_model<'a>(options: &Options<'a>) -> Option<&'a dyn LanguageModel> {
    options.language_model.filter(|model| model.is_valid())
}

fn char_count(text: &str) -> usize {
    text.chars().count()
}

/// 词的字数必须等于跨度的音节数，被钉住的字都要被各自的约束认可。
fn lexeme_satisfies(value: &str, syllables: usize, pins: &[(usize, &CharConstraint)]) -> bool {
    let chars: Vec<String> = value.chars().map(String::from).collect();
    chars.len() == syllables && pins.iter().all(|(index, constraint)| (constraint.accept)(&chars[*index]))
}

fn build_graph(syllables: &[String], lookup: Lookup, options: &Options) -> Vec<Vec<Edge>> {
    let model = active_model(options);
    let n = syllables.len();
    let mut graph: Vec<Vec<Edge>> = (0..n).map(|_| Vec::new()).collect();
    let max_len = options.max_phrase_syllables.max(1) as usize;
    let mut span_cache: HashMap<String, Vec<Lexeme>> = HashMap::new();
    for start in 0..n {
        let max_end = n.min(start + max_len);
        for end in start + 1..=max_end {
            let span = &syllables[start..end];
            let span_key = join_span(span);
            let pins: Vec<(usize, &CharConstraint)> = options
                .char_constraints
                .iter()
                .filter(|c| c.syllable >= start && c.syllable < end)
                .map(|c| (c.syllable - start, c))
                .collect();
            let constrained_rows: Vec<Lexeme>;
            let rows: &Vec<Lexeme> = if pins.is_empty() {
                span_cache.entry(span_key.clone()).or_insert_with(|| lookup(span))
            } else {
                // 受约束的跨度按位置筛，结果与起点有关，不进按拼音共享的缓存。
                let source = options.constrained_lookup.unwrap_or(lookup);
                constrained_rows = source(span).into_iter().filter(|row| lexeme_satisfies(&row.value, span.len(), &pins)).collect();
                &constrained_rows
            };
            let take = rows.len().min(options.span_limit.max(0) as usize);
            // 先验的分母：跨度内实际参与解码、读音相同的那些行。
            let mut reading_totals: HashMap<&str, i64> = HashMap::new();
            for row in &rows[..take] {
                *reading_totals.entry(row.key.as_str()).or_insert(0) += row.weight.max(0);
            }
            for row in &rows[..take] {
                if row.value.is_empty() {
                    continue;
                }
                let mut base_score;
                let mut index = 0;
                if let Some(model) = model {
                    index = model.index(&row.value);
                    base_score = dictionary_tiebreak(row.weight, options)
                        + reading_prior(row.weight, *reading_totals.get(row.key.as_str()).unwrap_or(&0), options);
                } else {
                    base_score = heuristic_log_prob(row.weight, end - start, options);
                }
                base_score -= row.penalty;
                graph[start].push(Edge {
                    end,
                    word: row.value.clone(),
                    key: if row.key.is_empty() { span_key.clone() } else { row.key.clone() },
                    input_domains: row.input_domains.clone(),
                    base_score,
                    index,
                });
            }
        }
    }
    graph
}

/// 保留字符串末尾至多 `n` 个码点。
fn tail_codepoints(text: &str, n: usize) -> String {
    let total = char_count(text);
    if total <= n {
        return text.to_string();
    }
    text.chars().skip(total - n).collect()
}

fn keep_beam(column: &mut Vec<Hyp>, beam: i32) {
    let beam = beam.max(0) as usize;
    if column.len() <= beam {
        return;
    }
    // C++ 用 partial_sort，平局的次序是实现定义的；这里用稳定排序。
    column.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
    column.truncate(beam);
}

/// 在音节串上解出 n-best 整句。
pub fn decode_word_lattice(syllables: &[String], lookup: Lookup, options: &Options) -> Vec<Path> {
    if syllables.is_empty() {
        return Vec::new();
    }
    let n = syllables.len();
    let model = active_model(options);
    let graph = build_graph(syllables, lookup, options);
    let mut columns: Vec<Vec<Hyp>> = (0..=n).map(|_| Vec::new()).collect();
    columns[0].push(Hyp {
        score: 0.0,
        prev: None,
        word: String::new(),
        key: String::new(),
        input_domains: Vec::new(),
        state: model.map(|m| m.null_state()).unwrap_or_default(),
        collocation_tail: String::new(),
    });

    for pos in 0..n {
        keep_beam(&mut columns[pos], options.beam);
        if columns[pos].is_empty() {
            continue;
        }
        for hi in 0..columns[pos].len() {
            for edge in &graph[pos] {
                let hyp = &columns[pos][hi];
                let mut score = hyp.score + edge.base_score;
                let mut state = ModelState::new();
                if let Some(model) = model {
                    let (model_score, next_state) = model.score(&hyp.state, edge.index);
                    score += f64::from(model_score);
                    state = next_state;
                }
                let mut collocation_tail = String::new();
                if let (Some(scorer), true) = (options.collocation_scorer, options.collocation_weight > 0.0) {
                    // 句尾词把 is_rear 置真：.gram 的「词+$」rear 项据此参与打分。
                    score += options.collocation_weight * scorer(&hyp.collocation_tail, &edge.word, edge.end == n);
                    collocation_tail = tail_codepoints(&format!("{}{}", hyp.collocation_tail, edge.word), COLLOCATION_TAIL_CODEPOINTS);
                }
                let next = Hyp {
                    score,
                    prev: Some((pos, hi)),
                    word: edge.word.clone(),
                    key: edge.key.clone(),
                    input_domains: edge.input_domains.clone(),
                    state,
                    collocation_tail,
                };
                columns[edge.end].push(next);
            }
        }
    }

    if columns[n].is_empty() {
        return Vec::new();
    }
    columns[n].sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
    let take = options.nbest.max(0).min(columns[n].len() as i32) as usize;

    let mut paths: Vec<Path> = Vec::with_capacity(take);
    let mut seen: HashSet<String> = HashSet::new();
    for i in 0..columns[n].len() {
        if paths.len() >= take {
            break;
        }
        let mut path = Path { sentence: String::new(), key: String::new(), log_prob: columns[n][i].score, words: Vec::new(), input_domains: Vec::new() };
        let mut keys: Vec<&str> = Vec::new();
        let mut cursor = Some((n, i));
        while let Some((pos, idx)) = cursor {
            if pos == 0 {
                break;
            }
            let hyp = &columns[pos][idx];
            path.words.push(hyp.word.clone());
            for id in &hyp.input_domains {
                if !path.input_domains.contains(id) {
                    path.input_domains.push(id.clone());
                }
            }
            keys.push(&hyp.key);
            cursor = hyp.prev;
        }
        path.words.reverse();
        keys.reverse();
        path.sentence = path.words.concat();
        path.key = keys.join("'");
        if path.sentence.is_empty() || !seen.insert(path.sentence.clone()) {
            continue;
        }
        paths.push(path);
    }
    paths
}

// -------------------------------------------------------------------------------------------------
// 并进候选列表
// -------------------------------------------------------------------------------------------------

/// 候选的来源，和 `engine/core/word_item.h` 的 `CandidateSource` 一一对应。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    Database,
    UserDatabase,
    CloudSuggestion,
    AiSuggestion,
    EnglishDictionary,
    QuickPhrase,
    Emoji,
    Kaomoji,
    Generated,
    Fallback,
    NeuralDesktop,
    NeuralKeyboard,
    DateTime,
    Collocation,
    AppDictionary,
    AiCompletion,
    AiQuestion,
}

/// 候选列表里的一项（只带词格合并要读写的那几个字段）。
#[derive(Debug, Clone, PartialEq)]
pub struct Item {
    pub pinyin: String,
    pub canonical_pinyin: String,
    pub word: String,
    pub weight: i64,
    pub source: Source,
    pub sentence_association: bool,
    pub input_domains: Vec<String>,
    /// 模糊音读出来的候选：敲出的读音与词库里的读音可能不同。
    pub fuzzy: bool,
    /// 非空表示候选来自纠错解释，值是纠错前敲的字母。
    pub corrected_from: String,
    /// 产出这个候选的输入方案：五笔拼音混输的组合里两边的候选共存，调频、删除按候选自己的方案走。
    pub scheme: crate::request::SchemeType,
    pub fixed_position: i32,
}

/// 摆固定位置、排快捷短语要读写的几个字段；会话候选、本地模式候选、调频候选都实现它。
pub trait Positioned {
    fn word(&self) -> &str;
    fn source(&self) -> Source;
    fn fixed_position(&self) -> i32;
    fn set_fixed_position(&mut self, position: i32);
}

impl Positioned for Item {
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

/// 对 n-best 的第二意见：只能重排，不许增删改。返回 false 表示弃权。
pub struct SourcedReranker<'a> {
    pub rerank: &'a dyn Fn(&mut Vec<Path>) -> bool,
    pub source: Source,
}

fn is_exact_full_key_hit(item: &Item, typed_key: &str, syllable_count: usize) -> bool {
    if item.source != Source::Database && item.source != Source::UserDatabase {
        return false;
    }
    if !item.canonical_pinyin.is_empty() {
        return item.canonical_pinyin == typed_key;
    }
    // 没带 canonical key 的行无从比对，退回字数判断。
    char_count(&item.word) == syllable_count
}

/// 整句候选该插的位置：紧跟在开头那串“键与输入完全相等”的词库命中之后。
/// 音节数相等不够：前缀区间扫描和模糊音展开出来的行键与输入不同，不该挡住整句。
pub fn generated_sentence_insert_position(candidates: &[Item], syllables: &[String]) -> usize {
    if syllables.is_empty() {
        return 0;
    }
    let typed_key = join_span(syllables);
    candidates.iter().take_while(|item| is_exact_full_key_hit(item, &typed_key, syllables.len())).count()
}

fn has_only_complete_pinyin_segments(syllables: &[String]) -> bool {
    !syllables.is_empty() && syllables.iter().all(|s| crate::syllables::is_intact(s))
}

fn prefers(ranked: &Option<Vec<Path>>, sentence: &str) -> bool {
    ranked.as_ref().is_some_and(|paths| paths.first().is_some_and(|p| p.sentence == sentence))
}

/// 在 `ranked` 里找第一个没出现过的整句；出现过时，开了 `show_next_on_duplicate` 就继续往下找，否则放弃。
fn first_distinct<'p>(ranked: &'p [Path], already: &HashSet<String>, show_next_on_duplicate: bool) -> Option<&'p Path> {
    for path in ranked {
        if already.contains(&path.sentence) {
            if show_next_on_duplicate {
                continue;
            }
            break;
        }
        return Some(path);
    }
    None
}

#[allow(clippy::too_many_arguments)]
fn select_distinct(
    ranked: &[Path],
    source: Source,
    typed: &str,
    already: &mut HashSet<String>,
    show_next_on_duplicate: bool,
) -> Option<Item> {
    let path = first_distinct(ranked, already, show_next_on_duplicate)?;
    already.insert(path.sentence.clone());
    // 对数域的分，常为负。列表里的次序才是排名，不要按这个权重排序。
    Some(Item {
        pinyin: typed.to_string(),
        canonical_pinyin: path.key.clone(),
        word: path.sentence.clone(),
        weight: (path.log_prob * 1000.0) as i64,
        source,
        sentence_association: true,
        input_domains: path.input_domains.clone(),
        fuzzy: false,
        corrected_from: String::new(),
        scheme: Default::default(),
        fixed_position: 0,
    })
}

/// 把词格解出的整句并进候选列表。至多插进四行：各重排器的首选、词格自己的首选（`Generated`，
/// 开了搭配时是 `Collocation`），指向同一句的只留一行；n-best 的其余部分只给重排器挑选。
pub fn merge_lattice_candidates(
    candidates: &mut Vec<Item>,
    syllables: &[String],
    lookup: Lookup,
    typed_pinyin: &str,
    options: &Options,
    rerankers: &[SourcedReranker],
) {
    if syllables.len() < 2 || !has_only_complete_pinyin_segments(syllables) {
        return;
    }
    let paths = if options.precomputed_paths.is_empty() {
        decode_word_lattice(syllables, lookup, options)
    } else {
        options.precomputed_paths.clone()
    };
    if paths.is_empty() {
        return;
    }

    let mut desktop_paths: Option<Vec<Path>> = None;
    let mut keyboard_paths: Option<Vec<Path>> = None;
    let mut other_ranked: Vec<(Vec<Path>, Source)> = Vec::new();
    let mut keyboard_enabled = false;
    for entry in rerankers {
        if entry.source == Source::NeuralKeyboard {
            keyboard_enabled = true;
        }
        // 每个重排器都从相同的静态顺序开始。
        let mut reranked = paths.clone();
        if !(entry.rerank)(&mut reranked) {
            continue;
        }
        match entry.source {
            Source::NeuralDesktop => desktop_paths = Some(reranked),
            Source::NeuralKeyboard => keyboard_paths = Some(reranked),
            other => other_ranked.push((reranked, other)),
        }
    }

    let unigram_position = candidates.iter().position(|item| item.source == Source::Fallback);
    let unigram_consensus = unigram_position.is_some_and(|at| {
        let word = &candidates[at].word;
        (options.include_lattice_best && &paths[0].sentence == word) || prefers(&keyboard_paths, word) || prefers(&desktop_paths, word)
    });

    let mut already: HashSet<String> = candidates.iter().map(|item| item.word.clone()).collect();
    let next_on_duplicate = options.show_next_on_duplicate;

    let mut lattice_item: Option<Item> = None;
    let (mut keyboard_item, mut desktop_item): (Option<Item>, Option<Item>) = (None, None);
    if options.include_collocation_best {
        lattice_item = select_distinct(&paths, Source::Collocation, typed_pinyin, &mut already, next_on_duplicate);
    }
    if lattice_item.is_none() && options.include_lattice_best {
        lattice_item = select_distinct(&paths, Source::Generated, typed_pinyin, &mut already, next_on_duplicate);
    }
    let lattice_consensus = lattice_item.as_ref().is_some_and(|item| {
        item.word == paths[0].sentence && (prefers(&keyboard_paths, &paths[0].sentence) || prefers(&desktop_paths, &paths[0].sentence))
    });

    let mut keyboard_before_desktop = false;
    if let (Some(keyboard), Some(desktop)) = (&keyboard_paths, &desktop_paths) {
        // 各自的首选在互相排除之前比较：同一句归 keyboard，否则 desktop 先显示。
        let keyboard_best = first_distinct(keyboard, &already, next_on_duplicate).map(|p| p.sentence.clone());
        let desktop_best = first_distinct(desktop, &already, next_on_duplicate).map(|p| p.sentence.clone());
        keyboard_before_desktop = keyboard_best.is_some() && keyboard_best == desktop_best;
        if keyboard_before_desktop {
            keyboard_item = select_distinct(keyboard, Source::NeuralKeyboard, typed_pinyin, &mut already, next_on_duplicate);
            desktop_item = select_distinct(desktop, Source::NeuralDesktop, typed_pinyin, &mut already, next_on_duplicate);
        } else {
            desktop_item = select_distinct(desktop, Source::NeuralDesktop, typed_pinyin, &mut already, next_on_duplicate);
            keyboard_item = select_distinct(keyboard, Source::NeuralKeyboard, typed_pinyin, &mut already, next_on_duplicate);
        }
    } else if let Some(keyboard) = &keyboard_paths {
        keyboard_item = select_distinct(keyboard, Source::NeuralKeyboard, typed_pinyin, &mut already, next_on_duplicate);
    } else if let (Some(desktop), false) = (&desktop_paths, keyboard_enabled) {
        // 两种神经模式都开时 desktop 等 keyboard 先出，免得它先占了 keyboard 的首选又在下一次查询里变掉。
        desktop_item = select_distinct(desktop, Source::NeuralDesktop, typed_pinyin, &mut already, next_on_duplicate);
    }

    let mut other_items: Vec<Item> = Vec::new();
    for (ranked, source) in &other_ranked {
        if let Some(item) = select_distinct(ranked, *source, typed_pinyin, &mut already, next_on_duplicate) {
            other_items.push(item);
        }
    }

    let promoted_unigram = if unigram_consensus { unigram_position.map(|at| candidates.remove(at)) } else { None };

    // 其他来源也认同时先升 Unigram，再把词格行排在神经行前面。
    let mut extra: Vec<Item> = Vec::new();
    extra.extend(promoted_unigram);
    if lattice_consensus {
        extra.extend(lattice_item.take());
    }
    if keyboard_before_desktop {
        extra.extend(keyboard_item);
        extra.extend(desktop_item);
    } else {
        extra.extend(desktop_item);
        extra.extend(keyboard_item);
    }
    extra.extend(other_items);
    if !lattice_consensus {
        extra.extend(lattice_item);
    }
    if extra.is_empty() {
        return;
    }
    let insert_at = generated_sentence_insert_position(candidates, syllables);
    candidates.splice(insert_at..insert_at, extra);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(items: &[&str]) -> Vec<String> {
        items.iter().map(|x| x.to_string()).collect()
    }

    fn lexeme(value: &str, weight: i64) -> Lexeme {
        Lexeme { value: value.into(), weight, ..Default::default() }
    }

    #[test]
    fn the_best_sentence_wins_and_n_best_has_no_duplicate_sentences() {
        let lookup = |span: &[String]| -> Vec<Lexeme> {
            match join_span(span).as_str() {
                "ni" => vec![lexeme("你", 5000), lexeme("泥", 100)],
                "hao" => vec![lexeme("好", 5000), lexeme("号", 100)],
                "ni'hao" => vec![lexeme("你好", 200)],
                _ => vec![],
            }
        };
        let paths = decode_word_lattice(&s(&["ni", "hao"]), &lookup, &Options::default());
        assert!(!paths.is_empty());
        let sentences: Vec<&str> = paths.iter().map(|p| p.sentence.as_str()).collect();
        let unique: HashSet<&&str> = sentences.iter().collect();
        assert_eq!(unique.len(), sentences.len(), "{sentences:?}");
        assert_eq!(paths[0].key, paths[0].key.to_lowercase());
    }

    #[test]
    fn an_unreachable_end_gives_no_sentence() {
        // 第二个音节没有任何词，整句走不通。
        let lookup = |span: &[String]| -> Vec<Lexeme> {
            if span == ["ni"] { vec![lexeme("你", 10)] } else { vec![] }
        };
        assert!(decode_word_lattice(&s(&["ni", "hao"]), &lookup, &Options::default()).is_empty());
        assert!(decode_word_lattice(&[], &lookup, &Options::default()).is_empty());
    }

    #[test]
    fn a_char_constraint_removes_every_edge_that_breaks_it() {
        let lookup = |span: &[String]| -> Vec<Lexeme> {
            match join_span(span).as_str() {
                "ni" => vec![lexeme("你", 5000), lexeme("泥", 4000)],
                "hao" => vec![lexeme("好", 5000)],
                _ => vec![],
            }
        };
        let only_ni = |c: &str| c == "泥";
        let options = Options { char_constraints: vec![CharConstraint { syllable: 0, accept: &only_ni }], ..Default::default() };
        let paths = decode_word_lattice(&s(&["ni", "hao"]), &lookup, &options);
        assert_eq!(paths.iter().map(|p| p.sentence.as_str()).collect::<Vec<_>>(), ["泥好"]);
        let never = |_: &str| false;
        let options = Options { char_constraints: vec![CharConstraint { syllable: 0, accept: &never }], ..Default::default() };
        assert!(decode_word_lattice(&s(&["ni", "hao"]), &lookup, &options).is_empty(), "一条都走不通就没有整句，不退回无约束的结果");
    }

    /// 玩具三元模型：只认一个“你好”搭配，其余按词长给分。
    struct Toy;
    impl LanguageModel for Toy {
        fn is_valid(&self) -> bool {
            true
        }
        fn null_state(&self) -> ModelState {
            Vec::new()
        }
        fn index(&self, word: &str) -> u32 {
            word.chars().next().map(|c| c as u32).unwrap_or(0)
        }
        fn score(&self, state: &ModelState, index: u32) -> (f32, ModelState) {
            let bonus = if state.last() == Some(&('你' as u32)) && index == '好' as u32 { 5.0 } else { 0.0 };
            let mut next = state.clone();
            next.push(index);
            (-3.0 + bonus, next)
        }
    }

    #[test]
    fn a_language_model_can_overturn_the_dictionary_weights() {
        let lookup = |span: &[String]| -> Vec<Lexeme> {
            match join_span(span).as_str() {
                "ni" => vec![lexeme("你", 500), lexeme("泥", 5_000_000)],
                "hao" => vec![lexeme("好", 500), lexeme("号", 5_000_000)],
                _ => vec![],
            }
        };
        let without = decode_word_lattice(&s(&["ni", "hao"]), &lookup, &Options::default());
        assert_eq!(without[0].sentence, "泥号", "没有模型时按字频");
        let with = decode_word_lattice(&s(&["ni", "hao"]), &lookup, &Options { language_model: Some(&Toy), ..Default::default() });
        assert_eq!(with[0].sentence, "你好", "模型认得的搭配压过字频：词库权重只是一个很小的平局项");
    }

    #[test]
    fn a_rare_reading_is_penalised_by_the_prior_only_with_a_model() {
        let rows = vec![lexeme("甲", 1_000_000), lexeme("乙", 0)];
        let totals_weight: i64 = rows.iter().map(|r| r.weight.max(0)).sum();
        let options = Options::default();
        assert_eq!(reading_prior(1_000_000, totals_weight, &options), 0.0, "常见读音不扣");
        let penalty = reading_prior(0, totals_weight, &options);
        assert!(penalty < 0.0 && penalty >= -options.reading_prior_floor, "{penalty}");
        let off = Options { reading_prior: 0.0, ..Default::default() };
        assert_eq!(reading_prior(0, totals_weight, &off), 0.0);
    }

    fn item(word: &str, source: Source, canonical: &str) -> Item {
        Item {
            pinyin: "nihao".into(),
            canonical_pinyin: canonical.into(),
            word: word.into(),
            weight: 1,
            source,
            sentence_association: false,
            input_domains: vec![],
            fuzzy: false,
            corrected_from: String::new(),
            scheme: Default::default(),
            fixed_position: 0,
        }
    }

    #[test]
    fn the_sentence_goes_after_the_exact_key_hits_and_not_after_prefix_rows() {
        let list = vec![
            item("你好", Source::Database, "ni'hao"),
            item("拟好", Source::UserDatabase, "ni'hao"),
            item("滚球", Source::Database, "ni'haox"),
            item("别的", Source::Database, "ni'hao"),
        ];
        assert_eq!(generated_sentence_insert_position(&list, &s(&["ni", "hao"])), 2);
        assert_eq!(generated_sentence_insert_position(&[], &s(&["ni"])), 0);
        assert_eq!(generated_sentence_insert_position(&list, &[]), 0);
    }

    #[test]
    fn merging_needs_two_complete_syllables() {
        let lookup = |_: &[String]| vec![lexeme("字", 10)];
        let mut list = vec![item("你", Source::Database, "ni")];
        merge_lattice_candidates(&mut list, &s(&["ni"]), &lookup, "ni", &Options::default(), &[]);
        assert_eq!(list.len(), 1);
        merge_lattice_candidates(&mut list, &s(&["ni", "zh"]), &lookup, "nizh", &Options::default(), &[]);
        assert_eq!(list.len(), 1, "不完整的音节不合并");
    }

    #[test]
    fn a_reranker_that_abstains_leaves_the_lattice_order() {
        let lookup = |span: &[String]| -> Vec<Lexeme> {
            match join_span(span).as_str() {
                "ni" => vec![lexeme("你", 5000)],
                "hao" => vec![lexeme("好", 5000)],
                _ => vec![],
            }
        };
        let decline = |_: &mut Vec<Path>| false;
        let rerankers = [SourcedReranker { rerank: &decline, source: Source::NeuralDesktop }];
        let mut list: Vec<Item> = Vec::new();
        merge_lattice_candidates(&mut list, &s(&["ni", "hao"]), &lookup, "nihao", &Options::default(), &rerankers);
        assert_eq!(list.len(), 1);
        assert_eq!((list[0].word.as_str(), list[0].source, list[0].sentence_association), ("你好", Source::Generated, true));
        assert_eq!(list[0].canonical_pinyin, "ni'hao");
    }
}
