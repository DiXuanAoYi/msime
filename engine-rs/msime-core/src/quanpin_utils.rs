//! 全拼输入的小判断与枚举，移植自 `engine/quanpin/quanpin_utils.cpp`：混合简拼/纯简拼的字面匹配、音节图与
//! 它的完整切分枚举、最少音节数打平的切分枚举、稀疏拼音回退，以及 ü 别名归一。
//!
//! 纠错那一块（`autocorrect_cut_kbest`、`legal_input_correction_cuts`、`looks_like_jianpin_spelling` 等）不在这里。

use crate::quanpin_query::{split_segments, Segments};
use crate::syllables::{is_intact, is_prefix, INTACT};

/// 字面混合简拼的匹配程度。输入不带分隔符，词库行的读音是 `code`（敲的）、`canonical`（完整音节）：
/// 返回 1 表示整串都对上（至少一个完整音节加至少一个声母），2 表示整串加一个没打完的尾巴，0 表示不是。
pub fn literal_mixed_pinyin_match(raw: &str, code: &str, canonical: &str) -> u32 {
    if raw.contains('\'') || canonical.is_empty() {
        return 0;
    }
    let raw_bytes = raw.as_bytes();
    let mut consumed = 0;
    for ch in code.bytes() {
        if ch != b'\'' {
            if consumed == raw_bytes.len() || raw_bytes[consumed] != ch {
                return 0;
            }
            consumed += 1;
        }
    }
    let Some(tail) = raw.get(consumed..) else { return 0 };
    if !tail.is_empty() && (tail.len() > 2 || !is_prefix(tail) || is_intact(tail)) {
        return 0;
    }
    let typed = split_segments(code);
    let reading = split_segments(canonical);
    if typed.len() != reading.len() {
        return 0;
    }
    let (mut full, mut initial) = (false, false);
    for (typed, reading) in typed.iter().zip(&reading) {
        if !is_intact(reading) {
            return 0;
        }
        if typed == reading {
            full = true;
        } else if typed.len() == 1 && !reading.is_empty() && typed.as_bytes()[0] == reading.as_bytes()[0] && !is_intact(typed) {
            initial = true;
        } else {
            return 0;
        }
    }
    if full && initial {
        if tail.is_empty() {
            1
        } else {
            2
        }
    } else {
        0
    }
}

/// 纯简拼：词库行每个音节都只敲了声母，连起来就是输入。
pub fn literal_jianpin_pinyin_match(raw: &str, code: &str, canonical: &str) -> bool {
    if raw.contains('\'') || canonical.is_empty() {
        return false;
    }
    let typed = split_segments(code);
    let reading = split_segments(canonical);
    if typed.len() < 2 || typed.len() != reading.len() {
        return false;
    }
    let mut letters = String::new();
    for (typed, reading) in typed.iter().zip(&reading) {
        if typed.len() != 1 || !is_prefix(typed) || is_intact(typed) || !is_intact(reading) || reading.as_bytes()[0] != typed.as_bytes()[0] {
            return false;
        }
        letters += typed;
    }
    letters == raw
}

/// 音节图：`edges[start]` 是从 `start` 出发、之后还能走到末尾的完整音节，按终点从远到近排。
pub struct SyllableGraph {
    input_length: usize,
    edges: Vec<Vec<(usize, &'static str)>>,
}

fn max_syllable_length() -> usize {
    INTACT.iter().map(|syllable| syllable.len()).max().unwrap_or(0)
}

pub fn build_syllable_graph(pinyin: &str) -> SyllableGraph {
    let length = pinyin.len();
    let mut graph = SyllableGraph { input_length: length, edges: vec![Vec::new(); length + 1] };
    if pinyin.is_empty() || pinyin.contains('\'') {
        return graph;
    }
    let longest = max_syllable_length();
    for start in 0..length {
        let last_end = length.min(start + longest);
        for end in (start + 1..=last_end).rev() {
            let Some(piece) = pinyin.get(start..end) else { continue };
            if let Some(syllable) = INTACT.iter().find(|syllable| **syllable == piece) {
                graph.edges[start].push((end, *syllable));
            }
        }
    }
    let mut reaches_end = vec![false; length + 1];
    reaches_end[length] = true;
    for start in (0..length).rev() {
        graph.edges[start].retain(|(end, _)| *end <= length && reaches_end[*end]);
        reaches_end[start] = !graph.edges[start].is_empty();
    }
    graph
}

/// 音节图里所有能走到末尾的路径，最多 `path_limit` 条（深度优先，先走远的边）。
pub fn enumerate_complete_segmentations(graph: &SyllableGraph, path_limit: usize) -> Vec<Segments> {
    let mut result = Vec::new();
    if path_limit == 0 || graph.input_length == 0 || graph.edges.len() != graph.input_length + 1 {
        return result;
    }
    fn walk(graph: &SyllableGraph, position: usize, current: &mut Segments, result: &mut Vec<Segments>, limit: usize) {
        if result.len() >= limit {
            return;
        }
        if position == graph.input_length {
            result.push(current.clone());
            return;
        }
        if position >= graph.edges.len() {
            return;
        }
        for (end, syllable) in &graph.edges[position] {
            current.push((*syllable).to_string());
            walk(graph, *end, current, result, limit);
            current.pop();
            if result.len() >= limit {
                return;
            }
        }
    }
    walk(graph, 0, &mut Vec::new(), &mut result, path_limit);
    result
}

/// 所有“音节数最少”的切分（等音节数打平的几种），最多 `path_limit` 条。
pub fn enumerate_min_segmentations(pinyin: &str, path_limit: usize) -> Vec<Segments> {
    let mut result = Vec::new();
    if pinyin.is_empty() || path_limit == 0 {
        return result;
    }
    const MAX_SYLLABLE_LENGTH: usize = 6;
    let length = pinyin.len();
    // min_count[i]：从 i 切到末尾最少几个音节。只沿“这一刀之后仍是最少音节”的边往下走。
    let mut min_count: Vec<Option<usize>> = vec![None; length + 1];
    min_count[length] = Some(0);
    for start in (0..length).rev() {
        for len in 1..=MAX_SYLLABLE_LENGTH {
            if start + len > length {
                break;
            }
            if let (Some(next), Some(piece)) = (min_count[start + len], pinyin.get(start..start + len)) {
                if is_intact(piece) {
                    min_count[start] = Some(min_count[start].map_or(next + 1, |best| best.min(next + 1)));
                }
            }
        }
    }
    if min_count[0].is_none() {
        return result;
    }
    fn walk(pinyin: &str, min_count: &[Option<usize>], start: usize, current: &mut Segments, result: &mut Vec<Segments>, limit: usize) {
        if result.len() >= limit {
            return;
        }
        if start == pinyin.len() {
            result.push(current.clone());
            return;
        }
        for len in 1..=MAX_SYLLABLE_LENGTH {
            if start + len > pinyin.len() {
                break;
            }
            let Some(next) = min_count[start + len] else { continue };
            if Some(next + 1) != min_count[start] {
                continue;
            }
            let Some(piece) = pinyin.get(start..start + len) else { continue };
            if !is_intact(piece) {
                continue;
            }
            current.push(piece.to_string());
            walk(pinyin, min_count, start + len, current, result, limit);
            current.pop();
        }
    }
    walk(pinyin, &min_count, 0, &mut Vec::new(), &mut result, path_limit);
    result
}

/// 词库里没有、但常被敲出来的音节，退回到相近的读法再查一遍：`dia` → `di'a` / `di`，`biang` → `bi'ang` / `bi`，
/// `gei` → `ge`，`yo` → `y`。后面的音节原样跟着（`append_suffix`）。
pub fn sparse_pinyin_fallback_segments(segments: &[String]) -> Vec<Segments> {
    struct Replacement {
        replacement: &'static [&'static str],
        append_suffix: bool,
    }
    const RULES: &[(&str, &[Replacement])] = &[
        ("dia", &[Replacement { replacement: &["di", "a"], append_suffix: true }, Replacement { replacement: &["di"], append_suffix: false }]),
        ("biang", &[Replacement { replacement: &["bi", "ang"], append_suffix: true }, Replacement { replacement: &["bi"], append_suffix: false }]),
        ("gei", &[Replacement { replacement: &["ge"], append_suffix: false }]),
        ("yo", &[Replacement { replacement: &["y"], append_suffix: false }]),
    ];
    let Some(first) = segments.first() else { return Vec::new() };
    for (full, replacements) in RULES {
        if first != full {
            continue;
        }
        return replacements
            .iter()
            .map(|entry| {
                let mut combined: Segments = entry.replacement.iter().map(|s| (*s).to_string()).collect();
                if entry.append_suffix {
                    combined.extend(segments.iter().skip(1).cloned());
                }
                combined
            })
            .collect();
    }
    Vec::new()
}

/// 正字法别名归一：把非标准 ü 拼写改写成词库的标准键。n/l 系的 `nue`/`lue` 改成 `nve`/`lve`，
/// j/q/x/y 系的 `v` 一律是 `u` 的别名（`jv` → `ju`）。`nu`/`lu`/`nv`/`lv` 是真实音节，不动。
pub fn normalize_umlaut_aliases(mut segments: Segments) -> Segments {
    for segment in &mut segments {
        let bytes = segment.as_bytes();
        if bytes.len() == 3 && bytes[1] == b'u' && bytes[2] == b'e' && (bytes[0] == b'n' || bytes[0] == b'l') {
            segment.replace_range(1..2, "v");
        } else if bytes.len() >= 2 && bytes[1] == b'v' && matches!(bytes[0], b'j' | b'q' | b'x' | b'y') {
            segment.replace_range(1..2, "u");
        }
    }
    segments
}

/// 纠错比较用的字母：去掉手动分隔符、转小写。`v` 和 `u` 当作不同的字母（别名改写正是纠错标记的来源之一）。
pub fn fold_letters(text: &str) -> String {
    text.chars().filter(|c| *c != '\'').map(|c| c.to_ascii_lowercase()).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(items: &[&str]) -> Vec<String> {
        items.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn min_segmentations_keep_only_ties() {
        let cuts = enumerate_min_segmentations("yibananzhao", 8);
        assert!(cuts.contains(&s(&["yi", "ban", "an", "zhao"])));
        assert!(cuts.contains(&s(&["yi", "ba", "nan", "zhao"])));
        assert!(cuts.iter().all(|cut| cut.len() == 4));
    }

    #[test]
    fn complete_segmentations_follow_longest_first() {
        let graph = build_syllable_graph("xian");
        assert_eq!(enumerate_complete_segmentations(&graph, 32), vec![s(&["xian"]), s(&["xi", "an"])]);
        assert!(enumerate_complete_segmentations(&build_syllable_graph("xi'an"), 32).is_empty());
    }

    #[test]
    fn umlaut_aliases_follow_the_dictionary_spelling() {
        assert_eq!(normalize_umlaut_aliases(s(&["nue", "lue", "jv", "qve", "nu", "nv"])), s(&["nve", "lve", "ju", "que", "nu", "nv"]));
    }

    #[test]
    fn sparse_fallbacks_only_for_listed_syllables() {
        assert_eq!(sparse_pinyin_fallback_segments(&s(&["dia", "ni"])), vec![s(&["di", "a", "ni"]), s(&["di"])]);
        assert!(sparse_pinyin_fallback_segments(&s(&["ni"])).is_empty());
    }

    #[test]
    fn literal_matches_tell_whole_words_from_tails() {
        assert_eq!(literal_mixed_pinyin_match("nih", "ni'h", "ni'hao"), 1);
        assert_eq!(literal_mixed_pinyin_match("nihz", "ni'h", "ni'hao"), 2);
        assert_eq!(literal_mixed_pinyin_match("nihzz", "ni'h", "ni'hao"), 0);
        assert!(literal_jianpin_pinyin_match("nh", "n'h", "ni'hao"));
        assert!(!literal_jianpin_pinyin_match("nih", "ni'h", "ni'hao"));
    }
}
