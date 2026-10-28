//! Google 拼音解码器对着真实的 C++ `PinyinDecoder` 核对。基线 `expected_google_pinyin.jsonl` 由 `msime_google_pinyin_probe`
//! 在随包 `dict_pinyin.dat` 上导出：
//!
//! - `qsort`：运行库 `qsort` 排带大量相同键的数组后的次序（同分候选的先后全由它决定），不需要词库；
//! - `splid`：`SpellingParser::get_splid_by_str` 对每个音节、各个前缀、带隔音符/大写/乱码的串给出的编号和“没打完”；
//! - `sentence`：从头解一个拼音串（含简拼、没打完的末音节、乱码、NUL、超过 64 个音节、127/128/129 字节边界），
//!   约三成带句中辅助码约束；
//! - `typing`：同一个串逐键输入再逐键删回去（C++ 用增量解码，Rust 每次从头解，所以也验证两者一致）。
//!
//! 要 `installer/app_data/dict_pinyin.dat`（或 `MSIME_APP_DATA` 指向的目录），缺了就跳过词库相关的部分。

use msime_core::google_pinyin::{DecoderConstraint, PinyinDecoder};
use msime_core::msvc_sort::qsort_by;
use serde_json::Value;
use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};

fn app_data() -> PathBuf {
    std::env::var_os("MSIME_APP_DATA").map(PathBuf::from).unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../installer/app_data"))
}

fn records() -> Vec<Value> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_google_pinyin.jsonl");
    fs::read_to_string(path).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn decode(decoder: &PinyinDecoder, pinyin: &str, constraints: &[(usize, HashSet<String>)]) -> String {
    let accepts: Vec<Box<dyn Fn(&str) -> bool + '_>> = constraints.iter().map(|(_, allowed)| Box::new(move |hanzi: &str| allowed.contains(hanzi)) as Box<dyn Fn(&str) -> bool>).collect();
    let list: Vec<DecoderConstraint> = constraints.iter().zip(&accepts).map(|((offset, _), accept)| DecoderConstraint { pinyin_offset: *offset, accept: accept.as_ref() }).collect();
    decoder.sentence(pinyin, &list)
}

#[test]
fn qsort_matches_the_c_runtime() {
    let mut arrays = 0usize;
    for record in records().iter().filter(|r| r["kind"] == "qsort") {
        let keys: Vec<u32> = record["keys"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap() as u32).collect();
        let want: Vec<usize> = record["ret"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap() as usize).collect();
        let mut items: Vec<(u32, usize)> = keys.iter().copied().zip(0..).collect();
        qsort_by(&mut items, |a, b| a.0.cmp(&b.0) as i32);
        let got: Vec<usize> = items.iter().map(|i| i.1).collect();
        assert_eq!(got, want, "keys = {keys:?}");
        arrays += 1;
    }
    assert!(arrays >= 500);
}

fn load() -> Option<PinyinDecoder> {
    let path = app_data().join("dict_pinyin.dat");
    if !path.exists() {
        eprintln!("跳过：{} 不存在", path.display());
        return None;
    }
    Some(PinyinDecoder::load_file(&path).expect("随包词库应能载入"))
}

#[test]
fn spelling_parser_matches_cpp() {
    let Some(decoder) = load() else { return };
    let mut checked = 0usize;
    for record in records().iter().filter(|r| r["kind"] == "splid") {
        let text = record["s"].as_str().unwrap();
        let got = decoder.spelling_id(text);
        let want = (record["id"].as_u64().unwrap() as u16, record["pre"].as_bool().unwrap());
        assert_eq!(got, want, "音节串 {text:?}");
        checked += 1;
    }
    assert!(checked > 4000);
}

#[test]
fn sentences_match_cpp() {
    let Some(decoder) = load() else { return };
    let (mut total, mut constrained, mut nonempty, mut changed) = (0usize, 0usize, 0usize, 0usize);
    for record in records().iter().filter(|r| r["kind"] == "sentence") {
        let pinyin = record["pinyin"].as_str().unwrap();
        let constraints: Vec<(usize, HashSet<String>)> =
            record["cons"].as_array().unwrap().iter().map(|c| (c[0].as_u64().unwrap() as usize, c[1].as_str().unwrap().chars().map(|ch| ch.to_string()).collect())).collect();
        let want = record["ret"].as_str().unwrap();
        assert_eq!(decode(&decoder, pinyin, &constraints), want, "拼音 {pinyin:?} 约束 {:?}", record["cons"]);
        total += 1;
        constrained += usize::from(!constraints.is_empty());
        nonempty += usize::from(!want.is_empty());
        if !constraints.is_empty() && !want.is_empty() && decode(&decoder, pinyin, &[]) != want {
            changed += 1;
        }
    }
    eprintln!("Google 整句：{total} 条，带约束 {constrained}，解出句子 {nonempty}，约束改变了结果的 {changed}");
    assert!(total >= 7000 && constrained > 1500 && nonempty > 5000 && changed > 100);
}

#[test]
fn incremental_typing_matches_a_fresh_decode() {
    let Some(decoder) = load() else { return };
    let mut steps = 0usize;
    for record in records().iter().filter(|r| r["kind"] == "typing") {
        let pinyin = record["pinyin"].as_str().unwrap();
        let rets: Vec<&str> = record["rets"].as_array().unwrap().iter().map(|v| v.as_str().unwrap()).collect();
        let mut lengths: Vec<usize> = (1..=pinyin.len()).collect();
        lengths.extend((1..pinyin.len()).rev());
        assert_eq!(lengths.len(), rets.len());
        for (length, want) in lengths.into_iter().zip(rets) {
            assert_eq!(decode(&decoder, &pinyin[..length], &[]), want, "拼音 {:?}", &pinyin[..length]);
            steps += 1;
        }
    }
    assert!(steps > 5000);
}
