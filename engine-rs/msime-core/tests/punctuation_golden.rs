//! 标点策略对着真实的 C++ `PunctuationPolicy` 核对。基线 `expected_punctuation.jsonl` 由 `msime_punctuation_probe` 导出：
//! 400 个随机字符序列（偏向引号与书名号，掺不支持的字符），每个序列用一个新策略，逐字符比较译文。不需要词库。

use msime_core::punctuation::PunctuationPolicy;
use serde_json::Value;
use std::fs;
use std::path::Path;

#[test]
fn punctuation_matches_cpp() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_punctuation.jsonl");
    let text = fs::read_to_string(path).unwrap();
    let (mut sequences, mut produced, mut nested_inner, mut unmatched_close) = (0, 0, 0, 0);
    for line in text.lines() {
        let record: Value = serde_json::from_str(line).unwrap();
        let mut policy = PunctuationPolicy::new();
        let input = record["input"].as_str().unwrap();
        let expected = record["output"].as_array().unwrap();
        assert_eq!(input.chars().count(), expected.len());
        for (index, (character, want)) in input.chars().zip(expected).enumerate() {
            let got = policy.translate(character);
            assert_eq!(got.map(str::to_string), want.as_str().map(str::to_string), "第 {} 序列 第 {index} 个字符 {character:?} 输入 {input:?}", record["round"]);
            produced += usize::from(got.is_some());
            nested_inner += usize::from(got == Some("〈") || got == Some("〉"));
            unmatched_close += usize::from(character == '>' && got == Some("》") && input[..input.char_indices().nth(index).unwrap().0].matches('<').count() <= input[..input.char_indices().nth(index).unwrap().0].matches('>').count());
        }
        sequences += 1;
    }
    eprintln!("标点：{sequences} 个序列，有译文 {produced}，内层书名号 {nested_inner}，多出来的右书名号 {unmatched_close}");
    assert!(sequences == 400 && produced > 4000 && nested_inner > 100 && unmatched_close > 20);
}
