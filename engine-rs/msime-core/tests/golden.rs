//! 对着现引擎的黄金样本核对 Rust 的词库查询。
//!
//! 现引擎的候选里还混着纠错、整句词格和用户调频，这些 Rust 还没接管，所以这里核对的是更窄的一条：
//! 对每条能切开的输入，Rust 从词库里取到的词，在现引擎的候选列表里**相对顺序必须一致**。
//! 数据来自随包词库 `installer/app_data/msime.db`，也可以用环境变量 `MSIME_DB` 指定；两者都没有就跳过。

use msime_core::{cut_min_segments, cut_min_segments_prefix, Dictionary};
use serde_json::Value;
use std::path::PathBuf;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn open() -> Option<Dictionary> {
    let database = std::env::var_os("MSIME_DB").map(PathBuf::from).unwrap_or_else(|| root().join("installer/app_data/msime.db"));
    if !database.exists() {
        eprintln!("跳过：没有 {}", database.display());
        return None;
    }
    Some(Dictionary::open(&database).expect("打不开词库"))
}

fn golden_rows() -> Vec<Value> {
    rows_of("expected.jsonl")
}

fn rows_of(file: &str) -> Vec<Value> {
    let text = std::fs::read_to_string(root().join("engine/contracts/golden").join(file)).unwrap();
    text.lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["scheme"] == "quanpin")
        .collect()
}

fn golden_texts(row: &Value) -> Vec<String> {
    row["snapshot"]["candidates"]
        .as_array()
        .unwrap()
        .iter()
        .map(|c| c["text"].as_str().unwrap().to_string())
        .collect()
}

/// 带单引号的输入按单引号分段，每段各自切分（和 C++ 一致：`xi'an` 是 xi、an，而 `xian` 是 xian）。
fn syllables(keys: &str) -> Option<Vec<String>> {
    let mut all = Vec::new();
    for part in keys.split('\'') {
        all.extend(cut_min_segments(part)?);
    }
    Some(all)
}

/// 把 `texts`（Rust 的结果，按 Rust 的顺序）逐个找到它们在 `golden` 里的位置，位置必须递增。
/// 基线只留了前 40 个候选，不在里面的词不参与比较。返回参与比较的词数。
fn assert_same_relative_order(label: &str, texts: &[String], golden: &[String]) -> usize {
    let (mut last, mut compared) = (None, 0);
    for text in texts {
        if let Some(position) = golden.iter().position(|g| g == text) {
            if let Some(previous) = last {
                assert!(
                    position > previous,
                    "{label}：Rust 的顺序在「{text}」处和 C++ 的 {golden:?} 不一致（Rust：{texts:?}）"
                );
            }
            last = Some(position);
            compared += 1;
        }
    }
    compared
}

#[test]
fn exact_lookups_keep_the_cpp_engines_relative_order() {
    let Some(dictionary) = open() else { return };
    let (mut checked, mut compared_words) = (0, 0);
    for row in golden_rows() {
        let keys = row["keys"].as_str().unwrap();
        let Some(cut) = syllables(keys) else { continue };
        let entries = dictionary.exact(&cut).unwrap();
        if entries.is_empty() {
            continue;
        }
        let texts: Vec<String> = entries.iter().take(30).map(|e| e.text.clone()).collect();
        compared_words += assert_same_relative_order(keys, &texts, &golden_texts(&row));
        checked += 1;
    }
    assert!(checked >= 30, "只核对了 {checked} 条，样本或词库不对劲");
    assert!(compared_words >= 150, "只比较了 {compared_words} 个词");
    eprintln!("精确查询：核对了 {checked} 条输入、{compared_words} 个词");
}

#[test]
fn jianpin_and_unfinished_syllables_keep_the_cpp_engines_relative_order() {
    let Some(dictionary) = open() else { return };
    let (mut checked, mut compared_words) = (0, 0);
    for row in golden_rows() {
        let keys = row["keys"].as_str().unwrap();
        if keys.contains('\'') {
            continue;
        }
        let Some(segments) = cut_min_segments_prefix(keys) else { continue };
        // 只看带简拼的输入：全是完整音节的归上面那条测试。
        if segments.iter().all(|s| msime_core::syllables::is_intact(s)) {
            continue;
        }
        let entries = msime_core::dedupe_by_text(dictionary.query(&segments, 100).unwrap());
        if entries.is_empty() {
            continue;
        }
        let texts: Vec<String> = entries.iter().take(30).map(|e| e.text.clone()).collect();
        let compared = assert_same_relative_order(keys, &texts, &golden_texts(&row));
        eprintln!("  {keys:<10} {segments:?} → {} 个词，{compared} 个在基线里", entries.len());
        compared_words += compared;
        checked += 1;
    }
    assert!(checked >= 10, "只核对了 {checked} 条简拼输入");
    assert!(compared_words >= 80, "只比较了 {compared_words} 个词");
    eprintln!("简拼查询：核对了 {checked} 条输入、{compared_words} 个词");
}

#[test]
fn fuzzy_pinyin_keeps_the_cpp_engines_relative_order() {
    use msime_core::fuzzy::{matched_letters, merge_fuzzy_candidates};
    let Some(dictionary) = open() else { return };
    let (mut checked, mut fuzzy_words, mut compared_words) = (0, 0, 0);
    for row in rows_of("expected_options.jsonl") {
        let rules = row["fuzzy"].as_u64().unwrap_or(0) as u32;
        if rules == 0 || row["autocorrect"].as_u64().unwrap_or(0) != 0 || row["scheme"] != "quanpin" {
            continue;
        }
        let keys = row["keys"].as_str().unwrap();
        let Some(typed) = syllables(keys) else { continue };
        let exact = dictionary.exact_candidates(&typed, 100).unwrap();
        let fuzzy = dictionary.fuzzy_candidates(&typed, rules).unwrap();
        let merged = merge_fuzzy_candidates(exact, fuzzy, matched_letters);
        fuzzy_words += merged.iter().filter(|c| c.fuzzy).count();
        let texts: Vec<String> = merged.iter().take(30).map(|c| c.text.clone()).collect();
        let label = format!("{keys}（fuzzy={rules}）");
        compared_words += assert_same_relative_order(&label, &texts, &golden_texts(&row));
        checked += 1;
    }
    assert!(checked >= 20, "只核对了 {checked} 条模糊音输入");
    assert!(fuzzy_words >= 100, "只出现了 {fuzzy_words} 个模糊词，模糊路径可能没起作用");
    eprintln!("模糊音：核对了 {checked} 条输入、{compared_words} 个词，其中模糊词合计 {fuzzy_words}");
}

/// C++ 在纠错之前还有几道 Rust 没接管的闸和仲裁，下面这些输入走的是那几条路，所以不在对照范围里：
/// - 简拼形状闸：`nhao` 像 n+hao 的简拼，C++ 直接不纠错，保留 您好；
/// - 合法输入闸：`tianqi`、`shurufa` 本身合法，主切不动，纠错读法只在整句打分里参与；
/// - 表外生成读法：`nihap` 的“你还”来自任意字母替换，只有静态表无法解释时才出场，这里的混排取舍还在 C++；
/// - `niihao` 只开“多键”时 C++ 返回空候选，开别的类型或全开都有 你好，疑似边角缺陷，见 README。
const AUTOCORRECT_CPP_ONLY: [(&str, u64); 6] =
    [("nhao", 15), ("nhao", 4), ("tianqi", 15), ("shurufa", 15), ("nihap", 15), ("nihap", 2)];

#[test]
fn autocorrect_primary_cut_keeps_the_cpp_engines_relative_order() {
    use msime_core::autocorrect::tables;
    let Some(dictionary) = open() else { return };
    let tables = tables();
    let (mut checked, mut compared_words) = (0, 0);
    for row in rows_of("expected_options.jsonl") {
        let types = row["autocorrect"].as_u64().unwrap_or(0);
        if types == 0 || row["fuzzy"].as_u64().unwrap_or(0) != 0 {
            continue;
        }
        let keys = row["keys"].as_str().unwrap();
        if AUTOCORRECT_CPP_ONLY.contains(&(keys, types)) || golden_texts(&row).is_empty() {
            continue;
        }
        let cuts = tables.cut_kbest(keys, types as u32, 9);
        let primary = cuts.first().unwrap_or_else(|| panic!("{keys}（autocorrect={types}）：Rust 没有纠错读法"));
        let words = dictionary.exact_candidates(&primary.syllables(), 100).unwrap();
        let texts: Vec<String> = words.iter().take(30).map(|c| c.text.clone()).collect();
        let label = format!("{keys}（autocorrect={types}，读法 {:?}）", primary.syllables());
        compared_words += assert_same_relative_order(&label, &texts, &golden_texts(&row));
        // 首位必须就是 C++ 的首位：纠错的意义在于把正确的词放到最前。
        assert_eq!(texts.first(), golden_texts(&row).first(), "{label}：首选词不同");
        checked += 1;
    }
    assert!(checked >= 7, "只核对了 {checked} 条纠错输入");
    eprintln!("纠错：核对了 {checked} 条输入、{compared_words} 个词；另有 {} 条走 C++ 的仲裁，不在对照内", AUTOCORRECT_CPP_ONLY.len());
}
