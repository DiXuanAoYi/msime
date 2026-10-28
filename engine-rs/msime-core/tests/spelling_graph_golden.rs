//! 直接辅助码的拼写图对着真实的 C++ `direct_helpcode::build_spelling_graph` 核对。基线 `expected_spelling_graph.jsonl`
//! 由 `msime_spelling_graph_probe` 导出：九套内置方案 × 330 条随机输入串，随机的斜杠/大写标记开关，以及五种辅码谓词
//! （空、哈希模 2/3/5 不为零、恒真）。辅码表用一个确定的哈希函数代替，两边写同一个。

use msime_core::shuangpin::profile;
use msime_core::spelling_graph::{build_spelling_graph, next_spelling_position, single_path, SpellingKind, SpellingOptions, SyllableSpelling};
use serde_json::Value;
use std::path::PathBuf;

fn aux_predicate(mode: u64, quanpin: &str, first: u8, second: u8) -> bool {
    if mode == 4 {
        return true;
    }
    let mut hash: u64 = 0xcbf29ce484222325;
    for b in quanpin.bytes().chain([first, second]) {
        hash ^= u64::from(b);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash % [0, 2, 3, 5][mode as usize] != 0
}

fn kind(name: &str) -> SpellingKind {
    match name {
        "plain" => SpellingKind::Plain,
        "aux1" => SpellingKind::Aux1,
        "aux2_slash" => SpellingKind::Aux2Slash,
        "aux2_upper" => SpellingKind::Aux2Upper,
        "aux2_abbrev" => SpellingKind::Aux2Abbrev,
        "initial" => SpellingKind::Initial,
        other => panic!("未知的拼写种类 {other}"),
    }
}

fn spelling(value: &Value) -> SyllableSpelling {
    SyllableSpelling {
        begin: value[0].as_u64().unwrap() as usize,
        end: value[1].as_u64().unwrap() as usize,
        code: value[2].as_str().unwrap().to_string(),
        quanpin: value[3].as_str().unwrap().to_string(),
        first: value[4].as_u64().unwrap() as u8,
        second: value[5].as_u64().unwrap() as u8,
        kind: kind(value[6].as_str().unwrap()),
    }
}

#[test]
fn spelling_graphs_equal_cpp() {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_spelling_graph.jsonl");
    let text = std::fs::read_to_string(path).unwrap();
    let (mut checked, mut empty, mut single, mut aux_edges, mut strict, mut lenient_used) = (0, 0, 0, 0, 0, 0);
    for row in text.lines().filter(|l| !l.trim().is_empty()).map(|l| serde_json::from_str::<Value>(l).unwrap()) {
        let typed = row["typed"].as_str().unwrap();
        let mode = row["mode"].as_u64().unwrap();
        let options = SpellingOptions { slash_marker: row["slash"].as_bool().unwrap(), uppercase_marker: row["upper"].as_bool().unwrap() };
        let predicate = |q: &str, f: u8, s: u8| aux_predicate(mode, q, f, s);
        let graph = build_spelling_graph(typed, profile(row["profile"].as_str().unwrap()), if mode == 0 { None } else { Some(&predicate) }, &options);
        let ctx = format!("{} {typed:?} mode={mode} slash={} upper={}", row["profile"], options.slash_marker, options.uppercase_marker);

        assert_eq!(graph.size as u64, row["size"].as_u64().unwrap(), "大小 {ctx}");
        assert_eq!(graph.is_empty(), row["empty"].as_bool().unwrap(), "空图 {ctx}");
        let want_edges = row["edges"].as_array().unwrap();
        assert_eq!(graph.edges.len(), want_edges.len(), "位置数 {ctx}");
        for (pos, (got, want)) in graph.edges.iter().zip(want_edges).enumerate() {
            let want: Vec<SyllableSpelling> = want.as_array().unwrap().iter().map(spelling).collect();
            assert_eq!(got, &want, "位置 {pos} 的边 {ctx}");
            aux_edges += got.iter().filter(|s| s.has_aux()).count();
        }
        let want_skip: Vec<bool> = row["skip"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap() == 1).collect();
        assert_eq!(graph.skip, want_skip, "分隔符 {ctx}");
        if !graph.is_empty() {
            let want_next: Vec<usize> = row["next"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap() as usize).collect();
            let next: Vec<usize> = (0..=graph.size).map(|pos| next_spelling_position(&graph, pos)).collect();
            assert_eq!(next, want_next, "下一个出字位置 {ctx}");
        }
        let path = single_path(&graph);
        assert_eq!(path.is_some(), row["single"].as_bool().unwrap(), "唯一路径 {ctx}");
        let want_path: Vec<SyllableSpelling> = row["path"].as_array().unwrap().iter().map(spelling).collect();
        assert_eq!(path.unwrap_or_default(), want_path, "路径 {ctx}");

        checked += 1;
        empty += i32::from(graph.is_empty());
        single += i32::from(row["single"].as_bool().unwrap());
        strict += i32::from(options.uppercase_marker && typed.bytes().any(|b| b.is_ascii_uppercase()));
        lenient_used += i32::from(typed.contains('/') || typed.contains(';'));
    }
    assert!(checked >= 2900 && empty > 50 && single > 1000 && aux_edges > 2000 && strict > 300 && lenient_used > 500, "{checked}/{empty}/{single}/{aux_edges}/{strict}/{lenient_used}");
    eprintln!("拼写图：{checked} 条输入串（空图 {empty}，唯一路径 {single}，辅码边 {aux_edges}，大写标记且含大写 {strict}，含斜杠/分号 {lenient_used}）的边、分隔符、下一位置、唯一路径与 C++ 一致");
}
