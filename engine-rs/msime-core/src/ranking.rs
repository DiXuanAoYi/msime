//! 调频：用户在候选里选了排在后面的词，要把它的权重改到哪里。
//!
//! 移植自 `engine/user_dictionary/user_dictionary_journal.cpp` 的 `adjust_candidate_ranking`，只搬算法，
//! 不碰 SQLite：输入是一列候选（权重、是不是属于被调频的读音），输出是要写的 (词, 权重) 序列，
//! 由调用方写进词库和用户日志。选中次数的计数也在这里，用内存里的表代替 C++ 的 `candidate_selection_state`。
//!
//! 几条 C++ 里踩过的坑，行为必须保持：
//! - 权重要留在出货词典的量级内（上限 1 亿），别写出 10^12 级的数；
//! - 一列候选里会混着别的读音（单字母简拼 `y` 同时有 `yi` 的「一」和 `you` 的「有」），所有候选按权重排在同一把尺上，
//!   但**只写属于被调频读音的行**；
//! - 算中点必须按权重降序，不能按显示顺序（整句占着前面、置顶的被提到前面，显示顺序不是权重顺序）；
//! - 没有整数中点时跳过一小段被占用的权重，实在放不下才整排重排成台阶；权重全是 1 的一堆行放不下台阶，
//!   只提升被选中的这一行，不往邻居身上写负数。

use std::collections::HashMap;

pub const MANAGED_WEIGHT_CEILING: i64 = 100_000_000;
pub const MANAGED_WEIGHT_FLOOR: i64 = 1;
const REBALANCE_GAP: i64 = 1000;
const REBALANCE_COUNT: usize = 16;

pub fn clamp_managed_weight(weight: i64) -> i64 {
    weight.clamp(MANAGED_WEIGHT_FLOOR, MANAGED_WEIGHT_CEILING)
}

/// 选中的词按调频模式该挪到哪个名次（0 是首位）。`rank` 是它此刻在按权重排序的列表里的名次，不为 0。
/// 模式：`halve` 名次折半，`linear` 每次上移 `linear_step` 位，`promote` 一路升到第 5 位再往上每次一位；
/// 其余（包括写错的模式名）直接到首位。
pub fn ranking_target(rank: usize, mode: &str, linear_step: i32, force_top: bool) -> usize {
    if force_top {
        return 0;
    }
    match mode {
        "halve" => rank / 2,
        "linear" => {
            // C++ 把步数转成无符号数：负数变成极大值，名次追不上它，直接到首位。
            let step = usize::try_from(linear_step).unwrap_or(usize::MAX);
            if rank > step {
                rank - step
            } else {
                0
            }
        }
        "promote" => {
            if rank > 4 {
                4
            } else {
                rank - 1
            }
        }
        _ => 0,
    }
}

/// 触发次数夹在 1 到 10 之间。
pub fn clamp_trigger_count(trigger_count: i32) -> u32 {
    trigger_count.clamp(1, 10) as u32
}

/// 一个候选。`owns_entry_key` 为真表示它的词库读音就是被调频的读音（写入只落在这样的行上）。
#[derive(Debug, Clone, PartialEq)]
pub struct Candidate {
    pub word: String,
    pub weight: i64,
    pub owns_entry_key: bool,
    /// 来自词库（系统词库或用户词库）。整句联想、云候选之类不参与调频的尺子。
    pub from_dictionary: bool,
}

/// 要写进词库（和用户日志）的权重，按顺序执行：先整排重排的台阶，最后是选中的词。
pub type Writes = Vec<(String, i64)>;

/// 选中第 `rank` 名（按权重降序，名次从 0 起）时算出要写的权重。`rows` 已按权重降序。
fn plan_move(rows: &[Candidate], rank: usize, target: usize) -> Writes {
    let max = i64::MAX;
    let top_near_limit = target == 0 && rows[0].weight > max - 1000;
    let upper = if target == 0 && !top_near_limit {
        rows[0].weight + 1000
    } else if target == 0 {
        rows[0].weight
    } else {
        rows[target - 1].weight
    };
    let lower = rows[target].weight;
    let oversized = upper > MANAGED_WEIGHT_CEILING || lower > MANAGED_WEIGHT_CEILING;
    let mut need_rebalance = top_near_limit || lower == max || upper <= lower || oversized;
    let mut new_weight = 0i64;

    if !need_rebalance {
        if upper == lower + 1 {
            // 没有整数中点：比上面那个高 1，并跳过一小段被占用的权重。
            new_weight = upper + 1;
            let mut conflicts = 0;
            for i in (0..target).rev() {
                let occupied = rows[i].weight;
                if occupied < new_weight {
                    continue;
                }
                if occupied > new_weight {
                    break;
                }
                conflicts += 1;
                if conflicts >= 16 || new_weight >= MANAGED_WEIGHT_CEILING {
                    need_rebalance = true;
                    break;
                }
                new_weight += 1;
            }
        } else {
            new_weight = lower + (upper - lower) / 2;
        }
    }

    if need_rebalance && !oversized && !top_near_limit && target != 0 {
        let end = rows.len().min(target + REBALANCE_COUNT);
        let last_index = (end - 1) as i64;
        let last_weight = upper - REBALANCE_GAP - (last_index - target as i64) * REBALANCE_GAP;
        if last_weight < MANAGED_WEIGHT_FLOOR {
            // 权重相等或太低，放不下 16 级的台阶：只提升选中的这一行，不往邻居身上写负数。
            let cluster = upper.max(lower);
            if cluster <= max - REBALANCE_GAP {
                new_weight = clamp_managed_weight(cluster + REBALANCE_GAP);
                need_rebalance = false;
            }
        }
    }

    let mut writes = Writes::new();
    if need_rebalance {
        let end = rows.len().min(target + REBALANCE_COUNT);
        let mut base = if target == 0 { MANAGED_WEIGHT_CEILING - REBALANCE_GAP } else { MANAGED_WEIGHT_CEILING };
        if target != 0 && !top_near_limit && !oversized {
            let requested = upper + target as i64 * REBALANCE_GAP - REBALANCE_GAP;
            base = requested.min(MANAGED_WEIGHT_CEILING);
        }
        for (i, row) in rows.iter().enumerate().take(end).skip(target) {
            if row.owns_entry_key {
                writes.push((row.word.clone(), clamp_managed_weight(base - i as i64 * REBALANCE_GAP)));
            }
        }
        new_weight = if target == 0 {
            base + REBALANCE_GAP
        } else {
            base - target as i64 * REBALANCE_GAP + REBALANCE_GAP / 2
        };
    }
    let _ = rank;
    writes.push((rows[rank].word.clone(), clamp_managed_weight(new_weight)));
    writes
}

/// 一次选词的结果。
#[derive(Debug, Clone, PartialEq)]
pub enum Outcome {
    /// 没有可调的东西：候选或词为空、该词被固定位置，或调频关闭。
    Ignored,
    /// 只计了一次数，还没到触发次数。
    Counted,
    /// 选的本来就是第一名，计数清零，什么都不改。
    AlreadyFirst,
    /// 选中的词不在词库候选里；这次的计数不保留。
    NotFound,
    /// 要写的权重，见 [`Writes`]。
    Moved(Writes),
}

/// 选中次数的计数，键是（上下文, 读音, 词）。C++ 里是用户库的 `candidate_selection_state` 表。
#[derive(Debug, Default)]
pub struct SelectionCounter {
    counts: HashMap<(String, String, String), u32>,
}

impl SelectionCounter {
    pub fn new() -> Self {
        Self::default()
    }

    /// 用户从 `ordered` 里选了 `value`。`fixed` 表示该词被固定了位置（固定位置的词不调频）。
    #[allow(clippy::too_many_arguments)]
    pub fn select(
        &mut self,
        context_key: &str,
        entry_key: &str,
        value: &str,
        ordered: &[Candidate],
        mode: &str,
        linear_step: i32,
        trigger_count: i32,
        force_top: bool,
        fixed: bool,
    ) -> Outcome {
        if entry_key.is_empty() || value.is_empty() || ordered.is_empty() || fixed {
            return Outcome::Ignored;
        }
        if !force_top && mode == "disabled" {
            return Outcome::Ignored;
        }
        let key = (context_key.to_string(), entry_key.to_string(), value.to_string());
        let previous = self.counts.get(&key).copied();
        if !force_top {
            let count = previous.unwrap_or(0) + 1;
            self.counts.insert(key.clone(), count);
            if count < clamp_trigger_count(trigger_count) {
                return Outcome::Counted;
            }
        }

        // 所有候选排在同一把尺（权重降序）上；稳定排序，同权重的保持显示顺序。
        let mut rows: Vec<Candidate> = ordered.iter().filter(|c| c.from_dictionary).cloned().collect();
        rows.sort_by(|a, b| b.weight.cmp(&a.weight));
        let Some(rank) = rows.iter().position(|c| c.word == value) else {
            // 事务回滚，这次的计数也不留。
            match previous {
                Some(count) => self.counts.insert(key, count),
                None => self.counts.remove(&key),
            };
            return Outcome::NotFound;
        };
        if rank == 0 {
            self.counts.remove(&key);
            return Outcome::AlreadyFirst;
        }
        let target = ranking_target(rank, mode, linear_step, force_top);
        let writes = plan_move(&rows, rank, target);
        self.counts.remove(&key);
        Outcome::Moved(writes)
    }

    /// 某个词此刻累计了几次。
    pub fn count(&self, context_key: &str, entry_key: &str, value: &str) -> u32 {
        self.counts.get(&(context_key.to_string(), entry_key.to_string(), value.to_string())).copied().unwrap_or(0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rows(weights: &[i64]) -> Vec<Candidate> {
        weights
            .iter()
            .enumerate()
            .map(|(i, w)| Candidate { word: format!("w{i}"), weight: *w, owns_entry_key: true, from_dictionary: true })
            .collect()
    }

    fn select(candidates: &[Candidate], index: usize, mode: &str) -> Outcome {
        SelectionCounter::new().select("ni", "ni", &candidates[index].word, candidates, mode, 1, 1, false, false)
    }

    #[test]
    fn targets_follow_the_three_modes() {
        assert_eq!(ranking_target(5, "halve", 1, false), 2);
        assert_eq!(ranking_target(1, "halve", 1, false), 0);
        assert_eq!(ranking_target(5, "linear", 2, false), 3);
        assert_eq!(ranking_target(2, "linear", 2, false), 0);
        assert_eq!(ranking_target(2, "linear", -1, false), 0);
        assert_eq!(ranking_target(9, "promote", 1, false), 4);
        assert_eq!(ranking_target(3, "promote", 1, false), 2);
        assert_eq!(ranking_target(9, "anything-else", 1, false), 0);
        assert_eq!(ranking_target(9, "halve", 1, true), 0);
    }

    #[test]
    fn a_midpoint_between_the_neighbours_is_used_when_there_is_room() {
        let list = rows(&[500_000, 400_000, 300_000, 200_000]);
        // 第 4 名折半是第 2 名：夹在 500000 和 400000 之间，取中点。
        assert_eq!(select(&list, 3, "halve"), Outcome::Moved(vec![("w3".into(), 450_000)]));
    }

    #[test]
    fn going_to_the_top_lands_between_the_first_row_and_a_thousand_above_it() {
        let list = rows(&[500_000, 400_000]);
        assert_eq!(select(&list, 1, "halve"), Outcome::Moved(vec![("w1".into(), 500_500)]));
    }

    #[test]
    fn picking_the_first_row_changes_nothing() {
        let list = rows(&[500_000, 400_000]);
        assert_eq!(select(&list, 0, "halve"), Outcome::AlreadyFirst);
    }

    #[test]
    fn tied_low_weights_promote_only_the_selected_row() {
        // 一堆权重 1 的行放不下台阶：不能往邻居身上写负数，只把选中的抬到 1001。
        let list = rows(&[1, 1, 1, 1, 1, 1]);
        assert_eq!(select(&list, 5, "halve"), Outcome::Moved(vec![("w5".into(), 1001)]));
    }

    #[test]
    fn weights_never_leave_the_shipped_range() {
        let list = rows(&[i64::MAX, 100, 50]);
        let Outcome::Moved(writes) = select(&list, 2, "halve") else { panic!() };
        assert!(writes.iter().all(|(_, w)| (MANAGED_WEIGHT_FLOOR..=MANAGED_WEIGHT_CEILING).contains(w)), "{writes:?}");
    }

    #[test]
    fn only_rows_of_the_adjusted_key_are_written() {
        let mut list = rows(&[900, 800, 700, 600, 500]);
        list[0].owns_entry_key = false;
        list[2].owns_entry_key = false;
        // 只剩 800/600/500 三行是被调频读音的行。
        let outcome = select(&list, 4, "promote");
        let Outcome::Moved(writes) = outcome else { panic!("{outcome:?}") };
        assert!(writes.iter().all(|(word, _)| word != "w0" && word != "w2"), "{writes:?}");
    }

    #[test]
    fn the_midpoint_uses_weight_order_not_display_order() {
        // 显示顺序里 10 排在最前（比如整句占着位置），但权重最高的是 900。
        let mut list = rows(&[10, 900, 500, 30]);
        list[0].weight = 10;
        let Outcome::Moved(writes) = select(&list, 3, "promote") else { panic!() };
        // 权重降序是 900, 500, 30, 10：w3(30) 名次 2，目标 1，夹在 900 和 500 之间。
        assert_eq!(writes, vec![("w3".to_string(), 700)]);
    }

    #[test]
    fn selections_only_count_until_the_trigger_is_reached() {
        let list = rows(&[500, 400, 300]);
        let mut counter = SelectionCounter::new();
        let pick = |counter: &mut SelectionCounter| counter.select("ni", "ni", "w2", &list, "halve", 1, 3, false, false);
        assert_eq!(pick(&mut counter), Outcome::Counted);
        assert_eq!(counter.count("ni", "ni", "w2"), 1);
        assert_eq!(pick(&mut counter), Outcome::Counted);
        assert!(matches!(pick(&mut counter), Outcome::Moved(_)));
        assert_eq!(counter.count("ni", "ni", "w2"), 0, "挪动之后计数清零");
    }

    #[test]
    fn force_top_skips_the_counter_and_disabled_mode_does_nothing() {
        let list = rows(&[500, 400, 300]);
        let mut counter = SelectionCounter::new();
        assert!(matches!(counter.select("ni", "ni", "w2", &list, "disabled", 1, 3, true, false), Outcome::Moved(_)));
        assert_eq!(counter.select("ni", "ni", "w2", &list, "disabled", 1, 3, false, false), Outcome::Ignored);
        assert_eq!(counter.select("ni", "ni", "w2", &list, "halve", 1, 3, false, true), Outcome::Ignored, "固定位置的词不调频");
    }

    #[test]
    fn a_word_that_is_not_in_the_list_leaves_no_count_behind() {
        let list = rows(&[500, 400]);
        let mut counter = SelectionCounter::new();
        assert_eq!(counter.select("ni", "ni", "missing", &list, "halve", 1, 1, false, false), Outcome::NotFound);
        assert_eq!(counter.count("ni", "ni", "missing"), 0);
    }
}
