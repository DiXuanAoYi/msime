//! 全拼手误纠错的“错拼 → 正确音节”表。
//!
//! C++ 那边是脚本 `server/scripts/generate_quanpin_autocorrect.py` 生成的 30 万字节头文件
//! `engine/quanpin/autocorrect_table.h`。这里不抄那份数据，而是把同样的生成规则用 Rust 写一遍，
//! 启动时现算；`tests/autocorrect_table_matches_cpp.rs` 会把算出来的四张表和那份头文件逐行比对，
//! 规则一旦和 C++ 对不上就红。
//!
//! 四种手误各自可以单独开关，位值和 `quanpin_utils.h` 的 `kAutocorrect*` 一致：
//! 换位（sahng → shang）、邻键（shabg → shang）、漏键（shng → shang）、多键（shangg、sjhang → shang）。
//! 一个错拼可以对应多个正确音节（漏键的 `shng` → `shang`、`sheng`），全部保留，由后面的切分和词频裁决。
//! 长度不足 3 的串不收：两个字母是简拼的地盘，`wj` 只能是 w+j，不能被改成 `wu`。

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

/// 插入一个字母：必须像真的手滑，也就是相邻键的重复（连击）或相邻键的邻键。
fn insertion_variants(syllable: &str) -> Vec<String> {
    let chars: Vec<char> = syllable.chars().collect();
    let mut out = Vec::new();
    for position in 0..=chars.len() {
        let mut letters: BTreeSet<char> = BTreeSet::new();
        if position > 0 {
            let left = chars[position - 1];
            letters.insert(left);
            letters.extend(neighbors(left));
        }
        if position < chars.len() {
            let right = chars[position];
            letters.insert(right);
            letters.extend(neighbors(right));
        }
        for letter in letters {
            let mut inserted = chars.clone();
            inserted.insert(position, letter);
            out.push(inserted.into_iter().collect());
        }
    }
    out
}

fn build(make_variants: fn(&str) -> Vec<String>) -> Table {
    let mut table = Table::new();
    for syllable in INTACT {
        if syllable.len() < 2 {
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
    Tables {
        transposition: build(transposition_variants),
        neighbor: build(neighbor_variants),
        deletion: build(deletion_variants),
        insertion: build(insertion_variants),
    }
}

/// 单个纠错边的代价，和 `quanpin_utils.h` 的 `kAutocorrect*Weight` 一致：换位最便宜，邻键最贵。
pub const WEIGHT_TRANSPOSITION: i32 = 10;
pub const WEIGHT_DELETION: i32 = 11;
pub const WEIGHT_INSERTION: i32 = 12;
pub const WEIGHT_NEIGHBOR: i32 = 13;

/// 一条切分里最多几处纠正；再多几乎都是凑巧拼出来的读法。
pub const MAX_EDGES: usize = 3;
const MAX_INPUT_LENGTH: usize = 64;

/// 某个错拼的一个目标音节，`types` 是提供这对（错拼, 音节）的所有纠错类型。
#[derive(Debug, Clone, PartialEq)]
pub struct Target {
    pub syllable: String,
    pub types: u32,
}

/// 切分里的一段。
#[derive(Debug, Clone, PartialEq)]
pub struct Segment {
    pub syllable: String,
    /// 对应敲出的字母；漏键比音节少一个字母，多键多一个。
    pub raw: String,
    pub corrected: bool,
    pub weight: i32,
}

/// 一种带纠正的切分。
#[derive(Debug, Clone, PartialEq)]
pub struct Cut {
    pub segments: Vec<Segment>,
    pub edges: usize,
    pub weight: i32,
}

impl Cut {
    pub fn syllables(&self) -> Vec<String> {
        self.segments.iter().map(|s| s.syllable.clone()).collect()
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
    /// 到这里为止的音节序列，同一序列只留排名最靠前的一条。
    sequence: Vec<String>,
}

impl Tables {
    /// 错拼 → 目标音节，目标按 换位、漏键、多键、邻键 的顺序排列，同表内按字母序；
    /// 这是 C++ 并列时的“表序”，k-best 的次序靠它。
    fn index(&self) -> std::collections::HashMap<&str, Vec<Target>> {
        let mut index: std::collections::HashMap<&str, Vec<Target>> = std::collections::HashMap::new();
        for (bit, table) in [
            (TRANSPOSITION, &self.transposition),
            (DELETION, &self.deletion),
            (INSERTION, &self.insertion),
            (NEIGHBOR, &self.neighbor),
        ] {
            for (wrong, corrects) in table {
                let targets = index.entry(wrong.as_str()).or_default();
                for correct in corrects {
                    match targets.iter_mut().find(|t| &t.syllable == correct) {
                        Some(existing) => existing.types |= bit,
                        None => targets.push(Target { syllable: correct.clone(), types: bit }),
                    }
                }
            }
        }
        index
    }

    /// 把敲出的字母切成音节，允许最多 [`MAX_EDGES`] 处纠正，返回最好的 `k` 种读法，最便宜的在前
    /// （排序键：纠正边数，再是权重和，最后是产生的先后）。每种读法至少带一处纠正；
    /// 本来就是合法切分、又没有纠错读法的输入返回空。对应 C++ `quanpin::autocorrect_cut_kbest`
    /// 的静态表部分——表外的“生成读法”和合法输入上的纠错闸还在 C++。
    pub fn cut_kbest(&self, raw: &str, types: u32, k: usize) -> Vec<Cut> {
        if k == 0 || types == 0 || raw.is_empty() || raw.len() > MAX_INPUT_LENGTH || raw.contains('\'') || !raw.is_ascii() {
            return Vec::new();
        }
        let index = self.index();
        let length = raw.len();
        let mut best: Vec<Vec<Hypothesis>> = (0..=length).map(|_| Vec::new()).collect();
        best[0].push(Hypothesis { edges: 0, weight: 0, arrival: 0, prev: None, segment: None, sequence: Vec::new() });
        let mut arrival = 0usize;

        let finalize = |list: &mut Vec<Hypothesis>| {
            list.sort_by(|a, b| (a.edges, a.weight, a.arrival).cmp(&(b.edges, b.weight, b.arrival)));
            let mut seen = std::collections::HashSet::new();
            list.retain(|h| seen.insert(h.sequence.clone()));
            list.truncate(k);
        };

        for start in 0..length {
            // 没有前驱的位置不出边；到了这里 start 位置的列表已经收齐，排名后冻结。
            finalize(&mut best[start]);
            if best[start].is_empty() {
                continue;
            }
            let longest = (length - start).min(6);
            for len in (1..=longest).rev() {
                let piece = &raw[start..start + len];
                let end = start + len;
                // (音节, 是否纠正, 权重)
                let mut edges: Vec<(String, bool, i32)> = Vec::new();
                if INTACT.contains(&piece) {
                    edges.push((piece.to_string(), false, 0));
                } else if let Some(targets) = index.get(piece) {
                    for target in targets {
                        let enabled = types & target.types;
                        if enabled != 0 {
                            edges.push((target.syllable.clone(), true, cheapest_weight(enabled)));
                        }
                    }
                } else {
                    continue;
                }
                for (syllable, corrected, weight) in edges {
                    for parent_index in 0..best[start].len() {
                        let parent = &best[start][parent_index];
                        let child_edges = parent.edges + usize::from(corrected);
                        if child_edges > MAX_EDGES || (end == length && child_edges == 0) {
                            continue;
                        }
                        // 纠错边消耗的字母数由 piece 决定，音节长度可能多一个或少一个。
                        let mut sequence = parent.sequence.clone();
                        sequence.push(syllable.clone());
                        let child = Hypothesis {
                            edges: child_edges,
                            weight: parent.weight + weight,
                            arrival,
                            prev: Some(parent_index),
                            segment: Some(Segment { syllable: syllable.clone(), raw: piece.to_string(), corrected, weight }),
                            sequence,
                        };
                        arrival += 1;
                        best[end].push(child);
                    }
                }
            }
        }
        finalize(&mut best[length]);

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
            results.push(Cut { segments, edges: final_hypothesis.edges, weight: final_hypothesis.weight });
        }
        results
    }

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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn each_kind_of_slip_maps_back_to_the_syllable() {
        let t = tables();
        assert_eq!(t.corrections("sahng", TRANSPOSITION), ["shang"]);
        assert_eq!(t.corrections("shabg", NEIGHBOR), ["shang"]);
        assert!(t.corrections("shng", DELETION).contains(&"shang".to_string()));
        assert!(t.corrections("shangg", INSERTION).contains(&"shang".to_string()));
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

        assert_eq!(reading(&t.cut_kbest("shangg", INSERTION, 9)[0]), "shang");
        assert!(t.cut_kbest("shangg", TRANSPOSITION, 9).is_empty());
        assert!(t.cut_kbest("shangg", DELETION, 9).is_empty());
        assert!(t.cut_kbest("shangg", NEIGHBOR, 9).is_empty());
        assert_eq!(reading(&t.cut_kbest("sjhang", INSERTION, 9)[0]), "shang");
        assert!(t.cut_kbest("sjhang", NEIGHBOR, 9).is_empty());
        assert_eq!(reading(&t.cut_kbest("shanggzhi", all_four, 9)[0]), "shang'zhi");
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
        let cut = &t.cut_kbest("shangg", INSERTION, 1)[0];
        assert_eq!(cut.segments.len(), 1);
        assert_eq!((cut.segments[0].raw.as_str(), cut.segments[0].syllable.as_str()), ("shangg", "shang"));
        assert_eq!((cut.edges, cut.weight), (1, WEIGHT_INSERTION));
    }
}
