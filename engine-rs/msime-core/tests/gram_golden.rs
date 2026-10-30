//! octagram `.gram` 读取器对着真实的 C++ `gram::GramDb` 核对。真模型约 390MB、另外下载，所以用探针现建的合成模型
//! `engine/contracts/golden/synthetic.gram`（格式与真模型相同）；基线 `expected_gram.jsonl` 由 `msime_gram_probe` 导出。

use msime_core::gram::{self, GramDb, GrammarConfig, QueryStats};
use serde_json::Value;
use std::path::PathBuf;

fn golden(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden").join(name)
}

fn rows(kind: &str) -> Vec<Value> {
    std::fs::read_to_string(golden("expected_gram.jsonl"))
        .unwrap()
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["kind"] == kind)
        .collect()
}

fn unhex(text: &Value) -> Vec<u8> {
    let text = text.as_str().unwrap();
    (0..text.len() / 2).map(|i| u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).unwrap()).collect()
}

#[test]
fn encoding_and_codepoint_helpers_equal_cpp() {
    let mut checked = 0;
    for row in rows("encode") {
        let text = unhex(&row["text"]);
        let encoded = gram::encode(&text);
        assert_eq!(encoded, unhex(&row["encoded"]), "编码 {:?}", row["text"]);
        assert_eq!(gram::codepoint_count(&text) as u64, row["count"].as_u64().unwrap(), "码点数 {:?}", row["text"]);
        assert_eq!(gram::unicode_length(&encoded, encoded.len()) as u64, row["unicode_length"].as_u64().unwrap());
        assert_eq!(gram::first_codepoints(&text, 3), unhex(&row["first3"]).as_slice());
        assert_eq!(gram::last_codepoints(&text, 3), unhex(&row["last3"]).as_slice());
        checked += 1;
    }
    assert!(checked >= 300);
    eprintln!("octagram 编码与码点辅助：{checked} 个文本（含非法字节和 NUL）与 C++ 一致");
}

#[test]
fn queries_on_the_synthetic_model_equal_cpp() {
    let db = GramDb::open(&golden("synthetic.gram")).expect("打不开合成模型");
    let (mut checked, mut hits, mut rear_hits, mut weak, mut exact) = (0, 0, 0, 0, 0);
    for row in rows("query") {
        let c = row["config"].as_array().unwrap();
        let config = GrammarConfig {
            collocation_max_length: c[0].as_i64().unwrap() as i32,
            collocation_min_length: c[1].as_i64().unwrap() as i32,
            collocation_penalty: c[2].as_f64().unwrap(),
            non_collocation_penalty: c[3].as_f64().unwrap(),
            weak_collocation_penalty: c[4].as_f64().unwrap(),
            rear_penalty: c[5].as_f64().unwrap(),
        };
        let rear = row["rear"].as_bool().unwrap();
        let mut stats = QueryStats::default();
        let score = db.query_bytes(&unhex(&row["context"]), &unhex(&row["word"]), rear, &config, Some(&mut stats));
        let want = row["score"].as_f64().unwrap();
        // MSVC 把 `值 / 10000.0` 编成了乘倒数，和除法有时差最后一位（1 ulp）；分数只用来比大小，
        // 这里按 1e-12 比，并统计逐位相同的比例，别让误差悄悄变大。
        assert!((score - want).abs() < 1e-12, "查询 {row}：{score} ≠ {want}");
        exact += i32::from(score.to_bits() == want.to_bits());
        let s = row["stats"].as_array().unwrap();
        assert_eq!(
            (stats.context_windows, stats.matched_windows, stats.matches),
            (s[0].as_i64().unwrap() as i32, s[1].as_i64().unwrap() as i32, s[2].as_i64().unwrap() as i32),
            "统计 {row}"
        );
        assert!((stats.best_value - s[3].as_f64().unwrap()).abs() < 1e-12, "最佳值 {row}");
        checked += 1;
        hits += i32::from(stats.matches > 0);
        rear_hits += i32::from(rear && score > config.non_collocation_penalty);
        weak += i32::from(score == stats.best_value + config.weak_collocation_penalty && stats.matches > 0);
    }
    assert!(checked >= 2500 && hits > 800 && rear_hits > 100 && weak > 5, "{checked}/{hits}/{rear_hits}/{weak}");
    assert!(exact * 100 >= checked * 95, "逐位相同的只有 {exact} / {checked}");
    eprintln!("octagram 查询：{checked} 次（命中 {hits}，句尾项抬分 {rear_hits}，吃弱搭配惩罚 {weak}）分数与统计与 C++ 一致（误差 1e-12 内，逐位相同 {exact} 次）");
}

#[test]
fn rejects_files_that_are_not_grammar_models() {
    assert!(GramDb::from_bytes(vec![0; 10]).is_err());
    let mut bad = std::fs::read(golden("synthetic.gram")).unwrap();
    bad[0] = b'X';
    assert!(GramDb::from_bytes(bad).is_err());
    let mut truncated = std::fs::read(golden("synthetic.gram")).unwrap();
    truncated.truncate(1000);
    assert!(GramDb::from_bytes(truncated).is_err());
}
