//! 全拼词库（不带纠错）对着真实的 C++ `QuanpinDictionary` 核对。基线 `expected_quanpin_dictionary.jsonl` 由
//! `msime_quanpin_dictionary_probe` 导出：
//!
//! - 读：在随包 `msime.db` 上跑 1300 个输入（取自词库真键的整串、前缀、简拼、混拼、没打完的尾音节、ü 别名、手动分隔符，
//!   外加最少音节数打平的歧义串）的 `query` / `fuzzy_candidates` / `expand_initial_candidates` / `find_candidate`，
//!   随机整句开关（词格、显示下一条）、模糊音、显式切分和“用户在这个读音上调过频”的日志状态；
//! - 写库场景：合成小词库上顺序跑造词、调频、删词，夹着查询，每步的返回值和词库/日志逐表转储的哈希（不含 `updated_at`）。
//!
//! 要 `installer/app_data/` 下的 `msime.db`、`sc.lm`（或 `MSIME_APP_DATA` 指向的目录），缺了就跳过。

use msime_core::kenlm::KenLm;
use msime_core::user_dictionary::{self, DictionaryKind};
use msime_core::lattice::{Item, Source};
use msime_core::quanpin_dictionary::{QuanpinDictionary, SentenceAssociation};
use rusqlite::types::ValueRef;
use rusqlite::{Connection, OpenFlags};
use serde_json::Value;
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

fn golden_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden")
}

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

fn source_name(source: Source) -> &'static str {
    match source {
        Source::Database => "database",
        Source::UserDatabase => "user",
        Source::CloudSuggestion => "cloud",
        Source::AiSuggestion => "ai",
        Source::EnglishDictionary => "english",
        Source::QuickPhrase => "quick",
        Source::Emoji => "emoji",
        Source::Kaomoji => "kaomoji",
        Source::Generated => "generated",
        Source::Fallback => "fallback",
        Source::NeuralDesktop => "neural_desktop",
        Source::NeuralKeyboard => "neural_keyboard",
        Source::DateTime => "datetime",
        Source::Collocation => "collocation",
        _ => "other",
    }
}

fn item_json(item: &Item) -> String {
    // 输入域非空才多一项，和探针一致。
    let domains = if item.input_domains.is_empty() { String::new() } else { format!(",[{}]", item.input_domains.iter().map(|d| quote(d)).collect::<Vec<_>>().join(",")) };
    format!(
        "[{},{},{},{},\"{}\",{},{},{}{}]",
        quote(&item.pinyin),
        quote(&item.canonical_pinyin),
        quote(&item.word),
        item.weight,
        source_name(item.source),
        i32::from(item.sentence_association),
        i32::from(item.fuzzy),
        quote(&item.corrected_from),
        domains
    )
}

fn list_json(items: &[Item]) -> Value {
    let all: String = items.iter().map(|item| item_json(item) + "\n").collect();
    let top: Vec<String> = items.iter().take(8).map(item_json).collect();
    serde_json::from_str(&format!("{{\"n\":{},\"h\":\"{}\",\"top\":[{}]}}", items.len(), fnv(&all), top.join(","))).unwrap()
}

fn records() -> Vec<Value> {
    fs::read_to_string(golden_dir().join("expected_quanpin_dictionary.jsonl")).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn load_environment() -> Option<(PathBuf, KenLm)> {
    let data = app_data();
    let (db, lm) = (data.join("msime.db"), data.join("sc.lm"));
    if !db.exists() || !lm.exists() {
        eprintln!("跳过：{} 下缺 msime.db / sc.lm", data.display());
        return None;
    }
    let model = KenLm::open(&lm).unwrap_or_else(|e| panic!("载入失败：{e}"));
    Some((db, model))
}

#[test]
fn read_queries_match_cpp() {
    let Some((db, model)) = load_environment() else { return };
    let journal = std::env::temp_dir().join(format!("msime_quanpin_golden_read_{}.db", std::process::id()));
    let _ = fs::remove_file(&journal);
    let journal_path = journal.to_string_lossy().into_owned();
    let mut dictionary = QuanpinDictionary::open(&db, &journal_path, Some(&model));
    let mut counts: HashMap<String, usize> = HashMap::new();
    let (mut nonempty, mut generated, mut fuzzy_rows, mut marked, mut preferred, mut sentence_first, mut corrected_queries, mut tagged) = (0, 0, 0, 0, 0, 0, 0, 0);
    for record in records().iter().filter(|r| r["kind"] == "read") {
        let association = record["assoc"].as_u64().unwrap();
        dictionary.set_sentence_association(SentenceAssociation { word_lattice: association >= 1, show_next_on_duplicate: association == 2 });
        let fuzzy = record["fuzzy"].as_u64().unwrap() as u32;
        let autocorrect = record["ac"].as_u64().unwrap_or(0) as u32;
        let (raw, seg) = (record["raw"].as_str().unwrap(), record["seg"].as_str().unwrap());
        let prefer = record["prefer"].as_str().unwrap();
        dictionary.set_enabled_domain_dictionaries(record["dom"].as_str().unwrap_or(""));
        let op = record["op"].as_str().unwrap();
        // 日志状态在探针里是跨场景累积的：每个场景的第一条记录（query）之前写入。
        if op == "query" && !prefer.is_empty() {
            assert!(user_dictionary::record_upsert(&journal_path, DictionaryKind::Pinyin, prefer, "测", 1, ""));
            preferred += 1;
        }
        let label = format!("{op} raw={raw:?} seg={seg:?} assoc={association} fuzzy={fuzzy} ac={autocorrect} prefer={prefer:?}");
        let got = match op {
            "query" => {
                let items = dictionary.query(raw, seg, autocorrect, fuzzy);
                nonempty += usize::from(!items.is_empty());
                corrected_queries += usize::from(autocorrect != 0 && items.iter().any(|i| !i.corrected_from.is_empty()));
                generated += items.iter().filter(|i| i.source == Source::Generated).count();
                fuzzy_rows += items.iter().filter(|i| i.fuzzy).count();
                marked += items.iter().filter(|i| !i.corrected_from.is_empty()).count();
                tagged += items.iter().filter(|i| !i.input_domains.is_empty()).count();
                sentence_first += usize::from(items.first().is_some_and(|i| i.sentence_association));
                assert_eq!(dictionary.pinyin_sequence(), record["seqs"].as_str().unwrap(), "输入串 {label}");
                assert_eq!(dictionary.pinyin_segmentation(), record["segmentation"].as_str().unwrap(), "主切分 {label}");
                list_json(&items)
            }
            "fuzzy" => list_json(&dictionary.fuzzy_candidates(record["typed"].as_str().unwrap(), fuzzy)),
            "expand" => {
                let mut candidates = dictionary.query(raw, seg, autocorrect, fuzzy);
                let expanded = dictionary.expand_initial_candidates(raw, &mut candidates);
                assert_eq!(expanded, record["expanded"].as_bool().unwrap(), "{label}");
                list_json(&candidates)
            }
            "find" => dictionary
                .find_candidate(record["key"].as_str().unwrap(), record["value"].as_str().unwrap())
                .map_or(Value::Null, |item| serde_json::from_str(&item_json(&item)).unwrap()),
            other => panic!("没见过的操作 {other}"),
        };
        assert_eq!(got, record["ret"], "{label}");
        *counts.entry(op.to_string()).or_default() += 1;
    }
    let _ = fs::remove_file(&journal);
    eprintln!("全拼词库读：{counts:?}，查询非空 {nonempty}，词格整句行 {generated}，模糊行 {fuzzy_rows}，纠错标记行 {marked}，带纠错且出标记的查询 {corrected_queries}，调频日志 {preferred} 条，首位是整句 {sentence_first}，带输入域的行 {tagged}");
    assert!(counts.values().sum::<usize>() > 2000);
    assert!(generated > 100 && fuzzy_rows > 100 && marked > 20 && preferred > 100 && corrected_queries > 200 && tagged > 500);
}

// ---------------------------------------------------------------------------------------------
// 写库场景
// ---------------------------------------------------------------------------------------------

fn cell(value: ValueRef) -> String {
    match value {
        ValueRef::Null => "<null>".to_string(),
        ValueRef::Integer(v) => v.to_string(),
        ValueRef::Real(v) => v.to_string(),
        ValueRef::Text(t) => String::from_utf8_lossy(t).into_owned(),
        ValueRef::Blob(b) => String::from_utf8_lossy(b).into_owned(),
    }
}

fn dump_main(file: &Path) -> String {
    let Ok(db) = Connection::open_with_flags(file, OpenFlags::SQLITE_OPEN_READ_ONLY) else { return "(打不开)\n".to_string() };
    let tables: Vec<String> = db
        .prepare("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_%' ORDER BY name")
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .map(Result::unwrap)
        .collect();
    let mut text = String::new();
    for table in tables {
        let mut statement = db.prepare(&format!("SELECT key,jp,value,weight FROM \"{table}\" ORDER BY 1,3,4,2")).unwrap();
        let mut rows = statement.query([]).unwrap();
        let mut body = String::new();
        while let Some(row) = rows.next().unwrap() {
            let cells: Vec<String> = (0..4).map(|i| cell(row.get_ref(i).unwrap())).collect();
            body += &cells.join("|");
            body.push('\n');
        }
        if !body.is_empty() {
            text += &format!("T:{table}\n{body}");
        }
    }
    text
}

fn dump_journal(file: &Path) -> String {
    let Ok(db) = Connection::open_with_flags(file, OpenFlags::SQLITE_OPEN_READ_ONLY) else { return "(无日志)\n".to_string() };
    if !file.exists() {
        return "(无日志)\n".to_string();
    }
    let mut statement = db.prepare("SELECT dictionary,key,value,operation,weight,display,user_inserted FROM user_dictionary_operations ORDER BY 1,2,3").unwrap();
    let mut rows = statement.query([]).unwrap();
    let mut text = String::new();
    while let Some(row) = rows.next().unwrap() {
        let cells: Vec<String> = (0..7).map(|i| cell(row.get_ref(i).unwrap())).collect();
        text += &cells.join("|");
        text.push('\n');
    }
    text
}

#[test]
fn write_scenarios_match_cpp() {
    let Some((_, model)) = load_environment() else { return };
    let dir = std::env::temp_dir().join(format!("msime_quanpin_golden_{}", std::process::id()));
    let mut dictionary: Option<QuanpinDictionary> = None;
    let mut failures: Vec<String> = Vec::new();
    let (mut scenario, mut step, mut checked, mut skip_scenario) = (0i64, 0usize, 0usize, false);
    let mut changed = 0;
    for record in records().iter().filter(|r| r["kind"].is_null() || r["kind"] == "scenario") {
        if record["kind"] == "scenario" {
            // 先放掉上一个场景的连接，Windows 上打开着的文件删不掉。
            drop(dictionary.take());
            scenario = record["id"].as_i64().unwrap();
            step = 0;
            skip_scenario = false;
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(dir.join("user")).unwrap();
            fs::create_dir_all(dir.join("dictionaries")).unwrap();
            fs::copy(golden_dir().join("quanpin_dictionary_initial.db"), dir.join("dictionaries").join("msime.db")).unwrap();
            let journal = dir.join("user").join("msime_user.db");
            let mut opened = QuanpinDictionary::open(&dir.join("dictionaries").join("msime.db"), &journal.to_string_lossy(), Some(&model));
            opened.set_sentence_association(SentenceAssociation { word_lattice: record["assoc"].as_u64().unwrap() == 1, show_next_on_duplicate: false });
            dictionary = Some(opened);
            continue;
        }
        step += 1;
        if skip_scenario {
            continue;
        }
        let dictionary = dictionary.as_mut().unwrap();
        let (pinyin, word) = (record["pinyin"].as_str().unwrap_or(""), record["word"].as_str().unwrap_or(""));
        let got = match record["op"].as_str().unwrap() {
            "create_word" => serde_json::json!(dictionary.create_word(pinyin, word)),
            "create_word_from_canonical_pinyin" => serde_json::json!(dictionary.create_word_from_canonical_pinyin(pinyin, word)),
            "update_weight" => serde_json::json!(dictionary.update_weight_by_pinyin_and_word(pinyin, word)),
            "delete" => serde_json::json!(dictionary.delete_by_pinyin_and_word(pinyin, word)),
            "update_by_word" => {
                let listed = list_json(&dictionary.query(record["raw"].as_str().unwrap(), "", 0, 0));
                if listed != record["listed"] {
                    failures.push(format!("场景 {scenario} 第 {step} 步 {record}\n  实际列表 {listed}"));
                    skip_scenario = true;
                    continue;
                }
                serde_json::json!(dictionary.update_weight_by_word(word))
            }
            "query" => list_json(&dictionary.query(record["raw"].as_str().unwrap(), "", 0, 0)),
            "find" => dictionary
                .find_candidate(record["key"].as_str().unwrap(), record["value"].as_str().unwrap())
                .map_or(Value::Null, |item| serde_json::from_str(&item_json(&item)).unwrap()),
            other => panic!("没见过的操作 {other}"),
        };
        let state = format!("{}==\n{}", dump_main(&dir.join("dictionaries").join("msime.db")), dump_journal(&dir.join("user").join("msime_user.db")));
        let got_state = fnv(&state);
        checked += 1;
        changed += usize::from(got == serde_json::json!(0));
        if got != record["ret"] || got_state != record["state"].as_str().unwrap() {
            failures.push(format!("场景 {scenario} 第 {step} 步 {record}\n  实际返回 {got}\n  实际状态 {got_state}"));
            skip_scenario = true;
        }
    }
    drop(dictionary.take());
    let _ = fs::remove_dir_all(&dir);
    assert!(failures.is_empty(), "{} 个场景对不上，前几个：\n{}", failures.len(), failures.iter().take(3).cloned().collect::<Vec<_>>().join("\n"));
    assert!(checked > 600, "只比了 {checked} 步");
    eprintln!("全拼词库写：{checked} 步，其中返回成功 {changed}");
}
