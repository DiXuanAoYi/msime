//! 全拼手误纠错的“错拼 → 正确音节”表。
//!
//! C++ 那边是脚本 `server/scripts/generate_quanpin_autocorrect.py` 生成的 30 万字节头文件
//! `engine/quanpin/autocorrect_table.h`。这里不抄那份数据，而是把同样的生成规则用 Rust 写一遍，
//! 启动时现算；`tests/autocorrect_table_matches_cpp.rs` 会把算出来的四张表和那份头文件逐行比对，
//! 规则一旦和 C++ 对不上就红。
//!
//! 四种手误各自可以单独开关，位值和 `quanpin_utils.h` 的 `kAutocorrect*` 一致：
//! 换位（sahng → shang）、邻键（shabg → shang）、漏键（shng → shang）、多键（sjhang → shang，插入的是相邻键的 QWERTY 邻键）。
//! 一个错拼可以对应多个正确音节（漏键的 `shng` → `shang`、`sheng`），全部保留，由后面的切分和词频裁决。
//! 长度不足 3 的串不收：两个字母是简拼的地盘，`wj` 只能是 w+j，不能被改成 `wu`。

use crate::quanpin_utils::{canonical_umlaut_spelling, is_jqxy_v_alias};
use crate::syllables::INTACT;
use std::collections::{BTreeMap, BTreeSet};

pub const TRANSPOSITION: u32 = 1 << 0;
pub const NEIGHBOR: u32 = 1 << 1;
pub const DELETION: u32 = 1 << 2;
pub const INSERTION: u32 = 1 << 3;

/// 错拼 → 可能的正确音节（有序，方便比对）。
pub type Table = BTreeMap<String, BTreeSet<String>>;

pub struct Tables {
    pub transposition: Table,
    pub neighbor: Table,
    pub deletion: Table,
    pub insertion: Table,
    /// 表外“生成式”形状（远键替换，zher → zhe、shatg → shang）：已剔除和前四张表重复的对。
    pub generated: Table,
}

/// 开关 → 掩码的唯一实现，对应 C++ `autocorrect_types_from_switches`：四个开关一一映射到各自的位，互不联动。
pub fn autocorrect_types_from_switches(transposition: bool, neighbor: bool, deletion: bool, insertion: bool) -> u32 {
    (if transposition { TRANSPOSITION } else { 0 })
        | (if neighbor { NEIGHBOR } else { 0 })
        | (if deletion { DELETION } else { 0 })
        | (if insertion { INSERTION } else { 0 })
}

/// 掩码下手误别名层（切分里的 aing → iang 之类）是否生效，对应 C++ `correction_aliases_enabled`：只看换位、邻键两位。
pub fn correction_aliases_enabled(autocorrect_types: u32) -> bool {
    autocorrect_types & (TRANSPOSITION | NEIGHBOR) != 0
}

/// QWERTY 字母键的邻键（手指够得着的范围）。
fn neighbors(letter: char) -> &'static [char] {
    match letter {
        'q' => &['w', 'a'],
        'w' => &['q', 'e', 's', 'a'],
        'e' => &['w', 'r', 's', 'd'],
        'r' => &['e', 't', 'd', 'f'],
        't' => &['r', 'y', 'f', 'g'],
        'y' => &['t', 'u', 'g', 'h'],
        'u' => &['y', 'i', 'h', 'j'],
        'i' => &['u', 'o', 'j', 'k'],
        'o' => &['i', 'p', 'k', 'l'],
        'p' => &['o', 'l'],
        'a' => &['q', 'w', 's', 'z'],
        's' => &['a', 'w', 'd', 'e', 'x', 'z'],
        'd' => &['s', 'e', 'r', 'f', 'c', 'x'],
        'f' => &['d', 'r', 't', 'g', 'v', 'c'],
        'g' => &['f', 't', 'y', 'h', 'b', 'v'],
        'h' => &['g', 'y', 'u', 'j', 'n', 'b'],
        'j' => &['h', 'u', 'i', 'k', 'm', 'n'],
        'k' => &['j', 'i', 'o', 'l', 'm'],
        'l' => &['k', 'o', 'p'],
        'z' => &['a', 's', 'x'],
        'x' => &['z', 's', 'd', 'c'],
        'c' => &['x', 'd', 'f', 'v'],
        'v' => &['c', 'f', 'g', 'b'],
        'b' => &['v', 'g', 'h', 'n'],
        'm' => &['n', 'j', 'k'],
        'n' => &['b', 'h', 'j', 'm'],
        _ => &[],
    }
}

fn transposition_variants(syllable: &str) -> Vec<String> {
    let chars: Vec<char> = syllable.chars().collect();
    (0..chars.len().saturating_sub(1))
        .map(|i| {
            let mut swapped = chars.clone();
            swapped.swap(i, i + 1);
            swapped.into_iter().collect()
        })
        .collect()
}

fn neighbor_variants(syllable: &str) -> Vec<String> {
    let chars: Vec<char> = syllable.chars().collect();
    let mut out = Vec::new();
    for (i, ch) in chars.iter().enumerate() {
        for neighbor in neighbors(*ch) {
            let mut replaced = chars.clone();
            replaced[i] = *neighbor;
            out.push(replaced.into_iter().collect());
        }
    }
    out
}

fn deletion_variants(syllable: &str) -> Vec<String> {
    let chars: Vec<char> = syllable.chars().collect();
    (0..chars.len())
        .map(|i| {
            let mut kept = chars.clone();
            kept.remove(i);
            kept.into_iter().collect()
        })
        .collect()
}

/// 插入一个字母：必须像真的手滑，也就是相邻键的 QWERTY 邻键。连击（重复相邻键）不收。
fn insertion_variants(syllable: &str) -> Vec<String> {
    let chars: Vec<char> = syllable.chars().collect();
    let mut out = Vec::new();
    for position in 0..=chars.len() {
        let mut letters: BTreeSet<char> = BTreeSet::new();
        if position > 0 {
            letters.extend(neighbors(chars[position - 1]));
        }
        if position < chars.len() {
            letters.extend(neighbors(chars[position]));
        }
        for letter in letters {
            let mut inserted = chars.clone();
            inserted.insert(position, letter);
            out.push(inserted.into_iter().collect());
        }
    }
    out
}

/// 远键替换：换上的字母既不是原字母也不是它的 QWERTY 邻键。
fn far_substitution_variants(syllable: &str) -> Vec<String> {
    let chars: Vec<char> = syllable.chars().collect();
    let mut out = Vec::new();
    for (i, ch) in chars.iter().enumerate() {
        for key in 'a'..='z' {
            if key == *ch || neighbors(*ch).contains(&key) {
                continue;
            }
            let mut replaced = chars.clone();
            replaced[i] = key;
            out.push(replaced.into_iter().collect());
        }
    }
    out
}

fn build(make_variants: fn(&str) -> Vec<String>) -> Table {
    let mut table = Table::new();
    for syllable in INTACT {
        // j/q/x/y 后面的 v 只是 u 的别名，不从它们派生纠错键。
        if syllable.len() < 2 || is_jqxy_v_alias(syllable) {
            continue;
        }
        for variant in make_variants(syllable) {
            // 自己、合法音节、不足三个字母（简拼的地盘）、超过六个字母都不收。
            if variant == *syllable || INTACT.contains(&variant.as_str()) || variant.len() < 3 || variant.len() > 6 {
                continue;
            }
            table.entry(variant).or_default().insert(syllable.to_string());
        }
    }
    table
}

pub fn tables() -> Tables {
    let transposition = build(transposition_variants);
    let neighbor = build(neighbor_variants);
    let deletion = build(deletion_variants);
    let insertion = build(insertion_variants);
    // 生成式形状剔除和静态表重复的 (错拼, 音节) 对，剔空的键也不留。
    let mut generated = build(far_substitution_variants);
    for (wrong, corrects) in generated.iter_mut() {
        for table in [&transposition, &neighbor, &deletion, &insertion] {
            if let Some(static_corrects) = table.get(wrong) {
                corrects.retain(|correct| !static_corrects.contains(correct));
            }
        }
    }
    generated.retain(|_, corrects| !corrects.is_empty());
    Tables { transposition, neighbor, deletion, insertion, generated }
}

/// 单个纠错边的代价，和 `quanpin_utils.h` 的 `kAutocorrect*Weight` 一致：换位最便宜，邻键最贵。
pub const WEIGHT_TRANSPOSITION: i32 = 10;
pub const WEIGHT_DELETION: i32 = 11;
pub const WEIGHT_INSERTION: i32 = 12;
pub const WEIGHT_NEIGHBOR: i32 = 13;

/// 一条切分里最多几处纠正；再多几乎都是凑巧拼出来的读法。
pub const MAX_EDGES: usize = 3;
const MAX_INPUT_LENGTH: usize = 64;

/// 表外“生成式”纠正的代价：比任何表内纠正都贵，所以表内有解释时永远排在它前面。
pub const WEIGHT_GENERATED: i32 = 15;

/// 某个错拼的一个目标音节，`types` 是提供这对（错拼, 音节）的所有纠错类型。
#[derive(Debug, Clone, PartialEq)]
pub struct Target {
    pub syllable: String,
    pub types: u32,
    /// 表外生成的一对（远键替换）：代价固定为 [`WEIGHT_GENERATED`]，开关仍按 `types` 判。
    pub generated: bool,
}

/// 切分里的一段。
#[derive(Debug, Clone, PartialEq)]
pub struct Segment {
    pub syllable: String,
    /// 对应敲出的字母；漏键比音节少一个字母，多键多一个。
    pub raw: String,
    /// `raw` 在敲出的字母串里的起点。
    pub start: usize,
    pub corrected: bool,
    pub weight: i32,
}

/// 一种带纠正的切分。
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Cut {
    pub segments: Vec<Segment>,
    pub edges: usize,
    pub weight: i32,
    /// 读法里有生成式的边。
    pub has_generated: bool,
}

impl Cut {
    pub fn syllables(&self) -> Vec<String> {
        self.segments.iter().map(|s| s.syllable.clone()).collect()
    }

    /// 同一档手误代价：纠正边数和权重和都相等。
    pub fn same_cost_as(&self, other: &Cut) -> bool {
        self.edges == other.edges && self.weight == other.weight
    }
}

/// 打开的类型里最便宜的那个的代价。
fn cheapest_weight(types: u32) -> i32 {
    [
        (TRANSPOSITION, WEIGHT_TRANSPOSITION),
        (DELETION, WEIGHT_DELETION),
        (INSERTION, WEIGHT_INSERTION),
        (NEIGHBOR, WEIGHT_NEIGHBOR),
    ]
    .into_iter()
    .filter(|(bit, _)| types & bit != 0)
    .map(|(_, weight)| weight)
    .min()
    .unwrap_or(0)
}

struct Hypothesis {
    edges: usize,
    weight: i32,
    arrival: usize,
    prev: Option<usize>,
    segment: Option<Segment>,
    has_generated: bool,
    /// 到这里为止的音节序列，同一序列只留排名最靠前的一条。
    sequence: Vec<String>,
}

type Index = std::collections::HashMap<String, Vec<Target>>;

/// 错拼 → 目标音节：先是四张静态表（换位、漏键、多键、邻键，同表内按字母序，这是 C++ 并列时的“表序”），
/// 再是生成式的对（生成期已经剔除了和静态表重复的，这里只追加）。
fn build_index(tables: &Tables) -> Index {
    let mut index: Index = std::collections::HashMap::new();
    for (bit, table) in [
        (TRANSPOSITION, &tables.transposition),
        (DELETION, &tables.deletion),
        (INSERTION, &tables.insertion),
        (NEIGHBOR, &tables.neighbor),
    ] {
        for (wrong, corrects) in table {
            let targets = index.entry(wrong.clone()).or_default();
            for correct in corrects {
                match targets.iter_mut().find(|t| &t.syllable == correct) {
                    Some(existing) => existing.types |= bit,
                    None => targets.push(Target { syllable: correct.clone(), types: bit, generated: false }),
                }
            }
        }
    }
    for (wrong, corrects) in &tables.generated {
        let targets = index.entry(wrong.clone()).or_default();
        for correct in corrects {
            if !targets.iter().any(|t| &t.syllable == correct) {
                targets.push(Target { syllable: correct.clone(), types: NEIGHBOR, generated: true });
            }
        }
    }
    index
}

fn shared_index() -> &'static Index {
    static INDEX: std::sync::OnceLock<Index> = std::sync::OnceLock::new();
    INDEX.get_or_init(|| build_index(&tables()))
}

impl Tables {
    /// 把敲出的字母切成音节，允许最多 [`MAX_EDGES`] 处纠正，返回最好的 `k` 种读法，最便宜的在前
    /// （排序键：有无生成式边，纠正边数，权重和，最后是产生的先后）。每种读法至少带一处纠正；
    /// 本来就是合法切分、又没有纠错读法的输入返回空。对应 C++ `quanpin::autocorrect_cut_kbest`。
    ///
    /// 静态优先：最好的读法是纯静态时，生成式的读法一概丢掉，只有静态表完全解释不了输入时才出场。
    /// 错拼表和生成对只算一次、全进程共用，所以这个方法用的就是 [`tables`] 生成的那一份，与 `self` 无关。
    pub fn cut_kbest(&self, raw: &str, types: u32, k: usize) -> Vec<Cut> {
        let _ = self;
        kbest(shared_index(), raw, types, k)
    }
}

/// 全进程共用错拼表上的 k-best，对应 C++ `quanpin::autocorrect_cut_kbest`。
pub fn autocorrect_cut_kbest(raw: &str, types: u32, k: usize) -> Vec<Cut> {
    kbest(shared_index(), raw, types, k)
}

fn kbest(index: &Index, raw: &str, types: u32, k: usize) -> Vec<Cut> {
    if k == 0 || types == 0 || raw.is_empty() || raw.len() > MAX_INPUT_LENGTH || raw.contains('\'') || !raw.is_ascii() {
        return Vec::new();
    }
    let length = raw.len();
    let mut best: Vec<Vec<Hypothesis>> = (0..=length).map(|_| Vec::new()).collect();
    best[0].push(Hypothesis { edges: 0, weight: 0, arrival: 0, prev: None, segment: None, has_generated: false, sequence: Vec::new() });
    let mut arrival = 0usize;

    let finalize = |list: &mut Vec<Hypothesis>| {
        // 含生成式边的读法排在所有静态读法之后，不管边数。
        list.sort_by(|a, b| (a.has_generated, a.edges, a.weight, a.arrival).cmp(&(b.has_generated, b.edges, b.weight, b.arrival)));
        let mut seen = std::collections::HashSet::new();
        list.retain(|h| seen.insert(h.sequence.clone()));
        list.truncate(k);
    };

    for start in 0..length {
        finalize(&mut best[start]);
        if best[start].is_empty() {
            continue;
        }
        let longest = (length - start).min(6);
        for len in (1..=longest).rev() {
            let piece = &raw[start..start + len];
            let end = start + len;
            // (音节, 是否纠正, 是否生成式, 权重)
            let mut edges: Vec<(String, bool, bool, i32)> = Vec::new();
            if INTACT.contains(&piece) {
                edges.push((piece.to_string(), false, false, 0));
            } else if let Some(targets) = index.get(piece) {
                for target in targets {
                    let enabled = types & target.types;
                    if enabled != 0 {
                        let weight = if target.generated { WEIGHT_GENERATED } else { cheapest_weight(enabled) };
                        edges.push((target.syllable.clone(), true, target.generated, weight));
                    }
                }
            } else {
                continue;
            }
            for (syllable, corrected, generated, weight) in edges {
                for parent_index in 0..best[start].len() {
                    let parent = &best[start][parent_index];
                    let child_edges = parent.edges + usize::from(corrected);
                    if child_edges > MAX_EDGES || (end == length && child_edges == 0) {
                        continue;
                    }
                    let mut sequence = parent.sequence.clone();
                    sequence.push(syllable.clone());
                    let child = Hypothesis {
                        edges: child_edges,
                        weight: parent.weight + weight,
                        arrival,
                        prev: Some(parent_index),
                        segment: Some(Segment { syllable: syllable.clone(), raw: piece.to_string(), start, corrected, weight }),
                        has_generated: parent.has_generated || generated,
                        sequence,
                    };
                    arrival += 1;
                    best[end].push(child);
                }
            }
        }
    }
    finalize(&mut best[length]);
    if best[length].first().is_some_and(|h| !h.has_generated) {
        best[length].retain(|h| !h.has_generated);
    }

    // 沿前驱往回走，还原每条读法的各段。
    let mut results = Vec::new();
    for final_hypothesis in &best[length] {
        let mut segments = Vec::new();
        let (mut position, mut current) = (length, final_hypothesis);
        while let Some(prev) = current.prev {
            let segment = current.segment.clone().unwrap();
            position -= segment.raw.len();
            segments.push(segment);
            current = &best[position][prev];
        }
        segments.reverse();
        results.push(Cut { segments, edges: final_hypothesis.edges, weight: final_hypothesis.weight, has_generated: final_hypothesis.has_generated });
    }
    results
}

// -------------------------------------------------------------------------------------------------
// 合法输入上的纠错、读音对回敲的字母、简拼形状
// -------------------------------------------------------------------------------------------------

/// 罕见合法音节 → 它的换位常用音节。人工收录：两侧都常用的换位对（hou/huo、bei/bie）放开后每个按键
/// 都要多解码几条读法。
const RARE_LEGAL_TRANSPOSITIONS: [(&str, &str); 2] = [("lia", "lai"), ("dia", "dai")];
/// 合法输入上一条读法最多带几处纠正。
const MAX_LEGAL_INPUT_CORRECTIONS: usize = 2;
/// 合法输入上认的纠错类型：换位、多字、漏字。邻键替换不认：合法输入里几乎每个字母都有邻键能换成另一个合法读法。
const LEGAL_INPUT_CORRECTION_TYPES: u32 = TRANSPOSITION | DELETION | INSERTION;
const LEGAL_INPUT_SEARCH_K: usize = 9;
const READING_ALIGNMENT_K: usize = 9;

fn is_zero_initial_syllable(syllable: &str) -> bool {
    matches!(syllable.as_bytes().first(), Some(b'a' | b'o' | b'e'))
}

/// 合法输入上的手误读法：字面切分全是合法音节时，列出纠错表在原始字母上给出的读法。只负责列出，不判断哪条对。
///
/// - 静态表（换位、多字、漏字，不含邻键与生成式）的 k-best 读法，至多 [`MAX_LEGAL_INPUT_CORRECTIONS`] 处纠正，
///   且纠正的那段必须盖住字面切分里某个非首位的零声母音节（挤出或多出的元音是合法输入上手误的标志）；
/// - 罕见合法音节换位成常用音节（`lia` → `lai`），只在多音节输入里认。
///
/// `segments` 必须是 `pinyin` 的逐字母切分，否则返回空。
pub fn legal_input_correction_cuts(pinyin: &str, segments: &[String], autocorrect_types: u32, k: usize) -> Vec<Cut> {
    let types = autocorrect_types & LEGAL_INPUT_CORRECTION_TYPES;
    if k == 0 || types == 0 || segments.is_empty() || pinyin.contains('\'') || !crate::quanpin_query::has_only_complete_pinyin_segments(segments) {
        return Vec::new();
    }
    let mut zero_initial_spans: Vec<(usize, usize)> = Vec::new();
    let mut letters = String::new();
    for (i, segment) in segments.iter().enumerate() {
        if i > 0 && is_zero_initial_syllable(segment) {
            zero_initial_spans.push((letters.len(), letters.len() + segment.len()));
        }
        letters += segment;
    }
    if letters != pinyin {
        return Vec::new();
    }

    let mut cuts: Vec<Cut> = Vec::new();
    let add_cut = |cuts: &mut Vec<Cut>, cut: Cut| {
        if !cuts.iter().any(|existing| existing.syllables() == cut.syllables()) {
            cuts.push(cut);
        }
    };

    if !zero_initial_spans.is_empty() {
        for cut in kbest(shared_index(), pinyin, types, k.max(LEGAL_INPUT_SEARCH_K)) {
            let covers_zero_initial = cut.segments.iter().any(|segment| {
                let end = segment.start + segment.raw.len();
                segment.corrected && zero_initial_spans.iter().any(|span| segment.start < span.1 && span.0 < end)
            });
            // 末尾音节是按漏字读出来的（nia → nian）：多半是还没打完，不是手误。
            let tail = cut.segments.last().unwrap();
            let unfinished_tail = tail.corrected && tail.raw.len() < tail.syllable.len();
            if covers_zero_initial && !unfinished_tail && !cut.has_generated && cut.edges <= MAX_LEGAL_INPUT_CORRECTIONS {
                add_cut(&mut cuts, cut);
            }
        }
    }

    if types & TRANSPOSITION != 0 && segments.len() >= 2 {
        let mut replaced = Cut::default();
        let mut start = 0;
        for syllable in segments {
            let mut segment = Segment { syllable: syllable.clone(), raw: syllable.clone(), start, corrected: false, weight: 0 };
            if let Some((_, common)) = RARE_LEGAL_TRANSPOSITIONS.iter().find(|(rare, _)| rare == syllable) {
                segment.syllable = (*common).to_string();
                segment.corrected = true;
                segment.weight = WEIGHT_TRANSPOSITION;
                replaced.edges += 1;
                replaced.weight += WEIGHT_TRANSPOSITION;
            }
            start += syllable.len();
            replaced.segments.push(segment);
        }
        if replaced.edges > 0 && replaced.edges <= MAX_LEGAL_INPUT_CORRECTIONS {
            add_cut(&mut cuts, replaced);
        }
    }

    cuts.truncate(k);
    cuts
}

/// 纠错切分里音节前缀与 `reading` 相同的那条（k-best 次序里最靠前的）。它的各段 `start`/`raw` 就是读音每个音节
/// 实际盖住的原始区间。
pub fn correction_cut_for_reading(raw_letters: &str, reading: &[String], autocorrect_types: u32) -> Option<Cut> {
    if reading.is_empty() {
        return None;
    }
    kbest(shared_index(), raw_letters, autocorrect_types, READING_ALIGNMENT_K)
        .into_iter()
        .find(|cut| cut.segments.len() >= reading.len() && reading.iter().zip(&cut.segments).all(|(syllable, segment)| *syllable == segment.syllable))
}

/// 选中的读音来自纠错切分时，它在原始输入里实际盖住了几个字母。整串是合法拼音且读音就是原始字母前缀、
/// 或找不到对应切分时返回 `None`，调用方照旧按读音长度消耗。`raw_letters` 是不含分隔符的原始字母。
pub fn corrected_reading_raw_length(raw_letters: &str, reading: &[String], autocorrect_types: u32) -> Option<usize> {
    let reading_letters: String = reading.concat();
    if reading.is_empty()
        || (raw_letters.starts_with(&reading_letters)
            && (crate::quanpin_query::is_complete_pinyin_input(raw_letters) || looks_like_jianpin_spelling(raw_letters)))
    {
        return None;
    }
    let cut = correction_cut_for_reading(raw_letters, reading, autocorrect_types)?;
    let last = &cut.segments[reading.len() - 1];
    Some(last.start + last.raw.len())
}

/// k=1 的投影：最便宜的一条读法，没有就是空的 [`Cut`]。
pub fn autocorrect_cut_detail(pinyin: &str, autocorrect_types: u32) -> Cut {
    kbest(shared_index(), pinyin, autocorrect_types, 1).into_iter().next().unwrap_or_default()
}

/// 前导声母加完整音节（`ywen` = y + wen），或若干完整音节加至多一个末尾字母（`zheg` = zhe + g）：
/// 这些简拼意图的形状不是手误。三个字母以上的全辅音串不算（`bqng` → `bang` 只有纠错一种读法）。
/// 含手动分隔符返回假。调用方负责在进纠错之前先过这道闸，搜索本身不看它。
pub fn looks_like_jianpin_spelling(pinyin: &str) -> bool {
    use crate::quanpin_query::is_complete_pinyin_input;
    use crate::syllables::{is_intact, is_prefix};
    if pinyin.is_empty() || pinyin.contains('\'') {
        return false;
    }
    let Some(initial) = pinyin.get(..1) else { return false };
    if is_prefix(initial) && !is_intact(initial) && is_complete_pinyin_input(&pinyin[1..]) {
        return true;
    }
    if is_complete_pinyin_input(pinyin) {
        return true;
    }
    if pinyin.len() < 2 {
        return false;
    }
    let Some(head) = pinyin.get(..pinyin.len() - 1) else { return false };
    if !is_complete_pinyin_input(head) {
        return false;
    }
    // 末尾那个字母必须像简拼声母；单独的元音更像换位手误（gau = ga + u → gua）。
    !matches!(pinyin.as_bytes()[pinyin.len() - 1], b'a' | b'e' | b'i' | b'o' | b'u' | b'v')
}

impl Tables {
    /// 一个错拼在打开的纠错类型里能改成哪些音节，去重、按字母序。
    pub fn corrections(&self, wrong: &str, types: u32) -> Vec<String> {
        let mut found: BTreeSet<&String> = BTreeSet::new();
        for (bit, table) in [
            (TRANSPOSITION, &self.transposition),
            (NEIGHBOR, &self.neighbor),
            (DELETION, &self.deletion),
            (INSERTION, &self.insertion),
        ] {
            if types & bit != 0 {
                if let Some(corrects) = table.get(wrong) {
                    found.extend(corrects);
                }
            }
        }
        found.into_iter().cloned().collect()
    }
}

// -------------------------------------------------------------------------------------------------
// 长句容错槽位模型
// -------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FaultSlotKind {
    /// 合法音节：读音固定。
    Determining,
    /// 单个字母：简拼，可展开成任一以它开头的音节。
    SingleInitial,
    /// 多个字母的非法串：只能经纠错表读成合法音节。
    Fault,
}

#[derive(Debug, Clone, PartialEq)]
pub struct FaultSlot {
    pub raw_text: String,
    pub base_syllable: String,
    pub kind: FaultSlotKind,
}

/// 短串（槽数不超过它）归 k-best 的既有势力范围。
pub const MAX_SHORT_AUTOCORRECT_SYLLABLES: usize = 4;
pub const MAX_FAULT_SLOT_COUNT: usize = 20;

fn fault_slot_kind(piece: &str) -> FaultSlotKind {
    if INTACT.contains(&piece) {
        FaultSlotKind::Determining
    } else if piece.len() == 1 {
        FaultSlotKind::SingleInitial
    } else {
        FaultSlotKind::Fault
    }
}

/// 把敲出的字母切成容错槽位：切分与查询层同一个切分器（残余按简拼尾解释），再按 intact 集分类。
/// 带分隔符、含非字母的串没有槽位。对应 C++ `build_fault_slots`。
pub fn build_fault_slots(pinyin: &str) -> Vec<FaultSlot> {
    if pinyin.is_empty() || pinyin.contains('\'') {
        return Vec::new();
    }
    let mut letters = String::with_capacity(pinyin.len());
    for byte in pinyin.bytes() {
        let lower = byte.to_ascii_lowercase();
        if !lower.is_ascii_lowercase() {
            return Vec::new();
        }
        letters.push(lower as char);
    }
    let Some(segments) = crate::segment::cut_min_segments_prefix(&letters) else { return Vec::new() };
    if segments.is_empty() || segments.concat() != letters {
        return Vec::new();
    }
    segments
        .into_iter()
        .map(|segment| {
            let kind = fault_slot_kind(&segment);
            // Fault 槽没有基础读法：多字母非法串不能当简拼查词。
            let base_syllable = if kind == FaultSlotKind::Fault { String::new() } else { segment.clone() };
            FaultSlot { raw_text: segment, base_syllable, kind }
        })
        .collect()
}

/// 一个槽的纠错表目标（音节, 权重），按启用的类型位过滤；已是合法音节的串没有目标。
/// 权重按启用位折算，生成式目标走昂贵档常数。对应 C++ `fault_slot_correction_targets`。
pub fn fault_slot_correction_targets(raw_text: &str, autocorrect_types: u32) -> Vec<(String, i32)> {
    if autocorrect_types == 0 || raw_text.is_empty() || INTACT.contains(&raw_text) {
        return Vec::new();
    }
    let Some(found) = shared_index().get(raw_text) else { return Vec::new() };
    let mut targets: Vec<(String, i32)> = Vec::new();
    for target in found {
        let enabled = autocorrect_types & target.types;
        if enabled == 0 {
            continue;
        }
        let weight = if target.generated { WEIGHT_GENERATED } else { cheapest_weight(enabled) };
        match targets.iter_mut().find(|entry| entry.0 == target.syllable) {
            None => targets.push((target.syllable.clone(), weight)),
            Some(existing) if weight < existing.1 => existing.1 = weight,
            Some(_) => {}
        }
    }
    targets
}

/// 容错仲裁是否接管：至少两个单字母槽、有非 Determining 槽，且槽数超过短串上限（不超过 20）。
pub fn fault_slots_eligible(slots: &[FaultSlot], autocorrect_types: u32) -> bool {
    if autocorrect_types == 0 || slots.is_empty() || slots.len() > MAX_FAULT_SLOT_COUNT {
        return false;
    }
    let single_initial = slots.iter().filter(|slot| slot.kind == FaultSlotKind::SingleInitial).count();
    let non_determining = slots.iter().filter(|slot| slot.kind != FaultSlotKind::Determining).count();
    single_initial >= 2 && non_determining > 0 && slots.len() > MAX_SHORT_AUTOCORRECT_SYLLABLES
}

/// 读音是槽原文删掉恰好一个字母得到的（yang → yan/yag）。
pub fn is_deletion_variant(raw_text: &str, reading: &str) -> bool {
    let (raw, reading) = (raw_text.as_bytes(), reading.as_bytes());
    if reading.len() + 1 != raw.len() {
        return false;
    }
    let mut r = 0;
    let mut skipped = 0;
    for &ch in raw {
        if skipped == 0 && (r >= reading.len() || ch != reading[r]) {
            skipped = 1;
            continue;
        }
        if r < reading.len() && reading[r] == ch {
            r += 1;
            continue;
        }
        return false;
    }
    skipped == 1 && r == reading.len()
}

/// 容错仲裁的读音对回敲的字母：按槽位模型逐槽核对，返回读音盖住的字母数。对应 C++ `fault_reading_raw_length`。
pub fn fault_reading_raw_length(raw_letters: &str, reading: &[String]) -> Option<usize> {
    let slots = build_fault_slots(raw_letters);
    if slots.is_empty() || reading.len() > slots.len() || reading.is_empty() {
        return None;
    }
    let mut consumed = 0;
    for (slot, syllable) in slots.iter().zip(reading) {
        // 槽盖住的原始字母必须被读音逐字母消耗（允许 ü 归一）。
        let matches_slot = *syllable == slot.base_syllable
            || (syllable.len() == slot.raw_text.len() && canonical_umlaut_spelling(&slot.raw_text) == canonical_umlaut_spelling(syllable));
        match slot.kind {
            FaultSlotKind::Determining => {
                if !matches_slot && !is_deletion_variant(&slot.raw_text, syllable) {
                    return None;
                }
            }
            FaultSlotKind::SingleInitial => {
                if !matches_slot && (syllable.is_empty() || syllable.as_bytes()[0] != slot.raw_text.as_bytes()[0]) {
                    return None;
                }
            }
            FaultSlotKind::Fault => {
                if syllable.is_empty() || !INTACT.contains(&syllable.as_str()) {
                    return None;
                }
            }
        }
        consumed += slot.raw_text.len();
    }
    Some(consumed)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn each_kind_of_slip_maps_back_to_the_syllable() {
        let t = tables();
        assert_eq!(t.corrections("sahng", TRANSPOSITION), ["shang"]);
        assert_eq!(t.corrections("shabg", NEIGHBOR), ["shang"]);
        assert!(t.corrections("shng", DELETION).contains(&"shang".to_string()));
        // 连击不是多字纠错：只收相邻键的 QWERTY 邻键。
        assert!(!t.corrections("shangg", INSERTION).contains(&"shang".to_string()));
        assert!(t.corrections("sjhang", INSERTION).contains(&"shang".to_string()));
    }

    #[test]
    fn a_slip_only_counts_when_its_type_is_on() {
        let t = tables();
        // 换位关掉后，sahng 只剩“sang 多敲了 h”这一种读法，不再有 shang。
        let readings = t.corrections("sahng", NEIGHBOR | DELETION | INSERTION);
        assert!(!readings.contains(&"shang".to_string()), "{readings:?}");
        assert!(t.corrections("sahng", 0).is_empty());
    }

    #[test]
    fn ambiguous_slips_keep_every_reading() {
        let t = tables();
        // 漏掉一个字母可以是 shang，也可以是 sheng：全部保留，交给后面的切分和词频裁决。
        assert_eq!(t.corrections("shng", DELETION), ["shang", "sheng"]);
    }

    #[test]
    fn two_letter_strings_and_legal_syllables_are_never_keys() {
        let t = tables();
        for table in [&t.transposition, &t.neighbor, &t.deletion, &t.insertion] {
            assert!(table.keys().all(|k| k.len() >= 3 && k.len() <= 6));
            assert!(table.keys().all(|k| !INTACT.contains(&k.as_str())));
        }
        assert!(t.corrections("wj", 15).is_empty());
    }

    fn reading(cut: &Cut) -> String {
        cut.syllables().join("'")
    }

    // 以下断言都来自 C++ 的 test_pinyin.cpp，是同一批输入的同一个结果。
    #[test]
    fn kbest_matches_the_cpp_expectations() {
        let t = tables();
        let both = TRANSPOSITION | NEIGHBOR;
        let all = both | DELETION;
        let all_four = all | INSERTION;

        let ahan = t.cut_kbest("ahan", both, 9);
        assert!(ahan.len() >= 2);
        assert_eq!((reading(&ahan[0]), reading(&ahan[1])), ("shan".into(), "zhan".into()));

        let shng = t.cut_kbest("shng", all, 9);
        assert_eq!(reading(&shng[0]), "shang");
        assert!(shng.iter().any(|c| reading(c) == "sheng"));

        // 同一处纠正里，换位（10）要比拆成两段的漏键（11）便宜。
        let sahng = t.cut_kbest("sahng", all, 9);
        assert_eq!((reading(&sahng[0]), reading(&sahng[1])), ("shang".into(), "sa'hang".into()));

        let gau = t.cut_kbest("gau", both, 9);
        assert_eq!(reading(&gau[0]), "gua");
        let gai = gau.iter().find(|c| reading(c) == "gai").expect("gau 要保留邻键读法 gai");
        assert!(gau[0].weight < gai.weight);

        assert!(t.cut_kbest("shangg", INSERTION, 9).is_empty());
        assert_eq!(reading(&t.cut_kbest("sjhang", INSERTION, 9)[0]), "shang");
        assert!(t.cut_kbest("sjhang", TRANSPOSITION, 9).is_empty());
        assert!(t.cut_kbest("sjhang", DELETION, 9).is_empty());
        assert!(t.cut_kbest("sjhang", NEIGHBOR, 9).is_empty());
        assert_eq!(reading(&t.cut_kbest("sjhangzhi", all_four, 9)[0]), "shang'zhi");
    }

    #[test]
    fn kbest_rejects_inputs_it_must_leave_alone() {
        let t = tables();
        // 合法输入：只列出带纠正的读法，原样的 ni'hao 不在里面。
        for cut in t.cut_kbest("nihao", 15, 9) {
            assert!(cut.edges >= 1 && cut.segments.iter().any(|s| s.corrected));
            assert_ne!(cut.syllables(), ["ni", "hao"]);
        }
        assert!(t.cut_kbest("nihoa", 0, 9).is_empty());
        assert!(t.cut_kbest("ni'hoa", 15, 9).is_empty());
        assert!(t.cut_kbest("", 15, 9).is_empty());
        assert!(t.cut_kbest("nihoa", 15, 0).is_empty());
    }

    #[test]
    fn kbest_keeps_the_raw_letters_each_segment_consumed() {
        let t = tables();
        let cut = &t.cut_kbest("sjhang", INSERTION, 1)[0];
        assert_eq!(cut.segments.len(), 1);
        assert_eq!((cut.segments[0].raw.as_str(), cut.segments[0].syllable.as_str()), ("sjhang", "shang"));
        assert_eq!((cut.edges, cut.weight), (1, WEIGHT_INSERTION));
    }
}
