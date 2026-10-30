//! 双拼直接辅助码（万象「直接辅助」）的拼写图，移植自 `engine/direct_helpcode/spelling_graph.{h,cpp}`。
//!
//! 在按键位置上把所有能匹配的拼写连成图，交给词格整句解码。每个位置上一个音节可以写成：
//! 双拼两键（`Plain`）、加第一位辅码（`Aux1`）、加两位辅码并补 `/`（`Aux2Slash`）、第二位辅码大写
//! （`Aux2Upper`）、加两位辅码不带标记（`Aux2Abbrev`，缩写），或单个声母键（`Initial`，缩写）。
//! 辅码只有在“这个音节下真有字的辅码以它开头”时才成立（`has_aux` 谓词）。
//!
//! 缩写对应 librime 的 kAbbreviation：只要存在一条全由非缩写拼写组成、走到末尾的路径，所有缩写边就被剪掉。
//! 开着大写标记时大写字母只能当四码的第二位辅码；一条都走不通时退回不分大小写的旧规则，`/` 与 `;` 也退回成分隔符。
//! 输入按 ASCII 字节处理。

use crate::shuangpin::{self, Profile};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SpellingKind {
    Plain,
    Aux1,
    Aux2Slash,
    Aux2Upper,
    Aux2Abbrev,
    Initial,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SyllableSpelling {
    /// 在输入串里的范围 `[begin, end)`，含辅码与 `/`。
    pub begin: usize,
    pub end: usize,
    /// 小写的双拼码：两键，`Initial` 是一键。
    pub code: String,
    /// 全拼音节；`Initial` 是声母。
    pub quanpin: String,
    /// 小写辅码，0 表示没有。
    pub first: u8,
    pub second: u8,
    pub kind: SpellingKind,
}

impl SyllableSpelling {
    pub fn is_abbreviation(&self) -> bool {
        matches!(self.kind, SpellingKind::Aux2Abbrev | SpellingKind::Initial)
    }

    pub fn has_aux(&self) -> bool {
        self.first != 0
    }
}

/// 全拼音节下有没有字的辅码以 `first`（`second` 非 0 时为 `first` + `second`）开头。
pub type AuxPredicate<'a> = &'a dyn Fn(&str, u8, u8) -> bool;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SpellingGraph {
    pub size: usize,
    /// `edges[pos]`：从 `pos` 开始的拼写。只保留落在某条从 0 到 `size` 的完整路径上的边。
    pub edges: Vec<Vec<SyllableSpelling>>,
    /// `skip[pos]`：`pos` 上是分隔符，不出字，直接走到 `pos + 1`。
    pub skip: Vec<bool>,
}

impl SpellingGraph {
    pub fn is_empty(&self) -> bool {
        self.edges.is_empty()
    }
}

/// 句中四码用什么结束，可以两个都开。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SpellingOptions {
    /// `uiab/`：两位辅码后补 `/`。
    pub slash_marker: bool,
    /// `uiaB`：第二位辅码敲成大写。
    pub uppercase_marker: bool,
}

impl Default for SpellingOptions {
    fn default() -> Self {
        Self { slash_marker: true, uppercase_marker: false }
    }
}

fn syllable_quanpin(code: &str, profile: &Profile) -> String {
    if !shuangpin::is_accepted_syllable_code(code, profile) {
        return String::new();
    }
    shuangpin::to_pinyin(code, profile)
}

fn spelling(begin: usize, end: usize, code: &str, quanpin: &str, kind: SpellingKind, first: u8, second: u8) -> SyllableSpelling {
    SyllableSpelling { begin, end, code: code.to_string(), quanpin: quanpin.to_string(), first, second, kind }
}

fn forward_reach(graph: &SpellingGraph, normal_only: bool) -> Vec<bool> {
    let mut reach = vec![false; graph.size + 1];
    reach[0] = true;
    for pos in 0..graph.size {
        if !reach[pos] {
            continue;
        }
        if graph.skip[pos] {
            reach[pos + 1] = true;
        }
        for s in &graph.edges[pos] {
            if !normal_only || !s.is_abbreviation() {
                reach[s.end] = true;
            }
        }
    }
    reach
}

fn build_attempt(typed: &[u8], profile: &Profile, has_aux: Option<AuxPredicate>, options: &SpellingOptions, lenient: bool) -> SpellingGraph {
    let n = typed.len();
    let mut graph = SpellingGraph { size: n, edges: vec![Vec::new(); n], skip: vec![false; n] };
    // 大写标记开着、又不是兜底那一遍时，大写字母只能当四码的第二位辅码。
    let strict_case = options.uppercase_marker && !lenient;
    let usable = |index: usize| !strict_case || !typed[index].is_ascii_uppercase();
    for pos in 0..n {
        let ch = typed[pos];
        graph.skip[pos] = ch == b'\'' || (lenient && (ch == b'/' || ch == b';'));
        if !ch.is_ascii_alphabetic() || !usable(pos) {
            continue;
        }
        let out = &mut graph.edges[pos];
        if pos + 1 < n && ((typed[pos + 1].is_ascii_alphabetic() && usable(pos + 1)) || typed[pos + 1] == b';') {
            let code = String::from_utf8(vec![ch.to_ascii_lowercase(), typed[pos + 1].to_ascii_lowercase()]).unwrap();
            let quanpin = syllable_quanpin(&code, profile);
            if !quanpin.is_empty() {
                out.push(spelling(pos, pos + 2, &code, &quanpin, SpellingKind::Plain, 0, 0));
                if pos + 2 < n && typed[pos + 2].is_ascii_alphabetic() && usable(pos + 2) {
                    let first = typed[pos + 2].to_ascii_lowercase();
                    if has_aux.is_some_and(|f| f(&quanpin, first, 0)) {
                        out.push(spelling(pos, pos + 3, &code, &quanpin, SpellingKind::Aux1, first, 0));
                    }
                    if pos + 3 < n && typed[pos + 3].is_ascii_alphabetic() {
                        let second = typed[pos + 3].to_ascii_lowercase();
                        if has_aux.is_some_and(|f| f(&quanpin, first, second)) {
                            if options.slash_marker && pos + 4 < n && typed[pos + 4] == b'/' {
                                out.push(spelling(pos, pos + 5, &code, &quanpin, SpellingKind::Aux2Slash, first, second));
                            }
                            if options.uppercase_marker && typed[pos + 3].is_ascii_uppercase() {
                                out.push(spelling(pos, pos + 4, &code, &quanpin, SpellingKind::Aux2Upper, first, second));
                            } else {
                                out.push(spelling(pos, pos + 4, &code, &quanpin, SpellingKind::Aux2Abbrev, first, second));
                            }
                        }
                    }
                }
            }
        }
        let initial = (ch.to_ascii_lowercase() as char).to_string();
        out.push(spelling(pos, pos + 1, &initial, &shuangpin::segmented_to_pinyin(&initial, profile), SpellingKind::Initial, 0, 0));
    }

    // librime：存在一条全由非缩写拼写走到末尾的路径时，缩写边全部作废。
    if forward_reach(&graph, true)[n] {
        for out in &mut graph.edges {
            out.retain(|s| !s.is_abbreviation());
        }
    }

    let reach = forward_reach(&graph, false);
    if !reach[n] {
        return SpellingGraph::default();
    }
    // 只留落在完整路径上的边：词格只需要能走到末尾的那些。
    let mut to_end = vec![false; n + 1];
    to_end[n] = true;
    for pos in (0..n).rev() {
        to_end[pos] = (graph.skip[pos] && to_end[pos + 1]) || graph.edges[pos].iter().any(|s| to_end[s.end]);
    }
    for pos in 0..n {
        if !reach[pos] {
            graph.edges[pos].clear();
        } else {
            graph.edges[pos].retain(|s| to_end[s.end]);
        }
    }
    graph
}

/// 走不通时返回空图。
pub fn build_spelling_graph(typed: &str, profile: &Profile, has_aux: Option<AuxPredicate>, options: &SpellingOptions) -> SpellingGraph {
    if typed.is_empty() {
        return SpellingGraph::default();
    }
    let graph = build_attempt(typed.as_bytes(), profile, has_aux, options, false);
    if graph.is_empty() {
        // 用不上的 / 和 ; 退回成分隔符、大写字母不再只当第二位辅码，候选照常出，不至于整串作废。
        return build_attempt(typed.as_bytes(), profile, has_aux, options, true);
    }
    graph
}

/// 下一个出字的位置：跳过 `pos` 起连续的分隔符。
pub fn next_spelling_position(graph: &SpellingGraph, mut pos: usize) -> usize {
    while pos < graph.size && graph.skip[pos] {
        pos += 1;
    }
    pos
}

/// 图里只有一条完整切分时返回它：没有可比的切法，不必整句解码。有零条或多条时返回 `None`。
pub fn single_path(graph: &SpellingGraph) -> Option<Vec<SyllableSpelling>> {
    if graph.is_empty() {
        return None;
    }
    // 从每个位置走到末尾有几条路，数到 2 就够了。
    let n = graph.size;
    let mut ways = vec![0i32; n + 1];
    ways[n] = 1;
    for pos in (0..n).rev() {
        let mut count = if graph.skip[pos] { ways[pos + 1] } else { 0 };
        for s in &graph.edges[pos] {
            count += ways[s.end];
        }
        ways[pos] = count.min(2);
    }
    if ways[0] != 1 {
        return None;
    }
    let mut path = Vec::new();
    let mut pos = 0;
    while pos < n {
        if graph.skip[pos] {
            pos += 1;
            continue;
        }
        let next = graph.edges[pos].iter().find(|s| ways[s.end] > 0)?;
        path.push(next.clone());
        pos = next.end;
    }
    Some(path)
}
