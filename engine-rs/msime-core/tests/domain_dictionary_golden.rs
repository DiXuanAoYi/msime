//! 领域词库对着真实的 C++ `domain_dictionary::Dictionary` 核对。基线 `expected_domain_dictionary.jsonl` 由
//! `msime_domain_dictionary_probe` 在随包 `domain-input.db` 上导出：
//!
//! - `ids` / `normalize`：公开词包 ID 和启用串归一（含不认识的名字、重复、空串、乱序、带空格）；
//! - `enable`：200 次切换启用集合，每次后面跟 14 步 `query`（全拼/双拼）、`exact`、`lattice`（随机模糊音、跨度上限）和
//!   `merge`，输入取自领域库真键的整串、前缀、简拼、混拼、没打完的尾音节，比条数、内容哈希、前 8 项（含输入域）。
//!
//! 要 `installer/app_data/domain-input.db`（或 `MSIME_APP_DATA` 指向的目录），缺了就跳过。

use msime_core::dict::QuerySource;
use msime_core::domain_dictionary::{ids, merge, normalize_enabled, DomainDictionary};
use msime_core::lattice::{Item, Lexeme, Source};
use serde_json::Value;
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

fn app_data() -> PathBuf {
    std::env::var_os("MSIME_APP_DATA").map(PathBuf::from).unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../installer/app_data"))
}

fn fnv(text: &str) -> String {
    let mut hash: u64 = 0xcbf29ce484222325;
    for b in text.bytes() {
        hash ^= u64::from(b);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("{hash:016x}")
}

fn quote(text: &str) -> String {
    let mut out = String::from("\"");
    for c in text.chars() {
        match c {
            '"' | '\\' => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 => out += &format!("\\u{:04x}", c as u32),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn strings_json(items: &[String]) -> String {
    format!("[{}]", items.iter().map(|s| quote(s)).collect::<Vec<_>>().join(","))
}

fn item_json(item: &Item) -> String {
    assert_eq!(item.source, Source::Database);
    format!("[{},{},{},{},0,{}]", quote(&item.pinyin), quote(&item.canonical_pinyin), quote(&item.word), item.weight, strings_json(&item.input_domains))
}

fn array_json(items: &[Item]) -> String {
    format!("[{}]", items.iter().map(item_json).collect::<Vec<_>>().join(","))
}

fn summary(rows: &[String]) -> Value {
    let all: String = rows.iter().map(|row| format!("{row}\n")).collect();
    let top: Vec<&str> = rows.iter().take(8).map(String::as_str).collect();
    serde_json::from_str(&format!("{{\"n\":{},\"h\":\"{}\",\"top\":[{}]}}", rows.len(), fnv(&all), top.join(","))).unwrap()
}

fn list_json(items: &[Item]) -> Value {
    summary(&items.iter().map(item_json).collect::<Vec<_>>())
}

fn lexemes_json(rows: &[Lexeme]) -> Value {
    summary(&rows.iter().map(|r| format!("[{},{},{},{:.6},{}]", quote(&r.key), quote(&r.value), r.weight, r.penalty, strings_json(&r.input_domains))).collect::<Vec<_>>())
}

fn strings(value: &Value) -> Vec<String> {
    value.as_array().unwrap().iter().map(|s| s.as_str().unwrap().to_string()).collect()
}

fn parse_items(value: &Value) -> Vec<Item> {
    value
        .as_array()
        .unwrap()
        .iter()
        .map(|row| Item {
            pinyin: row[0].as_str().unwrap().into(),
            canonical_pinyin: row[1].as_str().unwrap().into(),
            word: row[2].as_str().unwrap().into(),
            weight: row[3].as_i64().unwrap(),
            source: Source::Database,
            sentence_association: false,
            input_domains: strings(&row[5]),
            fuzzy: false,
            corrected_from: String::new(),
            scheme: Default::default(),
            fixed_position: 0,
        })
        .collect()
}

fn records() -> Vec<Value> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_domain_dictionary.jsonl");
    fs::read_to_string(path).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

#[test]
fn ids_and_normalization_match_cpp() {
    let mut normalized = 0;
    for record in records() {
        match record["kind"].as_str().unwrap() {
            "ids" => assert_eq!(strings(&record["ret"]), ids()),
            "normalize" => {
                let csv = record["csv"].as_str().unwrap();
                assert_eq!(normalize_enabled(csv), record["ret"].as_str().unwrap(), "normalize {csv:?}");
                normalized += 1;
            }
            _ => {}
        }
    }
    assert!(normalized >= 300);
}

#[test]
fn queries_match_cpp() {
    let path = app_data().join("domain-input.db");
    if !path.exists() {
        eprintln!("跳过：{} 不存在", path.display());
        return;
    }
    let mut dictionary = DomainDictionary::new(&path);
    let mut counts: HashMap<&str, (usize, usize)> = HashMap::new();
    let mut tagged = 0;
    let mut note = |kind: &'static str, nonempty: bool| {
        let entry = counts.entry(kind).or_default();
        entry.0 += 1;
        entry.1 += usize::from(nonempty);
    };
    for record in records() {
        match record["kind"].as_str().unwrap() {
            "enable" => {
                let csv = record["csv"].as_str().unwrap();
                assert_eq!(dictionary.set_enabled(csv), record["changed"].as_bool().unwrap(), "enable {csv:?}");
                assert_eq!(dictionary.enabled(), record["enabled"].as_str().unwrap(), "enable {csv:?}");
            }
            "query" => {
                let (segments, code) = (strings(&record["segments"]), record["code"].as_str().unwrap());
                let source = if record["source"] == 1 { QuerySource::Shuangpin } else { QuerySource::Quanpin };
                let rows = dictionary.query(&segments, code, record["limit"].as_u64().unwrap() as usize, source);
                assert_eq!(list_json(&rows), record["ret"], "query {segments:?} code={code:?} enabled={}", dictionary.enabled());
                tagged += rows.iter().filter(|r| !r.input_domains.is_empty()).count();
                note("query", !rows.is_empty());
            }
            "exact" => {
                let cuts: Vec<Vec<String>> = record["cuts"].as_array().unwrap().iter().map(strings).collect();
                let rows = dictionary.exact(&cuts, record["limit"].as_u64().unwrap() as usize);
                assert_eq!(list_json(&rows), record["ret"], "exact {cuts:?} enabled={}", dictionary.enabled());
                note("exact", !rows.is_empty());
            }
            "lattice" => {
                let span = strings(&record["span"]);
                let rows = dictionary.lattice(&span, record["limit"].as_i64().unwrap() as i32, record["rules"].as_u64().unwrap() as u32);
                assert_eq!(lexemes_json(&rows), record["ret"], "lattice {span:?} enabled={}", dictionary.enabled());
                note("lattice", !rows.is_empty());
            }
            "merge" => {
                let mut base = parse_items(&record["base"]);
                merge(&mut base, parse_items(&record["extra"]));
                assert_eq!(serde_json::from_str::<Value>(&array_json(&base)).unwrap(), record["ret"], "merge");
                note("merge", !base.is_empty());
            }
            _ => {}
        }
    }
    eprintln!("领域词库：{counts:?}，带输入域的行 {tagged}");
    assert!(counts["query"].1 > 150 && counts["exact"].1 > 80 && counts["lattice"].1 > 60 && counts["merge"].1 > 100 && tagged > 500);
}
