//! 全拼切分与键的小工具，移植自 `engine/quanpin/quanpin_query.cpp` 里的切分部分（贪心切分、带纠错的切分）和
//! `quanpin_utils.cpp` 里的几个判断。词库查询本身在 `dict.rs`。

use crate::syllables::{is_intact, INTACT};
use std::collections::HashMap;
use std::sync::OnceLock;

pub type Segments = Vec<String>;

const CORRECTION_PATH_LIMIT: usize = 32;

/// 按 `'` 切开；空串得到一个空段，和 C++ 的 `split` 一致（`split_segments` 对空串特判为空）。
pub fn split_segments(segmentation: &str) -> Segments {
    if segmentation.is_empty() {
        return Vec::new();
    }
    segmentation.split('\'').map(str::to_string).collect()
}

pub fn join_segments<S: AsRef<str>>(segments: &[S]) -> String {
    segments.iter().map(|s| s.as_ref()).collect::<Vec<_>>().join("'")
}

/// 每段取第一个字符，拼成简拼。
pub fn segments_to_jianpin<S: AsRef<str>>(segments: &[S]) -> String {
    segments.iter().filter_map(|s| s.as_ref().chars().next()).collect()
}

/// 全是合法完整音节（且至少一段）。
pub fn has_only_complete_pinyin_segments<S: AsRef<str>>(segments: &[S]) -> bool {
    !segments.is_empty() && segments.iter().all(|s| is_intact(s.as_ref()))
}

fn is_complete_pinyin_part(part: &str) -> bool {
    // 要能回溯：贪心在 zaijinuta 上取了 jin 就剩下切不动的 uta。与查询侧的切分同一个算法。
    !part.is_empty() && crate::segment::cut_min_segments(part).is_some()
}

/// 用 `'` 隔开的每一块都能切成完整音节。
pub fn is_complete_pinyin_input(pinyin: &str) -> bool {
    if pinyin.is_empty() {
        return false;
    }
    pinyin.split('\'').all(is_complete_pinyin_part)
}

/// 贪心切分：每块各自切成最少音节；`intact_only` 为假时音节的前缀也算一段。切不开的块原样留着（`intact_only` 时整个失败）。
pub fn cut_pinyin_greedy(pinyin: &str, intact_only: bool) -> Segments {
    if pinyin.is_empty() {
        return Vec::new();
    }
    let cut_piece = |part: &str| -> Option<Vec<String>> {
        if intact_only {
            crate::segment::cut_min_segments(part)
        } else {
            crate::segment::cut_min_segments_prefix(part)
        }
    };
    if !pinyin.contains('\'') {
        return cut_piece(pinyin).unwrap_or_default();
    }
    let mut merged = Vec::new();
    for part in pinyin.split('\'') {
        match cut_piece(part) {
            Some(cut) if !cut.is_empty() => merged.extend(cut),
            _ => {
                if intact_only && !part.is_empty() {
                    return Vec::new();
                }
                if !part.is_empty() {
                    merged.push(part.to_string());
                }
            }
        }
    }
    merged
}

type CorrectionAliases = HashMap<String, Vec<String>>;

fn pinyin_correction_aliases() -> &'static CorrectionAliases {
    static ALIASES: OnceLock<CorrectionAliases> = OnceLock::new();
    ALIASES.get_or_init(|| {
        let mut aliases: CorrectionAliases = HashMap::new();
        let mut add_alias = |typed: &str, canonical: &str| {
            let canonical_syllables = aliases.entry(typed.to_string()).or_default();
            if !canonical_syllables.iter().any(|c| c == canonical) {
                canonical_syllables.push(canonical.to_string());
            }
        };
        let add_suffix_aliases = |canonical_suffix: &str, typed_suffix: &str, add_alias: &mut dyn FnMut(&str, &str)| {
            for syllable in INTACT {
                if syllable.len() < canonical_suffix.len() || !syllable.ends_with(canonical_suffix) {
                    continue;
                }
                let prefix = &syllable[..syllable.len() - canonical_suffix.len()];
                add_alias(&format!("{prefix}{typed_suffix}"), syllable);
            }
        };
        add_suffix_aliases("iang", "aing", &mut add_alias);
        add_suffix_aliases("uang", "aung", &mut add_alias);
        add_suffix_aliases("ian", "ain", &mut add_alias);
        add_suffix_aliases("uan", "aun", &mut add_alias);
        add_suffix_aliases("iao", "aio", &mut add_alias);
        add_suffix_aliases("ing", "ihng", &mut add_alias);
        add_suffix_aliases("ang", "agn", &mut add_alias);
        add_suffix_aliases("eng", "egn", &mut add_alias);
        add_alias("egn", "eng");
        add_alias("jv", "ju");
        // 下一个音节前漏了 g：zhonguo -> zhong guo。
        add_suffix_aliases("ong", "on", &mut add_alias);
        // 优先当作 h 位置颠倒（ahng -> hang），同时保留多敲了 h 的读法（ahng -> ang）。
        add_suffix_aliases("hang", "ahng", &mut add_alias);
        add_suffix_aliases("ang", "ahng", &mut add_alias);
        // 同理 cehng -> cheng，备选 cehng -> ceng。
        add_suffix_aliases("heng", "ehng", &mut add_alias);
        add_suffix_aliases("eng", "ehng", &mut add_alias);
        aliases
    })
}

#[derive(Clone)]
struct RankedPath {
    segments: Segments,
    typed_lengths: Vec<usize>,
    correction_ranks: Vec<usize>,
}

fn solve_with_corrections(pinyin: &str, index: usize, memo: &mut HashMap<usize, Vec<RankedPath>>) -> Vec<RankedPath> {
    if index == pinyin.len() {
        return vec![RankedPath { segments: Vec::new(), typed_lengths: Vec::new(), correction_ranks: Vec::new() }];
    }
    if let Some(found) = memo.get(&index) {
        return found.clone();
    }
    let aliases = pinyin_correction_aliases();
    let mut paths: Vec<RankedPath> = Vec::new();
    for end in (index + 1..=pinyin.len()).rev() {
        let Some(typed) = pinyin.get(index..end) else { continue };
        let canonical_syllables: Vec<String> = if let Some(alias) = aliases.get(typed) {
            alias.clone()
        } else if is_intact(typed) {
            vec![typed.to_string()]
        } else {
            continue;
        };
        let suffix_paths = solve_with_corrections(pinyin, end, memo);
        for (canonical_index, canonical) in canonical_syllables.iter().enumerate() {
            for suffix in &suffix_paths {
                let mut segments = Vec::with_capacity(1 + suffix.segments.len());
                segments.push(canonical.clone());
                segments.extend(suffix.segments.iter().cloned());
                let mut typed_lengths = Vec::with_capacity(1 + suffix.typed_lengths.len());
                typed_lengths.push(end - index);
                typed_lengths.extend(suffix.typed_lengths.iter().copied());
                let mut correction_ranks = Vec::with_capacity(1 + suffix.correction_ranks.len());
                correction_ranks.push(canonical_index);
                correction_ranks.extend(suffix.correction_ranks.iter().copied());
                paths.push(RankedPath { segments, typed_lengths, correction_ranks });
            }
        }
    }
    // stable_sort：音节少的在前，再比每个音节敲了几个字母，再比纠错备选的名次。
    paths.sort_by(|a, b| {
        a.segments
            .len()
            .cmp(&b.segments.len())
            .then_with(|| a.typed_lengths.cmp(&b.typed_lengths))
            .then_with(|| a.correction_ranks.cmp(&b.correction_ranks))
    });
    paths.dedup_by(|later, earlier| later.segments == earlier.segments);
    if let Some(first) = paths.first() {
        let minimum_segments = first.segments.len();
        if let Some(cut_at) = paths.iter().position(|p| p.segments.len() != minimum_segments) {
            paths.truncate(cut_at);
        }
    }
    paths.truncate(CORRECTION_PATH_LIMIT);
    memo.insert(index, paths.clone());
    paths
}

fn cut_one_piece_with_corrections(pinyin: &str) -> Vec<Segments> {
    let mut memo = HashMap::new();
    solve_with_corrections(pinyin, 0, &mut memo).into_iter().map(|p| p.segments).collect()
}

fn cut_pinyin_with_corrections(pinyin: &str) -> Vec<Segments> {
    let mut merged_paths: Vec<Segments> = vec![Vec::new()];
    for part in pinyin.split('\'') {
        let part_paths = cut_one_piece_with_corrections(part);
        if part_paths.is_empty() {
            return Vec::new();
        }
        let mut combined: Vec<Segments> = Vec::new();
        'outer: for merged in &merged_paths {
            for part_path in &part_paths {
                let mut candidate = merged.clone();
                candidate.extend(part_path.iter().cloned());
                combined.push(candidate);
                if combined.len() == CORRECTION_PATH_LIMIT {
                    break 'outer;
                }
            }
        }
        merged_paths = combined;
    }
    merged_paths
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CutMode {
    Greedy,
    Correction,
}

/// 对应 C++ `cut_pinyin_by_mode`：贪心只出一条；纠错模式先出带纠错的若干条，没有再退回贪心。
pub fn cut_pinyin_by_mode(pinyin: &str, mode: CutMode) -> Vec<Segments> {
    if mode == CutMode::Correction {
        let corrected = cut_pinyin_with_corrections(pinyin);
        if !corrected.is_empty() {
            return corrected;
        }
    }
    let greedy = cut_pinyin_greedy(pinyin, false);
    if greedy.is_empty() {
        Vec::new()
    } else {
        vec![greedy]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seg(items: &[&str]) -> Segments {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn greedy_cut_keeps_unfinished_tails() {
        assert_eq!(cut_pinyin_greedy("nihao", false), seg(&["ni", "hao"]));
        assert_eq!(cut_pinyin_greedy("nh", false), seg(&["n", "h"]));
        assert_eq!(cut_pinyin_greedy("ni'hao", true), seg(&["ni", "hao"]));
        assert!(cut_pinyin_greedy("ni'hx", true).is_empty());
        assert_eq!(cut_pinyin_greedy("ni'hx", false), seg(&["ni", "h", "x"]));
    }

    #[test]
    fn correction_cut_repairs_typos_and_falls_back_to_greedy() {
        assert_eq!(cut_pinyin_by_mode("zhonguo", CutMode::Correction)[0], seg(&["zhong", "guo"]));
        assert_eq!(cut_pinyin_by_mode("nihao", CutMode::Correction)[0], seg(&["ni", "hao"]));
        assert_eq!(cut_pinyin_by_mode("xx", CutMode::Correction), cut_pinyin_by_mode("xx", CutMode::Greedy));
    }

    #[test]
    fn jianpin_and_joining() {
        assert_eq!(segments_to_jianpin(&seg(&["ni", "hao"])), "nh");
        assert_eq!(join_segments(&seg(&["ni", "hao"])), "ni'hao");
        assert!(is_complete_pinyin_input("zai'ji'nu'ta"));
        assert!(!is_complete_pinyin_input("zai'jx"));
    }
}
