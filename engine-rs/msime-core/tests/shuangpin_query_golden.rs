//! 双拼输入串处理对着真实的 C++ `shuangpin::*` 核对。基线 `expected_shuangpin_query.jsonl` 由
//! `msime_shuangpin_query_probe` 导出：九套内置方案 × 260 条随机输入串（方案里真能解出音节的两键码，
//! 掺入单键、大写辅助码、反引号段、手动分隔符和杂字符），每条记录分段、转全拼、分隔符处理、全码辅助长度、
//! 大小写还原、句中辅助码解析，以及光标在每个位置时各个“能否接反引号/第二码/分号”的判断。

use msime_core::mid_sentence;
use msime_core::shuangpin::profile;
use msime_core::shuangpin_query as q;
use serde_json::Value;
use std::path::PathBuf;

fn rows(kind: &str) -> Vec<Value> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_shuangpin_query.jsonl");
    std::fs::read_to_string(path)
        .unwrap()
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["kind"] == kind)
        .collect()
}

fn usizes(value: &Value) -> Vec<usize> {
    value.as_array().unwrap().iter().map(|v| v.as_u64().unwrap() as usize).collect()
}

fn bools(value: &Value) -> Vec<bool> {
    value.as_array().unwrap().iter().map(|v| v.as_u64().unwrap() != 0).collect()
}

#[test]
fn input_processing_equals_cpp() {
    let (mut checked, mut with_blocks, mut active, mut decorated_changed, mut cased) = (0, 0, 0, 0, 0);
    for row in rows("query") {
        let raw = row["raw"].as_str().unwrap();
        let upper = row["upper"].as_bool().unwrap();
        let profile = profile(row["profile"].as_str().unwrap());
        let ctx = format!("{} {raw:?} upper={upper}", row["profile"]);

        let segmented = q::segment_input(raw, profile);
        assert_eq!(segmented, row["segment"].as_str().unwrap(), "分段 {ctx}");
        assert_eq!(q::segment_raw_boundaries(raw, profile), usizes(&row["boundaries"]), "音节边界 {ctx}");
        assert_eq!(q::normalize_input_with_delimiters(raw, profile), row["quanpin_delims"].as_str().unwrap(), "转全拼 {ctx}");
        assert_eq!(q::normalize_input(raw, profile), row["normalize"].as_str().unwrap(), "去分隔符 {ctx}");

        let effective = q::effective_input_length(raw);
        assert_eq!(effective as u64, row["effective"].as_u64().unwrap(), "有效长度 {ctx}");
        let want_prefix = usizes(&row["prefix_lengths"]);
        let want_trims: Vec<&str> = row["trims"].as_array().unwrap().iter().map(|v| v.as_str().unwrap()).collect();
        for k in 0..=effective + 1 {
            assert_eq!(q::raw_length_for_effective_prefix(raw, k), want_prefix[k], "有效前缀 {k} {ctx}");
            assert_eq!(q::trim_trailing_letters_preserve_delimiters(raw, k), want_trims[k], "去末尾 {k} {ctx}");
        }

        let lower = raw.to_ascii_lowercase();
        assert_eq!(q::is_complete_input(raw, profile), row["complete"].as_bool().unwrap(), "完整输入 {ctx}");
        assert_eq!(q::is_complete_input(&lower, profile), row["complete_lower"].as_bool().unwrap(), "完整输入（小写）{ctx}");
        let active_length = q::detect_active_double_helpcode_length(&lower, raw, profile);
        assert_eq!(active_length as u64, row["active"].as_u64().unwrap(), "双辅助码长度 {ctx}");
        active += i32::from(active_length > 0);
        let with_cases = q::apply_segmentation_cases(&q::segment_input(&lower, profile), raw);
        assert_eq!(with_cases, row["cases"].as_str().unwrap(), "还原大小写 {ctx}");
        cased += i32::from(with_cases != with_cases.to_ascii_lowercase());

        assert_eq!(q::has_mid_sentence_helpcode(raw, upper), row["has_mid"].as_bool().unwrap(), "有句中辅助码 {ctx}");
        let parsed = q::parse_mid_sentence_helpcodes(raw, profile, upper);
        let want = &row["parsed"];
        assert_eq!(parsed.input, want["input"].as_str().unwrap(), "解析后的输入 {ctx}");
        assert_eq!(parsed.source_index, usizes(&want["source_index"]), "源下标 {ctx}");
        let helpcodes: Vec<(usize, u8, u8)> = parsed.helpcodes.iter().map(|h| (h.syllable, h.first, h.second)).collect();
        let want_helpcodes: Vec<(usize, u8, u8)> = want["helpcodes"]
            .as_array()
            .unwrap()
            .iter()
            .map(|h| (h[0].as_u64().unwrap() as usize, h[1].as_u64().unwrap() as u8, h[2].as_u64().unwrap() as u8))
            .collect();
        assert_eq!(helpcodes, want_helpcodes, "约束 {ctx}");
        let decorations: Vec<(usize, String)> = want["decorations"].as_array().unwrap().iter().map(|d| (d[0].as_u64().unwrap() as usize, d[1].as_str().unwrap().to_string())).collect();
        assert_eq!(parsed.decorations, decorations, "装饰 {ctx}");
        let parsed_segmentation = q::segment_input(&parsed.input.to_ascii_lowercase(), profile);
        let decorated = q::decorate_mid_sentence_segmentation(&parsed_segmentation, raw, profile, upper);
        assert_eq!(decorated, row["decorated"].as_str().unwrap(), "装饰后的切分 {ctx}");
        decorated_changed += i32::from(decorated != parsed_segmentation);
        with_blocks += i32::from(!parsed.decorations.is_empty());

        assert_eq!(q::accepts_mid_sentence_helpcode_marker(raw, upper), row["accepts_marker"].as_bool().unwrap(), "末尾接反引号 {ctx}");
        let (marker_at, semicolon_at, second_at) = (bools(&row["marker_at"]), bools(&row["semicolon_at"]), bools(&row["second_at"]));
        for caret in 0..=raw.len() + 1 {
            assert_eq!(q::accepts_mid_sentence_helpcode_marker_at(raw, caret, upper), marker_at[caret], "光标 {caret} 接反引号 {ctx}");
            assert_eq!(mid_sentence::accepts_semicolon_final_at(raw.as_bytes(), caret, upper), semicolon_at[caret], "光标 {caret} 分号 {ctx}");
            assert_eq!(mid_sentence::accepts_second_code_at(raw.as_bytes(), caret, upper), second_at[caret], "光标 {caret} 第二码 {ctx}");
        }
        assert_eq!(q::accepts_mid_sentence_second_code(raw, b'A', upper), row["second_code"].as_bool().unwrap(), "末尾接第二码 {ctx}");
        assert_eq!(mid_sentence::chunk_start(raw.as_bytes(), upper) as u64, row["chunk_start"].as_u64().unwrap(), "节起点 {ctx}");
        checked += 1;
    }
    assert!(checked >= 2300 && with_blocks > 500 && active > 80 && decorated_changed > 300 && cased > 100, "{checked}/{with_blocks}/{active}/{decorated_changed}/{cased}");
    eprintln!("双拼输入串：{checked} 条（含辅助码段 {with_blocks}，双辅助码状态 {active}，装饰改动 {decorated_changed}，带大写还原 {cased}）的分段、转全拼、边界、句中辅助码解析与各光标判断与 C++ 一致");
}

#[test]
fn syllable_helpcode_match_and_signature_equal_cpp() {
    let mut checked = 0;
    for row in rows("match") {
        let helpcode = q::SyllableHelpcode {
            syllable: row["syllable"].as_u64().unwrap() as usize,
            first: row["first"].as_u64().unwrap() as u8,
            second: row["second"].as_u64().unwrap() as u8,
        };
        assert_eq!(helpcode.matches(row["code"].as_str().unwrap()), row["matches"].as_bool().unwrap(), "{row}");
        let other = q::SyllableHelpcode { syllable: 3, first: b'a', second: b'b' };
        assert_eq!(q::syllable_helpcodes_signature(&[helpcode, other]), row["signature"].as_str().unwrap(), "{row}");
        checked += 1;
    }
    assert_eq!(checked, 21);
}
