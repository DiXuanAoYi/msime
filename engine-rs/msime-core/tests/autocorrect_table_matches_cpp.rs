//! Rust 现算的纠错表必须和 C++ 的生成头文件 `engine/quanpin/autocorrect_table.h` 逐行一致。

use msime_core::autocorrect::{tables, Table};
use std::collections::BTreeSet;
use std::path::PathBuf;

fn header() -> String {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/quanpin/autocorrect_table.h");
    std::fs::read_to_string(path).expect("读不到 engine/quanpin/autocorrect_table.h")
}

/// 取 `名字[] = {` 到下一个 `};` 之间的内容。
fn section<'a>(text: &'a str, name: &str) -> &'a str {
    let start = text.find(&format!("{name}[] = {{")).unwrap_or_else(|| panic!("找不到 {name}")) + name.len() + 6;
    &text[start..start + text[start..].find("};").unwrap()]
}

fn quoted(text: &str) -> Vec<String> {
    text.split('"').enumerate().filter_map(|(i, piece)| (i % 2 == 1).then(|| piece.to_string())).collect()
}

/// 解析 `{"wrong", index},` 行，用音节表把下标换回音节。
fn entries(text: &str, syllables: &[String]) -> Table {
    let mut table = Table::new();
    for chunk in text.split('{').skip(1) {
        let (wrong, rest) = chunk.split_once("\", ").expect("格式不对");
        let wrong = wrong.trim_start_matches('"').to_string();
        let index: usize = rest.split('}').next().unwrap().trim().parse().unwrap();
        table.entry(wrong).or_default().insert(syllables[index].clone());
    }
    table
}

fn describe(label: &str, ours: &Table, theirs: &Table) {
    let only_ours: BTreeSet<_> = ours.iter().flat_map(|(k, v)| v.iter().map(move |c| (k, c))).collect();
    let only_theirs: BTreeSet<_> = theirs.iter().flat_map(|(k, v)| v.iter().map(move |c| (k, c))).collect();
    let extra: Vec<_> = only_ours.difference(&only_theirs).take(5).collect();
    let missing: Vec<_> = only_theirs.difference(&only_ours).take(5).collect();
    assert!(extra.is_empty() && missing.is_empty(), "{label}：Rust 多出 {extra:?}，缺少 {missing:?}");
}

#[test]
fn rust_tables_equal_the_generated_cpp_header() {
    let text = header();
    let syllables = quoted(section(&text, "kCorrectSyllables"));
    assert_eq!(syllables.len(), msime_core::syllables::INTACT.len());
    let ours = tables();
    for (label, name, table) in [
        ("换位", "kTranspositionEntries", &ours.transposition),
        ("邻键", "kNeighborEntries", &ours.neighbor),
        ("漏键", "kDeletionEntries", &ours.deletion),
        ("多键", "kInsertionEntries", &ours.insertion),
    ] {
        let theirs = entries(section(&text, name), &syllables);
        assert!(theirs.len() > 100, "{label}：C++ 表只读到 {} 个键", theirs.len());
        describe(label, table, &theirs);
        assert_eq!(table.len(), theirs.len(), "{label}：键的个数不同");
        eprintln!("{label}：{} 个错拼键，与 C++ 一致", table.len());
    }
}
