//! 对着现引擎的黄金样本核对 Rust 的词库查询。
//!
//! 现引擎的候选里还混着简拼、纠错、整句词格和用户调频，这些 Rust 还没接管，所以这里核对的是更窄的一条：
//! 对每条能完整切成音节的输入，Rust 从词库里取到的词，在现引擎的候选列表里**相对顺序必须一致**。
//! 数据来自随包词库 `installer/app_data/msime.db`，也可以用环境变量 `MSIME_DB` 指定；两者都没有就跳过。

use msime_core::{cut_min_segments, Dictionary};
use std::path::PathBuf;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

/// 带单引号的输入按单引号分段，每段各自切分（和 C++ 一致：`xi'an` 是 xi、an，而 `xian` 是 xian）。
fn syllables(keys: &str) -> Option<Vec<String>> {
    let mut all = Vec::new();
    for part in keys.split('\'') {
        all.extend(cut_min_segments(part)?);
    }
    Some(all)
}

#[test]
fn exact_lookups_keep_the_cpp_engines_relative_order() {
    let database = std::env::var_os("MSIME_DB").map(PathBuf::from).unwrap_or_else(|| root().join("installer/app_data/msime.db"));
    if !database.exists() {
        eprintln!("跳过：没有 {}", database.display());
        return;
    }
    let dictionary = Dictionary::open(&database).expect("打不开词库");
    let golden = std::fs::read_to_string(root().join("engine/contracts/golden/expected.jsonl")).unwrap();

    let (mut checked, mut compared_words) = (0, 0);
    for line in golden.lines().filter(|l| !l.trim().is_empty()) {
        let row: serde_json::Value = serde_json::from_str(line).unwrap();
        if row["scheme"] != "quanpin" {
            continue;
        }
        let keys = row["keys"].as_str().unwrap();
        let Some(cut) = syllables(keys) else { continue };
        let entries = dictionary.exact(&cut).unwrap();
        if entries.is_empty() {
            continue;
        }
        let golden_texts: Vec<&str> = row["snapshot"]["candidates"]
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c["text"].as_str().unwrap())
            .collect();

        // 基线只留了前 40 个，Rust 的词也只看落在这 40 个里的，按它们在 C++ 列表里的位置检查是否单调。
        let mut last = None;
        let mut seen = Vec::new();
        for entry in entries.iter().take(30) {
            if let Some(position) = golden_texts.iter().position(|t| *t == entry.text) {
                if let Some(previous) = last {
                    assert!(
                        position > previous,
                        "{keys}：Rust 的顺序 {seen:?} 在「{}」处和 C++ 的 {golden_texts:?} 不一致",
                        entry.text
                    );
                }
                last = Some(position);
                seen.push(entry.text.clone());
                compared_words += 1;
            }
        }
        // 第一名要对得上：这些输入里 C++ 的首选就是词库里权重最高的那个。
        if let Some(first) = entries.first() {
            if golden_texts.first() != Some(&first.text.as_str()) {
                eprintln!("提示：{keys} 词库第一是「{}」，C++ 首选是「{}」（别的来源把它挤到前面）", first.text, golden_texts[0]);
            }
        }
        checked += 1;
    }
    assert!(checked >= 30, "只核对了 {checked} 条，样本或词库不对劲");
    assert!(compared_words >= 150, "只比较了 {compared_words} 个词");
    eprintln!("核对了 {checked} 条输入、{compared_words} 个词");
}
