//! 把一串全拼切成音节。移植自 `engine/quanpin/quanpin_utils.cpp` 的 `cut_one_piece_min_segments`
//! （`intact_only = true` 的那条路径）。
//!
//! 规则：切成最少的音节；音节数相同时，首个音节取更短的。后一条是有意的，
//! 不要改成“向前取最长”（`xian` 会被切成 `xian` 而不是 `xi'an`，`fangan` 则相反）。
//! 切不开返回 `None`。

use crate::syllables::is_intact;
use std::collections::HashMap;

pub fn cut_min_segments(pinyin: &str) -> Option<Vec<String>> {
    if pinyin.is_empty() || !pinyin.is_ascii() {
        return None;
    }
    let mut memo: HashMap<usize, Option<Vec<String>>> = HashMap::new();
    solve(pinyin, 0, &mut memo)
}

fn solve(text: &str, start: usize, memo: &mut HashMap<usize, Option<Vec<String>>>) -> Option<Vec<String>> {
    if start == text.len() {
        return Some(Vec::new());
    }
    if let Some(found) = memo.get(&start) {
        return found.clone();
    }
    let mut best: Option<Vec<String>> = None;
    // 和 C++ 一样从最长的片段往短试；平局时后试到的更短首音节会替换前一个。
    for end in (start + 1..=text.len()).rev() {
        let piece = &text[start..end];
        if !is_intact(piece) {
            continue;
        }
        let Some(rest) = solve(text, end, memo) else { continue };
        let mut candidate = Vec::with_capacity(1 + rest.len());
        candidate.push(piece.to_string());
        candidate.extend(rest);
        best = match best {
            None => Some(candidate),
            Some(current) if candidate.len() < current.len() => Some(candidate),
            Some(current)
                if candidate.len() == current.len() && candidate[0].len() < current[0].len() =>
            {
                Some(candidate)
            }
            keep => keep,
        };
    }
    memo.insert(start, best.clone());
    best
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cut(text: &str) -> Option<Vec<String>> {
        cut_min_segments(text)
    }

    #[test]
    fn splits_plain_words() {
        assert_eq!(cut("nihao").unwrap(), ["ni", "hao"]);
        assert_eq!(cut("zhongguo").unwrap(), ["zhong", "guo"]);
        assert_eq!(cut("shurufa").unwrap(), ["shu", "ru", "fa"]);
    }

    #[test]
    fn prefers_fewest_syllables_then_the_shorter_first_one() {
        // xian 既是一个音节也能切成 xi'an：音节更少的赢。
        assert_eq!(cut("xian").unwrap(), ["xian"]);
        // fangan：fan'gan 和 fang'an 一样多，取首音节更短的 fan。
        assert_eq!(cut("fangan").unwrap(), ["fan", "gan"]);
        assert_eq!(cut("tianan").unwrap(), ["tian", "an"]);
    }

    #[test]
    fn backtracks_instead_of_getting_stuck_on_a_greedy_cut() {
        // 贪心取 jin 会剩下切不动的 uta。
        assert_eq!(cut("zaijinuta").unwrap(), ["zai", "ji", "nu", "ta"]);
    }

    #[test]
    fn rejects_what_is_not_pinyin() {
        assert_eq!(cut(""), None);
        assert_eq!(cut("xyz"), None);
        assert_eq!(cut("nihaoa").map(|v| v.join("'")), Some("ni'hao'a".to_string()));
        assert_eq!(cut("你好"), None);
    }
}
