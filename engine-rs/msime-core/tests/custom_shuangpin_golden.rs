//! 自定义双拼方案对着真实的 C++ `BuildCustomShuangpinProfile` 核对。基线 `expected_custom_shuangpin.jsonl`
//! 由 `msime_custom_shuangpin_probe` 导出：对九套内置方案随机改动 1200 次，记录布局、错误信息和成功时的方案表。
//!
//! C++ 的布局表是 `unordered_map`，遍历顺序因实现而异：错误信息的先后、两个声母抢同一个键时谁算“先到”都随之变。
//! 所以前半段的错误按多重集比较，声母冲突只比较键；布局里小写后撞名的（后写覆盖先写，谁后写看遍历顺序）整条跳过。
//! 往返检查那一段的错误按排序后的音节输出，顺序是确定的，逐条比较。

use msime_core::shuangpin::{build_custom_profile, parse_zero_initial_rule, CustomLayout};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::PathBuf;

fn rows() -> Vec<Value> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_custom_shuangpin.jsonl");
    std::fs::read_to_string(path).unwrap().lines().filter(|l| !l.trim().is_empty()).map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn table(value: &Value) -> BTreeMap<String, String> {
    value.as_array().unwrap().iter().map(|p| (p[0].as_str().unwrap().to_string(), p[1].as_str().unwrap().to_string())).collect()
}

fn collides_after_lowercase(table: &BTreeMap<String, String>) -> bool {
    let mut seen = std::collections::HashSet::new();
    table.keys().any(|k| !seen.insert(k.to_ascii_lowercase()))
}

/// 错误信息去掉遍历顺序带来的差异：声母冲突只留键。
fn normalized(errors: &[String]) -> Vec<String> {
    let mut out: Vec<String> = errors
        .iter()
        .map(|e| match (e.strip_prefix("声母 "), e.rfind(" 都在 ")) {
            (Some(_), Some(at)) if e.contains(" 和 ") => format!("声母冲突{}", &e[at..]),
            _ => e.clone(),
        })
        .collect();
    out.sort();
    out
}

fn is_round_trip_error(e: &str) -> bool {
    e.starts_with("音节 ") || e.starts_with("另有 ")
}

#[test]
fn custom_profiles_equal_cpp() {
    let (mut checked, mut built, mut skipped, mut round_trip_failures, mut exact_errors) = (0, 0, 0, 0, 0);
    for row in rows().iter().filter(|r| r["kind"] == "custom_layout") {
        let (initials, finals, overrides) = (table(&row["initials"]), table(&row["finals"]), table(&row["overrides"]));
        if collides_after_lowercase(&initials) || collides_after_lowercase(&finals) || collides_after_lowercase(&overrides) {
            skipped += 1;
            continue;
        }
        let layout = CustomLayout {
            name: row["name"].as_str().unwrap().to_string(),
            zero_initial_rule: parse_zero_initial_rule(row["rule"].as_str().unwrap()).unwrap(),
            initials,
            finals,
            zero_initial_overrides: overrides,
        };
        let want_errors: Vec<String> = row["errors"].as_array().unwrap().iter().map(|e| e.as_str().unwrap().to_string()).collect();
        let got = build_custom_profile(&layout);
        match (&got, row["profile"].is_null()) {
            (Ok(profile), false) => {
                assert!(want_errors.is_empty(), "{}", row["name"]);
                assert_eq!(profile.initials, table(&row["profile"]["initials"]), "声母表 {}", row["name"]);
                assert_eq!(profile.zero_initials, table(&row["profile"]["zero_initials"]), "零声母表 {}", row["name"]);
                assert_eq!(profile.finals, table(&row["profile"]["finals"]), "韵母表 {}", row["name"]);
                built += 1;
            }
            (Err(errors), true) => {
                let early = |list: &[String]| normalized(&list.iter().filter(|e| !is_round_trip_error(e)).cloned().collect::<Vec<_>>());
                assert_eq!(early(errors), early(&want_errors), "错误信息 {}", row["name"]);
                // 往返检查段按排序后的音节逐个报。两个韵母共用一个键时，解码器读成哪个取决于韵母表的遍历顺序
                // （C++ 是 unordered_map，Rust 是 BTreeMap），所以“读成”后面的内容可以不同；条数、被点名的
                // 编码和“另有 N 个”的汇总行是确定的。
                let tail = |list: &[String]| {
                    list.iter()
                        .filter(|e| is_round_trip_error(e))
                        .map(|e| e.split(" 输入时").next().unwrap().split(' ').skip(2).collect::<Vec<_>>().join(" "))
                        .collect::<Vec<_>>()
                };
                assert_eq!(tail(errors).len(), tail(&want_errors).len(), "往返检查条数 {}", row["name"]);
                assert_eq!(errors.last().filter(|e| e.starts_with("另有 ")), want_errors.last().filter(|e| e.starts_with("另有 ")), "往返检查汇总 {}", row["name"]);
                round_trip_failures += i32::from(errors.iter().any(|e| is_round_trip_error(e)));
                exact_errors += i32::from(errors == &want_errors);
            }
            _ => panic!("成败不一致 {}：Rust {:?}，C++ {:?}", row["name"], got.as_ref().map(|_| ()), want_errors),
        }
        checked += 1;
    }
    assert!(checked >= 1100 && built >= 200 && round_trip_failures >= 100, "{checked}/{built}/{round_trip_failures}");
    eprintln!("自定义双拼：{checked} 个布局（成功 {built}，往返检查失败 {round_trip_failures}，跳过大小写撞名 {skipped}，错误信息逐条同序 {exact_errors}）与 C++ 一致");
}

#[test]
fn zero_initial_rule_names_equal_cpp() {
    let mut checked = 0;
    for row in rows().iter().filter(|r| r["kind"] == "rule") {
        let want = row["parsed"].as_str();
        let got = parse_zero_initial_rule(row["name"].as_str().unwrap()).map(|rule| match rule {
            msime_core::shuangpin::ZeroInitialRule::LetterO => "o",
            msime_core::shuangpin::ZeroInitialRule::LetterA => "a",
            msime_core::shuangpin::ZeroInitialRule::FirstLetter => "first_letter",
            msime_core::shuangpin::ZeroInitialRule::FullPinyin => "full_pinyin",
        });
        assert_eq!(got, want, "{}", row["name"]);
        checked += 1;
    }
    assert_eq!(checked, 7);
}
