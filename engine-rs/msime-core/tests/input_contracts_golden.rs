//! 输入形状契约对着真实的 C++ 头文件核对。基线 `expected_input_contracts.jsonl` 由 `msime_input_contracts_probe` 导出：
//! 2500 条日期时间输入（随机串 × 光标 × 字符：能否接受、是否还能接成某种形状、写完时的形状与段数），
//! 2500 条直接辅助码按键（斜杠、分号韵母能否接受）。不需要词库。

use msime_core::input_contracts::{self, DateTimeShape};
use serde_json::Value;
use std::fs;
use std::path::Path;

fn shape_number(shape: DateTimeShape) -> u64 {
    match shape {
        DateTimeShape::None => 0,
        DateTimeShape::Date => 1,
        DateTimeShape::DateTime => 2,
        DateTimeShape::YearMonthDay => 3,
        DateTimeShape::YearMonth => 4,
        DateTimeShape::MonthDay => 5,
        DateTimeShape::Time => 6,
    }
}

#[test]
fn input_contracts_match_cpp() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_input_contracts.jsonl");
    let text = fs::read_to_string(path).unwrap();
    let (mut dates, mut accepted, mut shapes, mut directs, mut slashes, mut semicolons) = (0, 0, 0, 0, 0, 0);
    for line in text.lines() {
        let record: Value = serde_json::from_str(line).unwrap();
        let input = record["text"].as_str().unwrap().as_bytes();
        let caret = record["caret"].as_u64().unwrap() as usize;
        match record["kind"].as_str().unwrap() {
            "date" => {
                let ch = record["ch"].as_u64().unwrap() as u8;
                assert_eq!(u64::from(input_contracts::date_time_accepts_at(input, caret, ch)), record["accepts"].as_u64().unwrap(), "AcceptsAt {line}");
                let body = &input[1..];
                assert_eq!(u64::from(input_contracts::is_viable_prefix(body)), record["viable"].as_u64().unwrap(), "IsViablePrefix {line}");
                assert_eq!(u64::from(input_contracts::is_specific_body(body)), record["specific"].as_u64().unwrap(), "IsSpecificBody {line}");
                let matched = input_contracts::match_complete(body);
                assert_eq!(shape_number(matched.shape), record["shape"].as_u64().unwrap(), "MatchComplete.shape {line}");
                assert_eq!(matched.group_count as u64, record["groups"].as_u64().unwrap(), "MatchComplete.groups {line}");
                dates += 1;
                accepted += usize::from(record["accepts"].as_u64() == Some(1));
                shapes += usize::from(matched.shape != DateTimeShape::None);
            }
            "direct" => {
                assert_eq!(u64::from(input_contracts::direct_helpcode_accepts_slash_at(input, caret)), record["slash"].as_u64().unwrap(), "AcceptsSlashAt {line}");
                assert_eq!(u64::from(input_contracts::direct_helpcode_accepts_semicolon_final_at(input, caret)), record["semicolon"].as_u64().unwrap(), "AcceptsSemicolonFinalAt {line}");
                directs += 1;
                slashes += usize::from(record["slash"].as_u64() == Some(1));
                semicolons += usize::from(record["semicolon"].as_u64() == Some(1));
            }
            other => panic!("不认识的记录 {other}"),
        }
    }
    eprintln!("输入形状：日期 {dates} 条（接受 {accepted}，认出形状 {shapes}），直接辅助码 {directs} 条（斜杠 {slashes}，分号 {semicolons}）");
    assert!(dates == 2500 && accepted > 200 && shapes > 100 && directs == 2500 && slashes > 100 && semicolons > 500);
}
