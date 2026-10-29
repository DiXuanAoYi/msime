//! 全拼纠错对着真实的 C++ 核对。基线 `expected_quanpin_autocorrect.jsonl` 由 `msime_quanpin_autocorrect_probe` 导出：
//!
//! - `kbest` / `cut1`：`autocorrect_cut_kbest` 与它的 k=1 投影，随机手误输入（漏字、多字、换位、邻键与远键替换、
//!   手动分隔符、大写、超长串）× 16 种类型掩码 × k=1/3/9，逐条比各段（音节、敲的字母、起点、是否纠正、权重）、
//!   纠正边数、权重和、是否带生成式边；
//! - `legal`：合法输入上的手误读法（含零声母音节、罕见合法音节 lia/dia、与切分不一致的输入）；
//! - `reading`：读音对回敲的字母（`correction_cut_for_reading`、`corrected_reading_raw_length`）；
//! - `jianpin`：`looks_like_jianpin_spelling`；
//! - `fault`：长句容错槽位模型（`build_fault_slots`、各槽的纠错目标、是否接管、读音对回原始字母）；
//!   `delvar`：`is_deletion_variant`；`umlaut`：`canonical_umlaut_spelling` 与 `is_jqxy_v_alias`。

use msime_core::autocorrect::{
    autocorrect_cut_detail, build_fault_slots, corrected_reading_raw_length, correction_cut_for_reading, fault_reading_raw_length, fault_slot_correction_targets,
    fault_slots_eligible, is_deletion_variant, legal_input_correction_cuts, looks_like_jianpin_spelling, tables, Cut, FaultSlotKind,
};
use msime_core::quanpin_utils::{canonical_umlaut_spelling, is_jqxy_v_alias};
use serde_json::Value;
use std::collections::HashMap;
use std::fs;
use std::path::Path;

fn records() -> Vec<Value> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_quanpin_autocorrect.jsonl");
    fs::read_to_string(path).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn strings(value: &Value) -> Vec<String> {
    value.as_array().unwrap().iter().map(|s| s.as_str().unwrap().to_string()).collect()
}

fn cut_value(cut: &Cut) -> Value {
    let segments: Vec<Value> = cut.segments.iter().map(|s| serde_json::json!([s.syllable, s.raw, s.start, i32::from(s.corrected), s.weight])).collect();
    serde_json::json!({"segments": segments, "edges": cut.edges, "weight": cut.weight, "generated": i32::from(cut.has_generated)})
}

fn cuts_value(cuts: &[Cut]) -> Value {
    Value::Array(cuts.iter().map(cut_value).collect())
}

#[test]
fn kbest_matches_cpp() {
    let tables = tables();
    let mut counts: HashMap<&str, usize> = HashMap::new();
    let (mut nonempty, mut generated, mut multi) = (0, 0, 0);
    for record in records().iter().filter(|r| r["kind"] == "kbest") {
        let input = record["input"].as_str().unwrap();
        let (mask, k) = (record["mask"].as_u64().unwrap() as u32, record["k"].as_u64().unwrap() as usize);
        let got = cuts_value(&tables.cut_kbest(input, mask, k));
        assert_eq!(got, record["ret"], "kbest {input:?} mask={mask} k={k}");
        *counts.entry("kbest").or_default() += 1;
        nonempty += usize::from(got.as_array().is_some_and(|a| !a.is_empty()));
        generated += usize::from(got.as_array().unwrap().iter().any(|c| c["generated"] == 1));
        multi += usize::from(got.as_array().unwrap().len() > 1);
    }
    for record in records().iter().filter(|r| r["kind"] == "cut1") {
        let input = record["input"].as_str().unwrap();
        let mask = record["mask"].as_u64().unwrap() as u32;
        let detail = autocorrect_cut_detail(input, mask);
        assert_eq!(cut_value(&detail), record["ret"], "cut1 {input:?} mask={mask}");
        assert_eq!(detail.syllables(), strings(&record["plain"]), "plain {input:?} mask={mask}");
        *counts.entry("cut1").or_default() += 1;
    }
    eprintln!("纠错 k-best：{counts:?}，非空 {nonempty}，含生成式读法 {generated}，多条读法 {multi}");
    assert!(nonempty > 1500 && generated > 300 && multi > 800);
}

#[test]
fn legal_input_corrections_match_cpp() {
    let (mut checked, mut nonempty, mut rare) = (0, 0, 0);
    for record in records().iter().filter(|r| r["kind"] == "legal") {
        let (pinyin, segments) = (record["pinyin"].as_str().unwrap(), strings(&record["segments"]));
        let (mask, k) = (record["mask"].as_u64().unwrap() as u32, record["k"].as_u64().unwrap() as usize);
        let got = cuts_value(&legal_input_correction_cuts(pinyin, &segments, mask, k));
        assert_eq!(got, record["ret"], "legal {pinyin:?} {segments:?} mask={mask} k={k}");
        checked += 1;
        nonempty += usize::from(!got.as_array().unwrap().is_empty());
        rare += usize::from(got.as_array().unwrap().iter().any(|c| c["segments"].as_array().unwrap().iter().any(|s| s[1] == s[0] && s[3] == 1 || s[0] == "lai" || s[0] == "dai")));
    }
    eprintln!("合法输入纠错：{checked} 条，非空 {nonempty}，含罕见音节换位 {rare}");
    assert!(checked >= 2500 && nonempty > 400 && rare > 20);
}

#[test]
fn readings_align_with_the_typed_letters() {
    let (mut checked, mut with_cut, mut with_length) = (0, 0, 0);
    for record in records().iter().filter(|r| r["kind"] == "reading") {
        let (input, reading) = (record["input"].as_str().unwrap(), strings(&record["reading"]));
        let mask = record["mask"].as_u64().unwrap() as u32;
        let cut = correction_cut_for_reading(input, &reading, mask);
        let got_cut = cut.as_ref().map_or(Value::Null, cut_value);
        assert_eq!(got_cut, record["cut"], "reading cut {input:?} {reading:?} mask={mask}");
        let raw_length = corrected_reading_raw_length(input, &reading, mask).map_or(Value::Null, |n| serde_json::json!(n));
        assert_eq!(raw_length, record["raw_length"], "raw length {input:?} {reading:?} mask={mask}");
        checked += 1;
        with_cut += usize::from(cut.is_some());
        with_length += usize::from(!raw_length.is_null());
    }
    eprintln!("读音对回字母：{checked} 条，有对应切分 {with_cut}，有原始长度 {with_length}");
    assert!(checked >= 2500 && with_cut > 500 && with_length > 400);
}

#[test]
fn jianpin_shapes_match_cpp() {
    let (mut checked, mut yes) = (0, 0);
    for record in records().iter().filter(|r| r["kind"] == "jianpin") {
        let input = record["input"].as_str().unwrap();
        let want = record["ret"].as_bool().unwrap();
        assert_eq!(looks_like_jianpin_spelling(input), want, "jianpin {input:?}");
        checked += 1;
        yes += usize::from(want);
    }
    eprintln!("简拼形状：{checked} 条，判为简拼 {yes}");
    assert!(checked >= 1500 && yes > 300 && yes < checked - 300);
}

#[test]
fn fault_slot_model_matches_cpp() {
    let (mut eligible, mut aligned, mut with_targets, mut total) = (0, 0, 0, 0);
    for record in records().iter().filter(|r| r["kind"] == "fault") {
        let input = record["input"].as_str().unwrap();
        let mask = record["mask"].as_u64().unwrap() as u32;
        let slots = build_fault_slots(input);
        let kind = |k: FaultSlotKind| match k {
            FaultSlotKind::Determining => 0,
            FaultSlotKind::SingleInitial => 1,
            FaultSlotKind::Fault => 2,
        };
        let got_slots = Value::Array(slots.iter().map(|s| serde_json::json!([s.raw_text, s.base_syllable, kind(s.kind)])).collect());
        assert_eq!(got_slots, record["slots"], "slots {input:?}");
        let got_targets = Value::Array(
            slots
                .iter()
                .map(|s| Value::Array(fault_slot_correction_targets(&s.raw_text, mask).into_iter().map(|(syllable, weight)| serde_json::json!([syllable, weight])).collect()))
                .collect(),
        );
        assert_eq!(got_targets, record["targets"], "targets {input:?} mask={mask}");
        assert_eq!(fault_slots_eligible(&slots, mask), record["eligible"].as_bool().unwrap(), "eligible {input:?} mask={mask}");
        let reading = strings(&record["reading"]);
        let raw_length = fault_reading_raw_length(input, &reading);
        assert_eq!(raw_length.map_or(Value::Null, |n| serde_json::json!(n)), record["raw_length"], "raw_length {input:?} {reading:?}");
        total += 1;
        eligible += usize::from(fault_slots_eligible(&slots, mask));
        aligned += usize::from(raw_length.is_some());
        with_targets += usize::from(got_targets.as_array().unwrap().iter().any(|t| !t.as_array().unwrap().is_empty()));
    }
    for record in records().iter().filter(|r| r["kind"] == "delvar") {
        let (raw, reading) = (record["raw"].as_str().unwrap(), record["reading"].as_str().unwrap());
        assert_eq!(is_deletion_variant(raw, reading), record["ret"].as_bool().unwrap(), "delvar {raw:?} {reading:?}");
    }
    for record in records().iter().filter(|r| r["kind"] == "umlaut") {
        let syllable = record["syllable"].as_str().unwrap();
        assert_eq!(canonical_umlaut_spelling(syllable), record["ret"].as_str().unwrap(), "umlaut {syllable:?}");
        assert_eq!(is_jqxy_v_alias(syllable), record["alias"].as_bool().unwrap(), "alias {syllable:?}");
    }
    eprintln!("容错槽位：{total} 条，接管 {eligible}，读音对回 {aligned}，带纠错目标 {with_targets}");
    assert!(total >= 2500 && eligible > 300 && aligned > 300 && with_targets > 50);
}
