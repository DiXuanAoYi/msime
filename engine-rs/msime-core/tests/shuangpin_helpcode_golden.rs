//! 双拼与辅助码对着真实的 C++ 实现核对。基线 `engine/contracts/golden/expected_helpshuang.jsonl`
//! 由 `msime_helpshuang_probe` 导出：方案表、全部一到两个字符的码的解码、随机串的分段、六套辅助码表的取码与匹配。
//! 辅助码表直接读仓库里的 `engine/helpcode/helpcodes/*.txt`。

use msime_core::helpcode::{self, SingleMatch, BUILTIN_SCHEMAS};
use msime_core::shuangpin::{self, PROFILE_NAMES};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;

fn engine() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine")
}

fn rows(kind: &str) -> Vec<Value> {
    let text = std::fs::read_to_string(engine().join("contracts/golden/expected_helpshuang.jsonl")).unwrap();
    text.lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["kind"] == kind)
        .collect()
}

fn string_map(value: &Value) -> BTreeMap<String, String> {
    value.as_object().unwrap().iter().map(|(k, v)| (k.clone(), v.as_str().unwrap().to_string())).collect()
}

fn all_codes() -> Vec<String> {
    let alphabet: Vec<char> = "abcdefghijklmnopqrstuvwxyz;".chars().collect();
    let mut codes = Vec::new();
    for a in &alphabet {
        codes.push(a.to_string());
        for b in &alphabet {
            codes.push(format!("{a}{b}"));
        }
    }
    codes
}

#[test]
fn the_nine_profile_tables_equal_the_cpp_tables() {
    let expected = rows("profile");
    assert_eq!(expected.len(), PROFILE_NAMES.len());
    for row in expected {
        let name = row["name"].as_str().unwrap();
        let profile = shuangpin::profile(name);
        assert_eq!(profile.name, row["resolved"].as_str().unwrap());
        assert_eq!(profile.initials, string_map(&row["initials"]), "{name} 声母");
        assert_eq!(profile.zero_initials, string_map(&row["zero_initials"]), "{name} 零声母");
        assert_eq!(profile.finals, string_map(&row["finals"]), "{name} 韵母");
        assert_eq!(profile.uses_semicolon_final(), row["semicolon"].as_bool().unwrap(), "{name} 分号韵母");
    }
}

#[test]
fn unknown_profile_names_fall_back_to_xiaohe() {
    assert_eq!(shuangpin::profile("no-such-layout").name, "xiaohe");
}

#[test]
fn every_one_or_two_character_code_decodes_like_cpp() {
    let codes = all_codes();
    let (mut checked, mut decoded) = (0, 0);
    for row in rows("decode") {
        let name = row["profile"].as_str().unwrap();
        let profile = shuangpin::profile(name);
        let expected: BTreeMap<String, (String, bool)> = row["rows"]
            .as_array()
            .unwrap()
            .iter()
            .map(|r| (r[0].as_str().unwrap().to_string(), (r[1].as_str().unwrap().to_string(), r[2].as_bool().unwrap())))
            .collect();
        for code in &codes {
            let (syllable, accepted) = expected.get(code).cloned().unwrap_or_default();
            assert_eq!(shuangpin::to_pinyin(code, profile), syllable, "{name}：{code} 解码");
            assert_eq!(shuangpin::is_accepted_syllable_code(code, profile), accepted, "{name}：{code} 是否被分段接受");
            checked += 1;
            decoded += usize::from(!syllable.is_empty());
        }
    }
    assert_eq!(checked, 9 * 756);
    assert!(decoded > 9 * 380, "解出来的码只有 {decoded} 个");
    eprintln!("双拼解码：{checked} 个码（9 套方案 × 756），其中能解成音节 {decoded} 个，与 C++ 逐个相同");
}

#[test]
fn a_code_never_has_two_valid_readings() {
    // C++ 里韵母表是无序表，一个键对应多个韵母时“后一个合法的赢”取决于遍历顺序；
    // 只要没有哪个码有两个合法读法，顺序就不影响结果，Rust 的有序遍历和 C++ 才等价。
    for name in PROFILE_NAMES {
        let profile = shuangpin::profile(name);
        for a in 'a'..='z' {
            let initial = profile
                .initials
                .iter()
                .find(|(_, code)| **code == a.to_string())
                .map(|(source, _)| source.clone())
                .unwrap_or_else(|| a.to_string());
            for b in "abcdefghijklmnopqrstuvwxyz;".chars() {
                let valid: BTreeSet<String> = profile
                    .finals
                    .iter()
                    .filter(|(_, key)| **key == b.to_string())
                    .map(|(final_part, _)| {
                        let final_part = if final_part == "v" && matches!(initial.as_str(), "j" | "q" | "x" | "y") {
                            "u"
                        } else {
                            final_part.as_str()
                        };
                        format!("{initial}{final_part}")
                    })
                    .filter(|syllable| shuangpin::decodable_syllables().contains(syllable.as_str()))
                    .collect();
                assert!(valid.len() <= 1, "{name}：{a}{b} 有多个合法读法 {valid:?}");
            }
        }
        // 声母和零声母的码也必须唯一，否则“取第一个匹配”依赖遍历顺序。
        for table in [&profile.initials, &profile.zero_initials] {
            let unique: BTreeSet<&String> = table.values().collect();
            assert_eq!(unique.len(), table.len(), "{name}：声母或零声母的码有重复");
        }
    }
}

#[test]
fn segmentation_conversion_and_full_help_match_cpp() {
    let mut checked = 0;
    for row in rows("segment") {
        let name = row["profile"].as_str().unwrap();
        let profile = shuangpin::profile(name);
        for r in row["rows"].as_array().unwrap() {
            let input = r[0].as_str().unwrap();
            assert_eq!(shuangpin::segment(input, profile), r[1].as_str().unwrap(), "{name}：{input} 分段");
            let segmented = shuangpin::segment(&input.to_ascii_lowercase(), profile);
            let converted =
                if segmented.is_empty() { String::new() } else { shuangpin::segmented_to_pinyin(&segmented, profile) };
            assert_eq!(converted, r[2].as_str().unwrap(), "{name}：{input} 转全拼");
            assert_eq!(shuangpin::is_full_help_mode(input, profile), r[3].as_bool().unwrap(), "{name}：{input} 全码辅助");
            assert_eq!(shuangpin::full_help_codes(input), r[4].as_str().unwrap(), "{name}：{input} 辅助码");
            checked += 1;
        }
    }
    assert!(checked > 800, "只核对了 {checked} 条");
    eprintln!("双拼分段：{checked} 条输入（9 套方案），分段、转全拼、全码辅助判断与 C++ 逐条相同");
}

fn load_keymap(schema: &str) -> helpcode::Keymap {
    let file = BUILTIN_SCHEMAS.iter().find(|(name, _)| *name == schema).unwrap().1;
    helpcode::parse_keymap(&std::fs::read_to_string(engine().join("helpcode").join(file)).unwrap())
}

fn letter(m: SingleMatch) -> char {
    match m {
        SingleMatch::First => 'F',
        SingleMatch::Last => 'L',
        SingleMatch::Both => 'B',
        SingleMatch::None => 'N',
    }
}

#[test]
fn helpcode_tables_codes_matches_reorders_and_filters_equal_cpp() {
    let (mut words_checked, mut with_code) = (0, 0);
    for row in rows("helpcode") {
        let schema = row["schema"].as_str().unwrap();
        let keymap = load_keymap(schema);
        assert_eq!(keymap.len() as u64, row["size"].as_u64().unwrap(), "{schema}：码表条数");

        for r in row["rows"].as_array().unwrap() {
            let word = r[0].as_str().unwrap();
            assert_eq!(helpcode::compute_helpcodes(word, false, &keymap), r[1].as_str().unwrap(), "{schema}：{word} 取码");
            assert_eq!(helpcode::compute_helpcodes(word, true, &keymap), r[2].as_str().unwrap(), "{schema}：{word} 取码（全大写）");
            let singles: String =
                ('a'..='z').map(|c| letter(helpcode::match_single_helpcode(word, &c.to_string(), &keymap))).collect();
            assert_eq!(singles, r[3].as_str().unwrap(), "{schema}：{word} 单码匹配");
            let mut doubles = Vec::new();
            for a in 'a'..='z' {
                for b in 'a'..='z' {
                    if helpcode::matches_double_helpcodes(word, &format!("{a}{b}"), &keymap) {
                        doubles.push(format!("{a}{b}"));
                    }
                }
            }
            let expected: Vec<String> = r[4].as_array().unwrap().iter().map(|v| v.as_str().unwrap().to_string()).collect();
            assert_eq!(doubles, expected, "{schema}：{word} 双码匹配");
            assert_eq!(helpcode::first_han_char(word), r[5].as_str().unwrap(), "{schema}：{word} 首个汉字");
            assert_eq!(helpcode::last_han_char(word), r[6].as_str().unwrap(), "{schema}：{word} 末个汉字");
            assert_eq!(helpcode::count_han_chars(word) as u64, r[7].as_u64().unwrap(), "{schema}：{word} 字数");
            assert_eq!(helpcode::count_utf8_chars(word) as u64, r[8].as_u64().unwrap(), "{schema}：{word} utf8 字数");
            words_checked += 1;
            with_code += usize::from(!r[1].as_str().unwrap().is_empty());
        }

        // 重排与筛选：列表里第 i 个词的权重是 100 - i，输出里的数字就是原下标。
        let list: Vec<(usize, String)> =
            row["list"].as_array().unwrap().iter().enumerate().map(|(i, w)| (i, w.as_str().unwrap().to_string())).collect();
        let weights = |items: Vec<(usize, String)>| -> Vec<u64> { items.iter().map(|x| 100 - x.0 as u64).collect() };
        let numbers = |value: &Value| -> Vec<u64> { value.as_array().unwrap().iter().map(|v| v.as_u64().unwrap()).collect() };
        for r in row["reorders"].as_array().unwrap() {
            let code = r[0].as_str().unwrap();
            let order = weights(helpcode::reorder_with_single_helpcode(&list, |x| x.1.as_str(), code, &keymap));
            assert_eq!(order, numbers(&r[1]), "{schema}：单码 {code} 重排");
        }
        for r in row["filters"].as_array().unwrap() {
            let codes = r[0].as_str().unwrap();
            let kept = weights(helpcode::filter_with_double_helpcodes(&list, |x| x.1.as_str(), codes, &keymap));
            assert_eq!(kept, numbers(&r[1]), "{schema}：双码 {codes} 筛选");
        }
    }
    assert!(words_checked > 1500 && with_code > 1000, "核对了 {words_checked} 个词、其中有码 {with_code} 个");
    eprintln!("辅助码：6 套码表，{words_checked} 个词（有码 {with_code} 个），取码、单双码匹配、重排、筛选与 C++ 逐项相同");
}

#[test]
fn trailing_capital_modes_match_cpp() {
    for row in rows("modes") {
        for r in row["rows"].as_array().unwrap() {
            let sample = r[0].as_str().unwrap();
            assert_eq!(helpcode::is_single_help_mode(sample), r[1].as_bool().unwrap(), "单码模式 {sample:?}");
            assert_eq!(helpcode::is_double_help_mode(sample), r[2].as_bool().unwrap(), "双码模式 {sample:?}");
        }
    }
}
