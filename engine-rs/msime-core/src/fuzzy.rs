//! 模糊音。移植自 `engine/quanpin/fuzzy_pinyin.h`：一个完整音节能被读成哪些近音，
//! 一串音节的近音路径，以及把模糊候选混进精确候选的规则。

use crate::syllables::{is_intact, INTACT};

/// 位掩码，位置和 `engine/core/fuzzy_pinyin_options.h` 的 `FuzzyPinyinRule` 一一对应，也是平台偏好里存的值。
pub mod rule {
    pub const Z_ZH: u32 = 1 << 0;
    pub const C_CH: u32 = 1 << 1;
    pub const S_SH: u32 = 1 << 2;
    pub const N_L: u32 = 1 << 3;
    pub const F_H: u32 = 1 << 4;
    pub const R_L: u32 = 1 << 5;
    pub const AN_ANG: u32 = 1 << 6;
    pub const EN_ENG: u32 = 1 << 7;
    pub const IN_ING: u32 = 1 << 8;
    pub const IAN_IANG: u32 = 1 << 9;
    pub const UAN_UANG: u32 = 1 << 10;
}

/// 开模糊音等于“这个音我可能没分清”，模糊读音和敲出的读音按词频混排；折扣保证敲出读音的常用字
/// 仍然压得住模糊读音。
pub const WEIGHT_RATIO: f64 = 0.2;

const INITIALS: [(&str, &str, u32); 6] = [
    ("z", "zh", rule::Z_ZH),
    ("c", "ch", rule::C_CH),
    ("s", "sh", rule::S_SH),
    ("n", "l", rule::N_L),
    ("f", "h", rule::F_H),
    ("r", "l", rule::R_L),
];
const FINALS: [(&str, &str, u32); 5] = [
    ("an", "ang", rule::AN_ANG),
    ("en", "eng", rule::EN_ENG),
    ("in", "ing", rule::IN_ING),
    ("ian", "iang", rule::IAN_IANG),
    ("uan", "uang", rule::UAN_UANG),
];

/// 一个完整音节的近音，第一个永远是它自己。没打完的音节不展开（保持正常的补全行为）。
pub fn fuzzy_syllables(syllable: &str, rules: u32) -> Vec<String> {
    if rules == 0 || !is_intact(syllable) {
        return vec![syllable.to_string()];
    }
    let initial = if ["zh", "ch", "sh"].iter().any(|p| syllable.starts_with(p)) {
        &syllable[..2]
    } else if "bpmfdtnlgkhjqxrzcsyw".contains(&syllable[..1]) {
        &syllable[..1]
    } else {
        ""
    };
    let final_part = &syllable[initial.len()..];
    let mut starts = vec![initial.to_string()];
    let mut ends = vec![final_part.to_string()];
    for (a, b, bit) in INITIALS {
        if rules & bit != 0 {
            if initial == a {
                starts.push(b.to_string());
            } else if initial == b {
                starts.push(a.to_string());
            }
        }
    }
    for (a, b, bit) in FINALS {
        if rules & bit != 0 {
            if final_part == a {
                ends.push(b.to_string());
            } else if final_part == b {
                ends.push(a.to_string());
            }
        }
    }
    let mut result = vec![syllable.to_string()];
    for start in &starts {
        for end in &ends {
            let candidate = format!("{start}{end}");
            if INTACT.contains(&candidate.as_str()) && !result.contains(&candidate) {
                result.push(candidate);
            }
        }
    }
    result
}

/// 一串音节所有的近音路径（不含原路径）。路径数封顶，免得规则全开时按乘积爆炸。
pub fn fuzzy_segmentations(segments: &[String], rules: u32, limit: usize) -> Vec<Vec<String>> {
    if rules == 0 || segments.is_empty() || limit == 0 {
        return Vec::new();
    }
    let mut paths: Vec<Vec<String>> = vec![Vec::new()];
    for syllable in segments {
        let alternatives = fuzzy_syllables(syllable, rules);
        let mut next = Vec::new();
        'fill: for path in &paths {
            for alternative in &alternatives {
                if next.len() == limit {
                    break 'fill;
                }
                let mut copy = path.clone();
                copy.push(alternative.clone());
                next.push(copy);
            }
        }
        paths = next;
    }
    paths.retain(|path| path != segments);
    paths
}

/// 参与混排的候选。
#[derive(Debug, Clone, PartialEq)]
pub struct Candidate {
    pub text: String,
    /// 候选覆盖的拼音，音节之间用单引号。
    pub pinyin: String,
    pub weight: i64,
    pub fuzzy: bool,
    /// 来自词库的精确候选：只有它们的位置由权重决定，整句、保护位、调频的结果留在原处。
    pub from_dictionary: bool,
}

/// 候选覆盖了多少个敲出的字母，单引号不算。
pub fn matched_letters(candidate: &Candidate) -> usize {
    candidate.pinyin.chars().filter(|c| *c != '\'').count()
}

/// 把模糊候选归并进精确候选。`group` 是候选覆盖的输入长度，大的在前。精确列表的次序原样保留；
/// 模糊行按 group、权重排好后，逐个插到第一个“比它短”或“同长而权重低于它折后权重”的精确词库行前面。
pub fn merge_fuzzy_candidates(
    exact: Vec<Candidate>,
    mut fuzzy: Vec<Candidate>,
    group: impl Fn(&Candidate) -> usize,
) -> Vec<Candidate> {
    let mut seen: std::collections::HashSet<String> = exact.iter().map(|c| c.text.clone()).collect();
    fuzzy.retain(|c| seen.insert(c.text.clone()));
    // 稳定排序：group 大的在前，同 group 权重高的在前。
    fuzzy.sort_by(|a, b| {
        let (ga, gb) = (group(a), group(b));
        gb.cmp(&ga).then(b.weight.cmp(&a.weight))
    });
    let ranked_by_weight = |c: &Candidate| !c.fuzzy && c.from_dictionary;
    let mut merged = Vec::with_capacity(exact.len() + fuzzy.len());
    let (mut exact, mut fuzzy) = (exact.into_iter().peekable(), fuzzy.into_iter().peekable());
    loop {
        let take_fuzzy = match (exact.peek(), fuzzy.peek()) {
            (None, None) => break,
            (None, Some(_)) => true,
            (Some(_), None) => false,
            (Some(e), Some(f)) if ranked_by_weight(e) => {
                let (ge, gf) = (group(e), group(f));
                if ge != gf {
                    gf > ge
                } else {
                    (e.weight as f64) < (f.weight as f64) * WEIGHT_RATIO
                }
            }
            (Some(_), Some(_)) => false,
        };
        merged.push(if take_fuzzy { fuzzy.next().unwrap() } else { exact.next().unwrap() });
    }
    merged
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(items: &[&str]) -> Vec<String> {
        items.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn without_rules_a_syllable_has_no_neighbours() {
        assert_eq!(fuzzy_syllables("zi", 0), ["zi"]);
    }

    #[test]
    fn expands_initial_pairs_both_ways() {
        assert_eq!(fuzzy_syllables("zi", rule::Z_ZH), ["zi", "zhi"]);
        assert_eq!(fuzzy_syllables("zhi", rule::Z_ZH), ["zhi", "zi"]);
        assert_eq!(fuzzy_syllables("ni", rule::N_L), ["ni", "li"]);
        assert_eq!(fuzzy_syllables("le", rule::R_L), ["le", "re"]);
    }

    #[test]
    fn expands_finals_and_keeps_only_legal_syllables() {
        assert_eq!(fuzzy_syllables("xin", rule::IN_ING), ["xin", "xing"]);
        assert_eq!(fuzzy_syllables("fan", rule::AN_ANG), ["fan", "fang"]);
        // wang 去掉 g 是 wan，合法；ban→bang 合法；而 dan 的近音 dang 也合法，zan→zang 合法……
        assert!(fuzzy_syllables("wang", rule::AN_ANG).contains(&"wan".to_string()));
        // chuang 没有对应的无 g 形式时不应造出非法音节：guan → guang 合法，nuan → nuang 不合法。
        assert_eq!(fuzzy_syllables("nuan", rule::UAN_UANG), ["nuan"]);
    }

    #[test]
    fn combines_initial_and_final_rules() {
        let all = fuzzy_syllables("zan", rule::Z_ZH | rule::AN_ANG);
        for expected in ["zan", "zang", "zhan", "zhang"] {
            assert!(all.contains(&expected.to_string()), "{all:?}");
        }
    }

    #[test]
    fn incomplete_syllables_are_left_alone() {
        assert_eq!(fuzzy_syllables("zh", rule::Z_ZH), ["zh"]);
        assert_eq!(fuzzy_syllables("n", rule::N_L), ["n"]);
    }

    #[test]
    fn segmentations_exclude_the_original_and_respect_the_limit() {
        let paths = fuzzy_segmentations(&s(&["zi", "dian"]), rule::Z_ZH, 64);
        assert_eq!(paths, vec![s(&["zhi", "dian"])]);
        let all = fuzzy_segmentations(&s(&["zan", "zan", "zan"]), 2047, 5);
        assert!(all.len() <= 5);
        assert!(fuzzy_segmentations(&s(&["zi"]), 0, 64).is_empty());
    }

    fn cand(text: &str, pinyin: &str, weight: i64, fuzzy: bool, from_dictionary: bool) -> Candidate {
        Candidate { text: text.into(), pinyin: pinyin.into(), weight, fuzzy, from_dictionary }
    }

    #[test]
    fn merge_drops_fuzzy_duplicates_and_keeps_the_exact_order() {
        let exact = vec![cand("你好", "ni'hao", 100, false, true), cand("拟好", "ni'hao", 10, false, true)];
        let fuzzy = vec![cand("你好", "li'hao", 90, true, true)];
        let merged = merge_fuzzy_candidates(exact.clone(), fuzzy, matched_letters);
        assert_eq!(merged, exact);
    }

    #[test]
    fn merge_inserts_a_fuzzy_word_above_exact_words_with_much_lower_weight() {
        // 模糊词权重 1000 折后 200，高于精确词 150：插到它前面。
        let exact = vec![cand("甲", "min", 10_000, false, true), cand("乙", "min", 150, false, true)];
        let fuzzy = vec![cand("明", "ming", 1000, true, true)];
        let merged = merge_fuzzy_candidates(exact, fuzzy, |_| 3);
        assert_eq!(merged.iter().map(|c| c.text.as_str()).collect::<Vec<_>>(), ["甲", "明", "乙"]);
    }

    #[test]
    fn merge_prefers_candidates_that_cover_more_of_the_input() {
        let exact = vec![cand("你", "ni", 100, false, true)];
        let fuzzy = vec![cand("利好", "li'hao", 1, true, true)];
        let merged = merge_fuzzy_candidates(exact, fuzzy, matched_letters);
        assert_eq!(merged[0].text, "利好");
    }

    #[test]
    fn merge_does_not_move_entries_that_are_not_ranked_by_weight() {
        // 整句联想之类不是词库行：它们留在原处，模糊行排到后面。
        let exact = vec![cand("整句", "a'b'c", 1, false, false), cand("词", "a", 5, false, true)];
        let fuzzy = vec![cand("糊", "a", 1_000_000, true, true)];
        let merged = merge_fuzzy_candidates(exact, fuzzy, |c| if c.pinyin.len() > 1 { 3 } else { 1 });
        assert_eq!(merged[0].text, "整句");
    }
}
