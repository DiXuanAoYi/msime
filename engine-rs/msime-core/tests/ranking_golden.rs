//! 调频算法对着真实的 C++ `adjust_candidate_ranking` 核对：用例在 `engine/contracts/golden/ranking_cases.tsv`，
//! 基线 `expected_ranking.jsonl` 是 `msime_ranking_probe` 在合成词库上跑出来的每一行最终权重。

use msime_core::ranking::{Candidate, Outcome, SelectionCounter};
use serde_json::Value;
use std::path::PathBuf;

fn golden(file: &str) -> String {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden").join(file);
    std::fs::read_to_string(path).unwrap()
}

#[test]
fn weights_after_every_case_equal_the_cpp_engines() {
    let (mut cases, mut moved, mut counted_only) = (0, 0, 0);
    for line in golden("expected_ranking.jsonl").lines().filter(|l| !l.trim().is_empty()) {
        let row: Value = serde_json::from_str(line).unwrap();
        let case = row["case"].as_str().unwrap();
        let columns: Vec<&str> = case.split('\t').collect();
        assert_eq!(columns.len(), 7, "{case}");

        // 候选：`权重:键`，值是显示顺序里的 w0、w1……
        let mut weights: Vec<i64> = Vec::new();
        let mut candidates = Vec::new();
        for (index, cell) in columns[0].split(',').enumerate() {
            let (weight, key) = cell.split_once(':').unwrap();
            let weight: i64 = weight.parse().unwrap();
            weights.push(weight);
            candidates.push(Candidate {
                word: format!("w{index}"),
                weight,
                owns_entry_key: key == "a",
                from_dictionary: true,
            });
        }
        let selected: usize = columns[1].parse().unwrap();
        let (mode, step) = (columns[2], columns[3].parse::<i32>().unwrap());
        let (trigger, force, calls) = (columns[4].parse::<i32>().unwrap(), columns[5] == "1", columns[6].parse::<usize>().unwrap());

        let mut counter = SelectionCounter::new();
        let mut changed = Vec::new();
        for _ in 0..calls {
            let outcome = counter.select("ni", "ni", &format!("w{selected}"), &candidates, mode, step, trigger, force, false);
            changed.push(matches!(outcome, Outcome::Moved(_)));
            match outcome {
                Outcome::Moved(writes) => {
                    moved += 1;
                    for (word, weight) in writes {
                        weights[word[1..].parse::<usize>().unwrap()] = weight;
                    }
                }
                Outcome::Counted => counted_only += 1,
                _ => {}
            }
        }

        let expected_weights: Vec<i64> = row["weights"].as_array().unwrap().iter().map(|v| v.as_i64().unwrap()).collect();
        let expected_changed: Vec<bool> = row["changed"].as_array().unwrap().iter().map(|v| v.as_bool().unwrap()).collect();
        assert_eq!(weights, expected_weights, "权重不同：{case}");
        assert_eq!(changed, expected_changed, "是否改了排序不同：{case}");
        cases += 1;
    }
    assert!(cases >= 250, "只核对了 {cases} 个用例");
    assert!(moved >= 150 && counted_only >= 20, "挪动 {moved} 次、只计数 {counted_only} 次，用例没覆盖到两条路径");
    eprintln!("调频：核对了 {cases} 个用例，其中真正改写权重 {moved} 次、只计数 {counted_only} 次，与 C++ 逐行相同");
}

#[test]
fn the_case_file_and_the_baseline_stay_in_step() {
    let cases = golden("ranking_cases.tsv");
    let wanted = cases.lines().filter(|l| !l.trim().is_empty() && !l.starts_with('#')).count();
    assert_eq!(golden("expected_ranking.jsonl").lines().filter(|l| !l.trim().is_empty()).count(), wanted);
}
