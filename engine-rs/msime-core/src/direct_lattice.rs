//! 直接辅助码的整句解码，移植自 `engine/direct_helpcode/direct_lattice.{h,cpp}`。
//!
//! 在按键位置上的拼写图里铺词，一次 Viterbi 同时决定切分与用字。和 [`crate::lattice::decode_word_lattice`]
//! 用同一套打分（语言模型、词库权重平局、生僻读音先验、搭配项），区别只在图：那边的节点是固定切分的音节序号，
//! 这里是输入串的按键位置，同一段按键的不同切法各自成边，由整句分数裁决。
//!
//! 束截断在 C++ 里是 `partial_sort`，平局次序由实现决定；这里照 MSVC 的实现排（[`crate::msvc_sort`]）。

use crate::lattice::{CollocationScorer, LanguageModel, Lexeme, ModelState};
use crate::spelling_graph::{next_spelling_position, SpellingGraph, SpellingKind, SyllableSpelling};
use std::collections::HashMap;

/// 搭配打分的上下文窗口：.gram 的查询键最多编入 8 个码点。
const COLLOCATION_TAIL_CODEPOINTS: usize = 8;

/// 按全拼跨度查词。第二个参数为真时跨度里有辅码约束，查询不能在 SQL 里先截断，由解码器筛完约束再截。
pub type SpanLookup<'a> = &'a dyn Fn(&[String], bool) -> Vec<Lexeme>;
/// 这个汉字（UTF-8 单字）的辅码满不满足 first/second（second 为 0 只看第一码）。
pub type CharAccept<'a> = &'a dyn Fn(&str, u8, u8) -> bool;

pub struct DecodeOptions<'a> {
    pub beam: i32,
    pub span_limit: i32,
    pub max_phrase_syllables: i32,
    /// 每个起点最多展开多少条拼写序列，防止长串上组合爆炸。
    pub max_sequences_per_start: i32,
    pub language_model: Option<&'a dyn LanguageModel>,
    pub dictionary_tiebreak: f64,
    pub reading_prior: f64,
    pub reading_prior_share: f64,
    pub reading_prior_floor: f64,
    pub unigram_z: f64,
    pub phrase_length_bonus: f64,
    /// 单个声母（缩写）占位边的分数：没有字可出，只让切分走得通，任何真正出字的切法都比它好。
    pub initial_penalty: f64,
    pub collocation_scorer: Option<CollocationScorer<'a>>,
    pub collocation_weight: f64,
}

impl Default for DecodeOptions<'_> {
    fn default() -> Self {
        Self {
            beam: 32,
            span_limit: 32,
            max_phrase_syllables: 7,
            max_sequences_per_start: 256,
            language_model: None,
            dictionary_tiebreak: 0.01,
            reading_prior: 1.0,
            reading_prior_share: 0.01,
            reading_prior_floor: 5.0,
            unigram_z: 1e6,
            phrase_length_bonus: 3.0,
            initial_penalty: -12.0,
            collocation_scorer: None,
            collocation_weight: 0.0,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct DecodedPath {
    pub syllables: Vec<SyllableSpelling>,
    pub sentence: String,
    /// 全拼键（`'` 分隔）与逐词文本。
    pub key: String,
    pub words: Vec<String>,
    pub log_prob: f64,
    /// 路径上有占位边（声母缩写、查不到字的音节）时整句不完整，不能当整句候选。
    pub complete: bool,
}

/// 一条拼写序列（音节 + 辅码）铺出来的词，跨按键复用：只和词库、码表、打分选项有关，三者变了由调用方清空。
#[derive(Debug, Clone)]
pub struct CachedWord {
    pub word: String,
    pub key: String,
    pub base_score: f64,
    pub index: u32,
}
pub type WordEdgeMemo = HashMap<String, Vec<CachedWord>>;

fn heuristic_log_prob(weight: i64, syllables: usize, options: &DecodeOptions) -> f64 {
    let w = if weight > 0 { weight as f64 } else { 1.0 };
    let z = if options.unigram_z > 1.0 { options.unigram_z } else { 1e6 };
    let lp = w.ln();
    if syllables <= 1 {
        return lp - z.ln();
    }
    lp + options.phrase_length_bonus * syllables as f64
}

fn dictionary_tiebreak(weight: i64, options: &DecodeOptions) -> f64 {
    let w = if weight > 0 { weight as f64 } else { 1.0 };
    options.dictionary_tiebreak * w.log10()
}

fn reading_prior(weight: i64, span_total: i64, options: &DecodeOptions) -> f64 {
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

fn active_model<'a>(options: &DecodeOptions<'a>) -> Option<&'a dyn LanguageModel> {
    options.language_model.filter(|model| model.is_valid())
}

struct WordEdge<'a> {
    end: usize,
    word: String,
    key: String,
    base_score: f64,
    index: u32,
    /// 占位边：缩写声母或查不到字的音节，只为让切分走得通，不进语言模型。
    placeholder: bool,
    spellings: Vec<&'a SyllableSpelling>,
}

struct WordGraphBuilder<'a> {
    graph: &'a SpellingGraph,
    lookup: SpanLookup<'a>,
    accept: CharAccept<'a>,
    options: &'a DecodeOptions<'a>,
    model: Option<&'a dyn LanguageModel>,
    memo: Option<&'a mut WordEdgeMemo>,
    cache: HashMap<String, Vec<Lexeme>>,
    sequences: i32,
}

impl<'a> WordGraphBuilder<'a> {
    fn build(&mut self) -> Vec<Vec<WordEdge<'a>>> {
        let mut words: Vec<Vec<WordEdge<'a>>> = (0..self.graph.size).map(|_| Vec::new()).collect();
        for start in 0..self.graph.size {
            if self.graph.edges[start].is_empty() {
                continue;
            }
            self.sequences = 0;
            let mut sequence: Vec<&'a SyllableSpelling> = Vec::new();
            let mut out = Vec::new();
            self.extend(start, &mut sequence, &mut out);
            words[start] = out;
        }
        words
    }

    fn extend(&mut self, pos: usize, sequence: &mut Vec<&'a SyllableSpelling>, out: &mut Vec<WordEdge<'a>>) {
        if pos >= self.graph.size {
            return;
        }
        let graph = self.graph;
        for spelling in &graph.edges[pos] {
            if self.sequences >= self.options.max_sequences_per_start {
                return;
            }
            if spelling.kind == SpellingKind::Initial {
                // 声母缩写只单独成边，不拼进多字词：词格查的是完整读音。
                if sequence.is_empty() {
                    out.push(self.placeholder(spelling));
                }
                continue;
            }
            self.sequences += 1;
            sequence.push(spelling);
            self.emit(sequence, out);
            if (sequence.len() as i32) < self.options.max_phrase_syllables {
                self.extend(next_spelling_position(graph, spelling.end), sequence, out);
            }
            sequence.pop();
        }
    }

    fn placeholder(&self, spelling: &'a SyllableSpelling) -> WordEdge<'a> {
        WordEdge {
            end: spelling.end,
            word: String::new(),
            key: String::new(),
            base_score: self.options.initial_penalty,
            index: 0,
            placeholder: true,
            spellings: vec![spelling],
        }
    }

    fn rows_for(&mut self, span: &[String], constrained: bool) -> &Vec<Lexeme> {
        let mut key = span.join("'");
        if constrained {
            key.push('#');
        }
        let lookup = self.lookup;
        self.cache.entry(key).or_insert_with(|| lookup(span, constrained))
    }

    /// 这条拼写序列铺出来的词只取决于各音节的读音与辅码，跨按键记在 memo 里。
    fn memo_key(sequence: &[&SyllableSpelling]) -> String {
        let mut key = String::new();
        for s in sequence {
            key += &s.quanpin;
            key.push(if s.first != 0 { s.first as char } else { '-' });
            key.push(if s.second != 0 { s.second as char } else { '-' });
            key.push('|');
        }
        key
    }

    fn emit(&mut self, sequence: &[&'a SyllableSpelling], out: &mut Vec<WordEdge<'a>>) {
        let words: Vec<CachedWord> = if self.memo.is_some() {
            let key = Self::memo_key(sequence);
            if let Some(found) = self.memo.as_ref().and_then(|m| m.get(&key)) {
                found.clone()
            } else {
                let computed = self.lookup_words(sequence);
                self.memo.as_mut().unwrap().insert(key, computed.clone());
                computed
            }
        } else {
            self.lookup_words(sequence)
        };

        if words.is_empty() {
            // 单个音节查不到字时也留一条占位边，免得整条切分断掉；分数比声母缩写还低。
            if sequence.len() == 1 {
                let mut edge = self.placeholder(sequence[0]);
                edge.base_score = self.options.initial_penalty * 2.0;
                out.push(edge);
            }
            return;
        }
        for word in words {
            out.push(WordEdge {
                end: sequence.last().unwrap().end,
                word: word.word,
                key: word.key,
                base_score: word.base_score,
                index: word.index,
                placeholder: false,
                spellings: sequence.to_vec(),
            });
        }
    }

    fn lookup_words(&mut self, sequence: &[&SyllableSpelling]) -> Vec<CachedWord> {
        let span: Vec<String> = sequence.iter().map(|s| s.quanpin.clone()).collect();
        let constrained = sequence.iter().any(|s| s.has_aux());
        let (span_limit, accept) = (self.options.span_limit, self.accept);
        let rows = self.rows_for(&span, constrained).clone();

        let mut accepted: Vec<&Lexeme> = Vec::new();
        for row in &rows {
            if accepted.len() as i32 >= span_limit {
                break;
            }
            if row.value.is_empty() {
                continue;
            }
            let chars: Vec<String> = row.value.chars().map(String::from).collect();
            if chars.len() != sequence.len() {
                continue;
            }
            if sequence.iter().zip(&chars).all(|(s, c)| !s.has_aux() || accept(c, s.first, s.second)) {
                accepted.push(row);
            }
        }

        // 按读音分开求和，模糊读音的行不压低敲出读音的占比。
        let mut reading_totals: HashMap<&str, i64> = HashMap::new();
        for row in &accepted {
            *reading_totals.entry(row.key.as_str()).or_insert(0) += row.weight.max(0);
        }
        let span_key = span.join("'");
        let mut words = Vec::new();
        for row in &accepted {
            let mut word = CachedWord {
                word: row.value.clone(),
                key: if row.key.is_empty() { span_key.clone() } else { row.key.clone() },
                base_score: 0.0,
                index: 0,
            };
            if let Some(model) = self.model {
                word.index = model.index(&word.word);
                word.base_score =
                    dictionary_tiebreak(row.weight, self.options) + reading_prior(row.weight, reading_totals[row.key.as_str()], self.options);
            } else {
                word.base_score = heuristic_log_prob(row.weight, sequence.len(), self.options);
            }
            word.base_score -= row.penalty;
            words.push(word);
        }
        words
    }
}

struct Hyp {
    score: f64,
    prev: Option<(usize, usize)>,
    /// 走到这个假设的那条边（起点位置，该位置词边的下标）；分隔符直通时为空。
    edge: Option<(usize, usize)>,
    state: ModelState,
    collocation_tail: String,
}

fn tail_codepoints(text: &str, n: usize) -> String {
    let total = text.chars().count();
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
    // C++ 的 partial_sort 对平局的次序没有规定，这里照 MSVC 的实现排。
    crate::msvc_sort::partial_sort_by(column, beam, |a, b| a.score > b.score);
    column.truncate(beam);
}

pub fn decode_best_path<'a>(
    graph: &'a SpellingGraph,
    lookup: SpanLookup<'a>,
    accept: CharAccept<'a>,
    options: &'a DecodeOptions<'a>,
    memo: Option<&'a mut WordEdgeMemo>,
) -> Option<DecodedPath> {
    if graph.is_empty() || graph.size == 0 {
        return None;
    }
    let n = graph.size;
    let model = active_model(options);
    let collocation = options.collocation_scorer.is_some() && options.collocation_weight > 0.0;
    let words = {
        let mut builder = WordGraphBuilder { graph, lookup, accept, options, model, memo, cache: HashMap::new(), sequences: 0 };
        builder.build()
    };

    let mut columns: Vec<Vec<Hyp>> = (0..=n).map(|_| Vec::new()).collect();
    columns[0].push(Hyp {
        score: 0.0,
        prev: None,
        edge: None,
        state: model.map(|m| m.null_state()).unwrap_or_default(),
        collocation_tail: String::new(),
    });

    for pos in 0..n {
        keep_beam(&mut columns[pos], options.beam);
        if columns[pos].is_empty() {
            continue;
        }
        for hi in 0..columns[pos].len() {
            if graph.skip[pos] {
                let hyp = &columns[pos][hi];
                let next = Hyp { score: hyp.score, prev: Some((pos, hi)), edge: None, state: hyp.state.clone(), collocation_tail: hyp.collocation_tail.clone() };
                columns[pos + 1].push(next);
            }
            for (ei, edge) in words[pos].iter().enumerate() {
                let hyp = &columns[pos][hi];
                let mut score = hyp.score + edge.base_score;
                let mut state = ModelState::new();
                let mut collocation_tail = String::new();
                if edge.placeholder || model.is_none() {
                    state = hyp.state.clone();
                }
                // 关闭搭配项时尾窗始终为空。
                if collocation && edge.placeholder {
                    collocation_tail = hyp.collocation_tail.clone();
                }
                if !edge.placeholder {
                    if let Some(model) = model {
                        let (model_score, next_state) = model.score(&hyp.state, edge.index);
                        score += f64::from(model_score);
                        state = next_state;
                    }
                    if collocation {
                        let is_rear = next_spelling_position(graph, edge.end) == n;
                        score += options.collocation_weight * (options.collocation_scorer.unwrap())(&hyp.collocation_tail, &edge.word, is_rear);
                        collocation_tail = tail_codepoints(&format!("{}{}", hyp.collocation_tail, edge.word), COLLOCATION_TAIL_CODEPOINTS);
                    }
                }
                columns[edge.end].push(Hyp { score, prev: Some((pos, hi)), edge: Some((pos, ei)), state, collocation_tail });
            }
        }
    }

    let final_col = &columns[n];
    if final_col.is_empty() {
        return None;
    }
    // max_element：并列时取最先出现的。
    let mut best = 0;
    for (i, hyp) in final_col.iter().enumerate() {
        if final_col[best].score < hyp.score {
            best = i;
        }
    }

    let mut path = DecodedPath { syllables: Vec::new(), sentence: String::new(), key: String::new(), words: Vec::new(), log_prob: final_col[best].score, complete: true };
    let mut edges: Vec<&WordEdge> = Vec::new();
    let (mut pos, mut idx) = (n as i64, best as i64);
    while pos > 0 && idx >= 0 {
        let hyp = &columns[pos as usize][idx as usize];
        if let Some((start, ei)) = hyp.edge {
            edges.push(&words[start][ei]);
        }
        match hyp.prev {
            Some((p, i)) => {
                pos = p as i64;
                idx = i as i64;
            }
            None => break,
        }
    }
    edges.reverse();
    for edge in edges {
        path.complete = path.complete && !edge.placeholder;
        path.sentence += &edge.word;
        if !edge.placeholder {
            if !path.key.is_empty() {
                path.key.push('\'');
            }
            path.key += &edge.key;
            path.words.push(edge.word.clone());
        }
        path.syllables.extend(edge.spellings.iter().map(|s| (*s).clone()));
    }
    Some(path)
}
