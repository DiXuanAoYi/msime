//! 双拼直接辅助码的入口，移植自 `engine/direct_helpcode/direct_resolver.{h,cpp}`。
//!
//! 把用户敲的原串（`uiauiq`）整句解码，选出切分，再把请求改写成句中辅助码的形状（`ui'ui` + 每个音节上的约束），
//! 之后的查词、整句、选词推进、预编辑全部复用句中辅助码那条已有的路径。
//!
//! 和 C++ 的差别：C++ 的 `QueryRequest` 还没有移植，这里改写的结果放在 [`Rewrite`] 里（只含 `apply_spellings`
//! 会写的那些字段）；跨按键的查词缓存、整句缓存和搭配分缓存只影响速度、不影响结果，没有搬过来
//! （词边缓存 [`WordEdgeMemo`] 留着，因为解码器的接口里有它）。

use crate::direct_lattice::{decode_best_path, CharAccept, DecodeOptions, DecodedPath, SpanLookup, WordEdgeMemo};
use crate::lattice::Lexeme;
use crate::shuangpin::Profile;
use crate::shuangpin_query::{self as query, SyllableHelpcode};
use crate::spelling_graph::{build_spelling_graph, single_path, SpellingKind, SpellingOptions, SyllableSpelling};
use std::collections::{HashMap, HashSet};

const SPAN_CACHE_LIMIT: usize = 8192;

/// 选中的切分改写出来的请求字段，对应 C++ `apply_spellings` 写进 `QueryRequest` 的那些。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Rewrite {
    pub raw_input_with_syllable_helpcodes: String,
    /// 去掉辅码、音节之间按需补 `'` 的双拼串；同时是 `raw_input` 和 `raw_input_with_cases`。
    pub raw_input: String,
    pub syllable_helpcodes: Vec<SyllableHelpcode>,
    /// 去段串到原串的下标映射，末尾多一项等于原串长度。
    pub source_index: Vec<usize>,
    pub decorations: Vec<(usize, String)>,
    pub valid: bool,
    pub raw_segmentation: String,
    /// 同时是 `normalized_segmentation` 和 `segmentation`。
    pub normalized_segmentation: String,
    pub normalized_input: String,
}

/// 两个相邻音节之间要不要放一个手动分隔符：前一个音节后面挂着辅码、任一个是单键声母、或原串里两者之间本来就隔着
/// 别的字符时都要放。其余情况是连续的两键音节，贪心切分自己就切得出来，不放正是为了让它们和普通双拼走完全相同的路径。
fn needs_delimiter(previous: &SyllableSpelling, current: &SyllableSpelling) -> bool {
    previous.end != previous.begin + previous.code.len()
        || previous.kind == SpellingKind::Initial
        || current.kind == SpellingKind::Initial
        || current.begin != previous.end
}

/// 按选中的切分改写请求，纯函数。`typed` 是原串（保留大小写）。
pub fn apply_spellings(typed: &str, spellings: &[SyllableSpelling], profile: &Profile) -> Rewrite {
    let mut clean = String::new();
    let mut rewrite = Rewrite::default();
    for (i, spelling) in spellings.iter().enumerate() {
        if i > 0 && needs_delimiter(&spellings[i - 1], spelling) {
            let previous = &spellings[i - 1];
            clean.push('\'');
            // 和句中辅助码一致：分隔符对应前一个音节辅码段的起点。
            rewrite.source_index.push(previous.begin + previous.code.len());
        }
        for (j, ch) in spelling.code.chars().enumerate() {
            clean.push(ch);
            rewrite.source_index.push(spelling.begin + j);
        }
        let code_end = spelling.begin + spelling.code.len();
        if spelling.end > code_end {
            rewrite.decorations.push((i, typed[code_end..spelling.end].to_string()));
        }
        if spelling.has_aux() {
            rewrite.syllable_helpcodes.push(SyllableHelpcode { syllable: i, first: spelling.first, second: spelling.second });
        }
    }
    rewrite.source_index.push(typed.len());
    rewrite.raw_input_with_syllable_helpcodes = typed.to_string();
    rewrite.valid = !clean.is_empty();
    if rewrite.valid {
        let raw_segmentation = query::segment_input(&clean, profile);
        rewrite.normalized_segmentation = query::to_quanpin_segmentation(&raw_segmentation, profile);
        rewrite.normalized_input = query::normalize_input(&clean, profile);
        rewrite.raw_segmentation = raw_segmentation;
    }
    rewrite.raw_input = clean;
    rewrite
}

/// 全拼读音 + 单字，词库里所有单字行，建辅码索引用。
pub type SingleCharRows<'a> = &'a dyn Fn() -> Vec<(String, String)>;

pub struct ResolveContext<'a> {
    pub lookup: Option<SpanLookup<'a>>,
    /// 一次取出词库里全部单字行。为空时退回按音节逐个查。
    pub single_char_rows: Option<SingleCharRows<'a>>,
    /// 当前辅助码方案的码表（汉字 → 辅码）；为空时没有辅码边，整串按普通双拼解。
    pub keymap: Option<&'a HashMap<String, String>>,
    pub options: DecodeOptions<'a>,
}

/// 一次 `resolve` 的结果。
#[derive(Debug, Clone, Default)]
pub struct Resolved {
    /// 改写了请求。选中的切分里没有辅码、和普通双拼切得一样时不改写，那时请求本来就对。
    pub rewrite: Option<Rewrite>,
    /// 这次顺带解出的最优整句；只有一条切分（没解码）或路径不完整时为空。
    pub sentence: Option<DecodedPath>,
}

#[derive(Default)]
struct SyllableAux {
    first: [bool; 26],
    pairs: HashSet<String>,
}

impl SyllableAux {
    fn add(&mut self, code: &str) {
        let bytes = code.as_bytes();
        if bytes[0].is_ascii_lowercase() {
            self.first[(bytes[0] - b'a') as usize] = true;
        }
        if bytes.len() > 1 {
            self.pairs.insert(code[..2].to_string());
        }
    }
}

fn is_single_utf8_char(text: &str) -> bool {
    text.chars().count() == 1
}

pub struct Resolver {
    profile: Profile,
    aux_index: HashMap<String, SyllableAux>,
    aux_index_built: bool,
    word_memo: WordEdgeMemo,
}

impl Resolver {
    pub fn new(profile: &Profile) -> Self {
        Self { profile: profile.clone(), aux_index: HashMap::new(), aux_index_built: false, word_memo: WordEdgeMemo::new() }
    }

    /// 码表换了：辅码索引作废（它只依赖单字读音和码表）。词库或整句打分选项变了用 [`Resolver::reset_cache`]。
    pub fn reset_keymap(&mut self) {
        self.reset_cache();
        self.aux_index.clear();
        self.aux_index_built = false;
    }

    pub fn reset_cache(&mut self) {
        self.word_memo.clear();
    }

    fn build_aux_index(&mut self, context: &ResolveContext) {
        self.aux_index_built = true;
        let (Some(rows), Some(keymap)) = (context.single_char_rows, context.keymap) else { return };
        // 一次扫完词库的单字行，对应万象词库里为每个字派生的 拼音;辅码 拼写。
        for (quanpin, hanzi) in rows() {
            match keymap.get(&hanzi) {
                Some(code) if !code.is_empty() => self.aux_index.entry(quanpin).or_default().add(code),
                _ => {}
            }
        }
    }

    fn has_aux(&mut self, quanpin: &str, first: u8, second: u8, context: &ResolveContext) -> bool {
        let Some(keymap) = context.keymap else { return false };
        if !first.is_ascii_lowercase() {
            return false;
        }
        if !self.aux_index_built {
            self.build_aux_index(context);
        }
        if !self.aux_index.contains_key(quanpin) {
            // 全量扫描里没有这个读音（没有全量扫描，或词库键的写法不同，如 ü）：退回按音节查一次，结果同样记下，之后不再查。
            let mut aux = SyllableAux::default();
            let rows = context.lookup.map(|lookup| lookup(&[quanpin.to_string()], true)).unwrap_or_default();
            for row in &rows {
                if !is_single_utf8_char(&row.value) {
                    continue;
                }
                match keymap.get(&row.value) {
                    Some(code) if !code.is_empty() => aux.add(code),
                    _ => {}
                }
            }
            self.aux_index.insert(quanpin.to_string(), aux);
        }
        let aux = &self.aux_index[quanpin];
        if second == 0 {
            aux.first[(first - b'a') as usize]
        } else {
            aux.pairs.contains(&String::from_utf8(vec![first, second]).unwrap_or_default())
        }
    }

    /// `typed` 是原串（`raw_input_with_cases`，空则取 `raw_input`，由调用方选）。
    pub fn resolve(&mut self, typed: &str, spelling_options: &SpellingOptions, context: &ResolveContext) -> Resolved {
        if typed.is_empty() {
            return Resolved::default();
        }
        // 先在一个临时的 Resolver 视图上建图：谓词要改索引，所以用 RefCell 绕开借用。
        let cell = std::cell::RefCell::new(&mut *self);
        let predicate = |quanpin: &str, first: u8, second: u8| cell.borrow_mut().has_aux(quanpin, first, second, context);
        let profile = cell.borrow().profile.clone();
        let graph = build_spelling_graph(typed, &profile, Some(&predicate), spelling_options);
        drop(cell);

        let mut spellings: Vec<SyllableSpelling> = Vec::new();
        let mut sentence: Option<DecodedPath> = None;
        // 只有一条完整切分时没有可比的切法，跳过整句解码；整句留给词典层自己的词格去解。
        if !graph.is_empty() {
            if let Some(only) = single_path(&graph) {
                spellings = only;
            } else {
                let no_rows = |_: &[String], _: bool| Vec::<Lexeme>::new();
                let lookup: SpanLookup = context.lookup.unwrap_or(&no_rows);
                let keymap = context.keymap;
                let accept = |hanzi: &str, first: u8, second: u8| {
                    keymap
                        .and_then(|k| k.get(hanzi))
                        .is_some_and(|code| SyllableHelpcode { syllable: 0, first, second }.matches(code))
                };
                if self.word_memo.len() >= SPAN_CACHE_LIMIT {
                    self.word_memo.clear();
                }
                let accept_ref: CharAccept = &accept;
                if let Some(path) = decode_best_path(&graph, lookup, accept_ref, &context.options, Some(&mut self.word_memo)) {
                    spellings = path.syllables.clone();
                    if path.complete && !path.sentence.is_empty() {
                        sentence = Some(path);
                    }
                }
            }
        }
        if spellings.is_empty() {
            return Resolved::default();
        }

        // 没有辅码、也没有吃掉分隔符以外的字符时，选中的切分就是普通双拼的切分：请求原样放行。
        let plain = spellings.iter().all(|s| s.end == s.begin + s.code.len());
        if plain {
            let joined = spellings.iter().map(|s| s.code.as_str()).collect::<Vec<_>>().join("'");
            if joined == query::segment_input(&typed.to_ascii_lowercase(), &self.profile) {
                return Resolved { rewrite: None, sentence };
            }
        }
        Resolved { rewrite: Some(apply_spellings(typed, &spellings, &self.profile)), sentence }
    }
}
