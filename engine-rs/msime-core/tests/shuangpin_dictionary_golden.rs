//! 双拼词库对着真实的 C++ `ShuangpinDictionary` 核对。基线 `expected_shuangpin_dictionary.jsonl` 由
//! `msime_shuangpin_dictionary_probe` 导出，三类记录：
//!
//! - `sort`：`std::sort` 对相等权重的最终次序（`msvc_sort::sort_by` 要逐个下标相同）；
//! - `read`：在随包 `msime.db` 上跑 generate / generateSeries / 句中辅助码筛选 / 单码双码辅助码 / 单字母扩展 / 查权重 /
//!   造词查询，九套内置方案 × 70 个随机输入串，随机整句开关、模糊音和句中辅助码约束；
//! - 写库场景：合成小词库上顺序跑造词、调频、删词，每步的返回值和词库/日志逐表转储的哈希（不含 `updated_at`）。
//!
//! 读和写库场景要 `installer/app_data/` 下的 `msime.db`、`sc.lm`、`helpcodes/`（或 `MSIME_APP_DATA` 指向的目录），缺了就跳过。

use msime_core::helpcode;
use msime_core::kenlm::KenLm;
use msime_core::lattice::{Item, Source};
use msime_core::msvc_sort;
use msime_core::sentence_sources::SentenceSources;
use msime_core::shuangpin::profile;
use msime_core::shuangpin_dictionary::{SentenceAssociation, ShuangpinDictionary};
use msime_core::shuangpin_query::SyllableHelpcode;
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
        "[{},{},{},{},\"{}\",{}{}]",
        quote(&item.pinyin),
        quote(&item.canonical_pinyin),
        quote(&item.word),
        item.weight,
        source_name(item.source),
        i32::from(item.sentence_association),
        domains
    )
}

fn list_json(items: &[Item]) -> Value {
    let all: String = items.iter().map(|item| item_json(item) + "\n").collect();
    let top: Vec<String> = items.iter().take(8).map(item_json).collect();
    serde_json::from_str(&format!("{{\"n\":{},\"h\":\"{}\",\"top\":[{}]}}", items.len(), fnv(&all), top.join(","))).unwrap()
}

fn records() -> Vec<Value> {
    fs::read_to_string(golden_dir().join("expected_shuangpin_dictionary.jsonl")).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

#[test]
fn std_sort_orders_ties_like_msvc() {
    let mut checked = 0;
    let mut with_ties = 0;
    for record in records().iter().filter(|r| r["kind"] == "sort") {
        let weights: Vec<i64> = record["weights"].as_array().unwrap().iter().map(|w| w.as_i64().unwrap()).collect();
        let mut rows: Vec<(i64, usize)> = weights.iter().copied().enumerate().map(|(i, w)| (w, i)).collect();
        // 基线记的是排序前的输入（`repeat == 1` 时已经按权重降序排过），下标是输入里的位置。
        msvc_sort::sort_by(&mut rows, |a, b| a.0 > b.0);
        let order: Vec<usize> = rows.iter().map(|r| r.1).collect();
        let want: Vec<usize> = record["order"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap() as usize).collect();
        assert_eq!(order, want, "排序 #{} 大小 {}", record["id"], weights.len());
        checked += 1;
        with_ties += usize::from(weights.len() > 32 && weights.windows(2).any(|w| w[0] == w[1]) || weights.len() > 32);
    }
    assert!(checked >= 200);
    assert!(with_ties > 100);
}

#[test]
fn pinyin_cutting_matches_cpp() {
    use msime_core::quanpin_query::{cut_pinyin_by_mode, is_complete_pinyin_input, segments_to_jianpin, split_segments, CutMode};
    let paths_of = |value: &Value| -> Vec<Vec<String>> {
        value.as_array().unwrap().iter().map(|p| p.as_array().unwrap().iter().map(|s| s.as_str().unwrap().to_string()).collect()).collect()
    };
    let (mut checked, mut corrected, mut multi, mut incomplete) = (0, 0, 0, 0);
    for record in records().iter().filter(|r| r["kind"] == "cut") {
        let input = record["input"].as_str().unwrap();
        assert_eq!(cut_pinyin_by_mode(input, CutMode::Greedy), paths_of(&record["greedy"]), "贪心 {input:?}");
        let want = paths_of(&record["correction"]);
        assert_eq!(cut_pinyin_by_mode(input, CutMode::Correction), want, "纠错 {input:?}");
        assert_eq!(is_complete_pinyin_input(input), record["complete"].as_bool().unwrap(), "完整 {input:?}");
        assert_eq!(segments_to_jianpin(&split_segments(input)), record["jianpin"].as_str().unwrap(), "简拼 {input:?}");
        checked += 1;
        corrected += usize::from(want != paths_of(&record["greedy"]));
        multi += usize::from(want.len() > 1);
        incomplete += usize::from(!record["complete"].as_bool().unwrap());
    }
    assert!(checked >= 4500);
    eprintln!("切分 {checked} 条：纠错与贪心不同 {corrected}，多条路径 {multi}，不完整 {incomplete}");
    assert!(corrected > 200 && multi > 20 && incomplete > 200);
}

fn load_environment() -> Option<(PathBuf, KenLm, helpcode::Keymap)> {
    let data = app_data();
    let (db, lm, keymap) = (data.join("msime.db"), data.join("sc.lm"), data.join("helpcodes").join("xiaohe_helpcode.txt"));
    if !db.exists() || !lm.exists() || !keymap.exists() {
        eprintln!("跳过：{} 下缺 msime.db / sc.lm / helpcodes", data.display());
        return None;
    }
    let model = KenLm::open(&lm).unwrap_or_else(|e| panic!("载入失败：{e}"));
    Some((db, model, helpcode::parse_keymap(&fs::read_to_string(keymap).unwrap())))
}

fn constraints_of(value: &Value) -> Vec<SyllableHelpcode> {
    value
        .as_array()
        .unwrap()
        .iter()
        .map(|c| SyllableHelpcode { syllable: c[0].as_u64().unwrap() as usize, first: c[1].as_u64().unwrap() as u8, second: c[2].as_u64().unwrap() as u8 })
        .collect()
}

#[test]
fn read_queries_match_cpp() {
    let Some((db, model, keymap)) = load_environment() else { return };
    let journal = std::env::temp_dir().join("msime_shuangpin_golden_read_journal.db");
    let mut dictionaries: HashMap<String, ShuangpinDictionary> = HashMap::new();
    let mut counts: HashMap<String, usize> = HashMap::new();
    let mut nonempty = 0;
    for record in records().iter().filter(|r| r["kind"] == "read") {
        let name = record["profile"].as_str().unwrap().to_string();
        let dictionary = dictionaries.entry(name.clone()).or_insert_with(|| ShuangpinDictionary::open(profile(&name), &db, &journal.to_string_lossy(), keymap.clone(), Some(&model)));
        let association = record["assoc"].as_u64().unwrap();
        dictionary.set_sentence_association(SentenceAssociation { word_lattice: association >= 1, show_next_on_duplicate: association == 2, ..Default::default() });
        dictionary.set_fuzzy_pinyin(record["fuzzy"].as_u64().unwrap() as u32);
        dictionary.set_syllable_helpcodes(constraints_of(&record["constraints"]));
        dictionary.set_enabled_domain_dictionaries(record["dom"].as_str().unwrap_or(""));
        let (seq, seg) = (record["seq"].as_str().unwrap(), record["seg"].as_str().unwrap());
        let op = record["op"].as_str().unwrap();
        let got = match op {
            "generate" => list_json(&dictionary.generate(seq, seg)),
            "series" => list_json(&dictionary.generate_series(seq, seg)),
            "filter" => {
                let mut series = dictionary.generate_series(seq, seg);
                dictionary.filter_by_syllable_helpcodes(&mut series);
                list_json(&series)
            }
            "helpcodes" => list_json(&dictionary.generate_with_helpcodes(seq, seg, record["typed"].as_str().unwrap(), record["codes"].as_str().unwrap())),
            "expand" => {
                let mut candidates = dictionary.generate_series(seq, seg);
                let expanded = dictionary.expand_initial_candidates(seq, &mut candidates);
                assert_eq!(expanded, record["expanded"].as_bool().unwrap(), "{record}");
                list_json(&candidates)
            }
            "find" => dictionary
                .find_candidate(record["key"].as_str().unwrap(), record["value"].as_str().unwrap())
                .map_or(Value::Null, |item| serde_json::from_str(&item_json(&item)).unwrap()),
            "creating" => list_json(&dictionary.generate_for_creating_word(seq)),
            other => panic!("没见过的操作 {other}"),
        };
        assert_eq!(got, record["ret"], "{name} {op} {seq:?} assoc={association} fuzzy={} constraints={}", record["fuzzy"], record["constraints"]);
        *counts.entry(op.to_string()).or_default() += 1;
        nonempty += usize::from(got.get("n").and_then(Value::as_u64).unwrap_or(0) > 0);
    }
    eprintln!("双拼词库读：{counts:?}，非空 {nonempty}");
    assert!(counts.values().sum::<usize>() > 2000);
}

// ---------------------------------------------------------------------------------------------
// 整句来源
// ---------------------------------------------------------------------------------------------

/// 后台重排线程算完。
fn wait_for_worker(sources: &SentenceSources) {
    for _ in 0..2000 {
        if sources.worker.is_idle() {
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    panic!("神经重排线程十秒内没停");
}

#[test]
fn sentence_sources_match_cpp() {
    let Some((db, model, keymap)) = load_environment() else { return };
    let data = app_data();
    let (dict, network) = (data.join("dict_pinyin.dat"), data.join("sentence-model.safetensors"));
    if !dict.exists() || !network.exists() {
        eprintln!("跳过：{} 下缺 dict_pinyin.dat / sentence-model.safetensors", data.display());
        return;
    }
    let journal = std::env::temp_dir().join("msime_shuangpin_golden_sources_journal.db");
    let sources = SentenceSources::new(&data);
    let gram = golden_dir().join("synthetic.gram").to_string_lossy().into_owned();
    let mut dictionaries: HashMap<String, ShuangpinDictionary> = HashMap::new();
    let (mut cases, mut google_rows, mut reranked, mut waited, mut constrained) = (0, 0, 0, 0, 0);
    for record in records().iter().filter(|r| r["kind"] == "sources") {
        let name = record["profile"].as_str().unwrap().to_string();
        let dictionary = dictionaries
            .entry(name.clone())
            .or_insert_with(|| ShuangpinDictionary::open(profile(&name), &db, &journal.to_string_lossy(), keymap.clone(), Some(&model)).with_sentence_sources(sources.clone()));
        let flag = |key: &str| record[key].as_u64().unwrap() == 1;
        dictionary.set_sentence_association(SentenceAssociation {
            word_lattice: flag("lat"),
            google: flag("google"),
            neural_keyboard: flag("nk"),
            neural_desktop: flag("nd"),
            show_next_on_duplicate: flag("next"),
            neural_desktop_model: if record["ndm"] == "same" { network.to_string_lossy().into_owned() } else { String::new() },
            collocation_model: if flag("col") { gram.clone() } else { String::new() },
            collocation_weight: record["colw"].as_f64().unwrap(),
            collocation_rerank: flag("colr"),
            ..Default::default()
        });
        dictionary.set_rescoring_context(record["context"].as_str().unwrap());
        dictionary.set_fuzzy_pinyin(record["fuzzy"].as_u64().unwrap() as u32);
        let helpcodes = constraints_of(&record["constraints"]);
        constrained += usize::from(!helpcodes.is_empty());
        dictionary.set_syllable_helpcodes(helpcodes);
        sources.worker.clear();
        let (seq, seg) = (record["seq"].as_str().unwrap(), record["seg"].as_str().unwrap());
        let label = format!("{name} seq={seq:?} seg={seg:?} fuzzy={} constraints={} context={} {record}", record["fuzzy"], record["constraints"], record["context"]);
        let first = dictionary.generate_series(seq, seg);
        assert_eq!(list_json(&first), record["first"], "第一次 {label}");
        google_rows += first.iter().filter(|i| i.source == Source::Fallback && i.sentence_association).count();
        // 一次查询可能排了不止一批：算完就重查，直到结果不再变。
        let second = loop {
            wait_for_worker(&sources);
            let again = dictionary.generate_series(seq, seg);
            wait_for_worker(&sources);
            let settled = dictionary.generate_series(seq, seg);
            if list_json(&settled) == list_json(&again) {
                break settled;
            }
        };
        if record["second"].is_null() {
            assert_eq!(list_json(&second), record["first"], "没有后台任务时重查应当不变 {label}");
        } else {
            assert_eq!(list_json(&second), record["second"], "重排后 {label}");
            waited += 1;
            reranked += usize::from(list_json(&second) != list_json(&first));
        }
        cases += 1;
    }
    let _ = fs::remove_file(&journal);
    eprintln!("双拼整句来源：{cases} 个，Google 整句行 {google_rows}，带句中辅助码 {constrained}，神经重排重查 {waited}（改变次序 {reranked}）");
    assert!(cases >= 300 && google_rows > 50 && waited > 30 && constrained > 30);
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
    let Some((_, model, keymap)) = load_environment() else { return };
    let dir = std::env::temp_dir().join(format!("msime_shuangpin_golden_{}", std::process::id()));
    let mut dictionary: Option<ShuangpinDictionary> = None;
    let mut failures: Vec<String> = Vec::new();
    let (mut scenario, mut step, mut checked, mut skip_scenario) = (0i64, 0usize, 0usize, false);
    let mut changed = 0;
    // 每个场景在 Option 里重新建一个字典；借用 model 的生命周期随函数。
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
            fs::copy(golden_dir().join("shuangpin_dictionary_initial.db"), dir.join("dictionaries").join("msime.db")).unwrap();
            let journal = dir.join("user").join("msime_user.db");
            let mut opened = ShuangpinDictionary::open(
                profile(record["profile"].as_str().unwrap()),
                &dir.join("dictionaries").join("msime.db"),
                &journal.to_string_lossy(),
                keymap.clone(),
                Some(&model),
            );
            opened.set_sentence_association(SentenceAssociation { word_lattice: true, ..Default::default() });
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
            "create_word_from_quanpin" => serde_json::json!(dictionary.create_word_from_quanpin(pinyin, word)),
            "update_weight" => serde_json::json!(dictionary.update_weight_by_pinyin_and_word(pinyin, word)),
            "delete" => serde_json::json!(dictionary.delete_by_pinyin_and_word(pinyin, word)),
            "series" => list_json(&dictionary.generate_series(record["seq"].as_str().unwrap(), record["seg"].as_str().unwrap())),
            "creating" => list_json(&dictionary.generate_for_creating_word(record["seq"].as_str().unwrap())),
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
    let _ = fs::remove_dir_all(&dir);
    assert!(failures.is_empty(), "{} 个场景对不上，前几个：\n{}", failures.len(), failures.iter().take(3).cloned().collect::<Vec<_>>().join("\n"));
    assert!(checked > 600, "只比了 {checked} 步");
    eprintln!("双拼词库写：{checked} 步，其中返回成功 {changed}");
}
