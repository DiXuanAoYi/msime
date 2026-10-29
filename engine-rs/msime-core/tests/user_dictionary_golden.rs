//! 用户词库（日志、调频、固定位置、快捷短语/英文槽位、日期时间顺序、重放、个人词库编辑与分页、词库状态导出）
//! 对着真实的 C++ `user_dictionary::*` / `metasequoia::*` 核对。
//!
//! 基线 `expected_user_dictionary.jsonl` 由 `msime_user_dictionary_probe` 导出：240 个场景（后 80 个专测出厂词库换过时的回放），每个场景从同一份合成词库
//! （`user_dictionary_initial_main.db`、`user_dictionary_initial_english.db`）和空日志出发，顺序执行 8 到 25 步随机操作，
//! 每步记下参数、返回值，以及执行后三个库（日志、msime.db、english.db）逐表转储的哈希。
//! 转储不含 `updated_at`（来自 `unixepoch()`），其余全部参与。

use msime_core::english;
use msime_core::lattice::Source;
use msime_core::user_dictionary::*;
use rusqlite::types::ValueRef;
use rusqlite::{Connection, OpenFlags};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};

fn fnv(text: &str) -> u64 {
    let mut hash: u64 = 0xcbf29ce484222325;
    for b in text.bytes() {
        hash ^= u64::from(b);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

fn golden_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden")
}

fn cell(value: ValueRef) -> String {
    match value {
        ValueRef::Null => "<null>".to_string(),
        ValueRef::Integer(v) => v.to_string(),
        ValueRef::Real(v) => v.to_string(),
        ValueRef::Text(t) => String::from_utf8_lossy(t).into_owned(),
        ValueRef::Blob(b) => String::from_utf8_lossy(b).into_owned(),
    }
}

fn dump_table(db: &Connection, table: &str, order: &str) -> String {
    let mut text = format!("T:{table}\n");
    let Ok(mut statement) = db.prepare(&format!("SELECT * FROM \"{table}\" ORDER BY {order}")) else {
        return text + "(无)\n";
    };
    let columns = statement.column_count();
    let mut rows = statement.query([]).unwrap();
    while let Some(row) = rows.next().unwrap() {
        for i in 0..columns {
            if i > 0 {
                text.push('|');
            }
            text += &cell(row.get_ref(i).unwrap());
        }
        text.push('\n');
    }
    text
}

fn dump_database(file: &Path, tables: &[(&str, &str)]) -> String {
    let Ok(db) = Connection::open_with_flags(file, OpenFlags::SQLITE_OPEN_READ_ONLY) else { return "(打不开)\n".to_string() };
    tables.iter().map(|(table, order)| dump_table(&db, table, order)).collect()
}

fn dump_state(dir: &Path) -> String {
    let mut text = String::from("== journal\n");
    let journal = dir.join("user").join("msime_user.db");
    match Connection::open_with_flags(&journal, OpenFlags::SQLITE_OPEN_READ_ONLY) {
        Ok(db) if journal.exists() => {
            text += "T:user_dictionary_operations\n";
            let mut statement = db
                .prepare("SELECT dictionary,key,value,operation,weight,display,user_inserted,weight_edited FROM user_dictionary_operations ORDER BY 1,2,3")
                .unwrap();
            let mut rows = statement.query([]).unwrap();
            while let Some(row) = rows.next().unwrap() {
                let cells: Vec<String> = (0..8).map(|i| cell(row.get_ref(i).unwrap())).collect();
                text += &cells.join("|");
                text.push('\n');
            }
            drop(rows);
            drop(statement);
            drop(db);
            text += &dump_database(
                &journal,
                &[
                    ("dictionary_state", "1"),
                    ("personal_dictionary_receipts", "1"),
                    ("candidate_selection_state", "1,2,3"),
                    ("fixed_candidate_positions", "1,2,3"),
                    ("quick_phrase_slots", "1"),
                    ("english_slots", "1"),
                    ("date_time_format_order", "1,3"),
                ],
            );
        }
        _ => text += "(无日志)\n",
    }
    text += "== main\n";
    text += &dump_database(
        &dir.join("dictionaries").join("msime.db"),
        &[
            ("tbl_1_n", "1,3"),
            ("tbl_1_w", "1,3"),
            ("tbl_1_j", "1,3"),
            ("tbl_1_t", "1,3"),
            ("tbl_1_d", "1,3"),
            ("tbl_1_k", "1,3"),
            ("tbl_1_l", "1,3"),
            ("tbl_1_c", "1,3"),
            ("tbl_1_m", "1,3"),
            ("tbl_1_f", "1,3"),
            ("tbl_2_n", "1,3"),
            ("tbl_2_j", "1,3"),
            ("tbl_3_n", "1,3"),
            ("wubi86", "1,2"),
            ("quick_parases", "1,2"),
        ],
    );
    text += "== english\n";
    text += &dump_database(&dir.join("dictionaries").join("english.db"), &[("english_words", "1,2")]);
    text
}

fn kind_of(label: &str) -> DictionaryKind {
    match label {
        "wubi" => DictionaryKind::Wubi,
        "quick" => DictionaryKind::QuickPhrase,
        "english" => DictionaryKind::English,
        _ => DictionaryKind::Pinyin,
    }
}

fn personal_kind(label: &str) -> PersonalDictionaryKind {
    match label {
        "wubi" => PersonalDictionaryKind::Wubi,
        "quick" => PersonalDictionaryKind::QuickPhrase,
        "english" => PersonalDictionaryKind::English,
        _ => PersonalDictionaryKind::Pinyin,
    }
}

fn personal_label(kind: PersonalDictionaryKind) -> &'static str {
    match kind {
        PersonalDictionaryKind::Pinyin => "pinyin",
        PersonalDictionaryKind::Wubi => "wubi",
        PersonalDictionaryKind::QuickPhrase => "quick",
        PersonalDictionaryKind::English => "english",
    }
}

fn source_name(source: Source) -> &'static str {
    match source {
        Source::Database => "database",
        Source::UserDatabase => "user",
        Source::CloudSuggestion => "cloud",
        Source::AiSuggestion => "ai",
        Source::EnglishDictionary => "english",
        _ => "other",
    }
}

fn source_of(name: &str) -> Source {
    match name {
        "database" => Source::Database,
        "user" => Source::UserDatabase,
        "cloud" => Source::CloudSuggestion,
        "ai" => Source::AiSuggestion,
        "english" => Source::EnglishDictionary,
        _ => Source::Fallback,
    }
}

fn candidates_json(items: &[WordItem]) -> Value {
    Value::Array(
        items
            .iter()
            .map(|item| {
                json!({"pinyin": item.pinyin, "canonical": item.canonical_pinyin, "word": item.word, "weight": item.weight,
                       "source": source_name(item.source), "fixed": item.fixed_position})
            })
            .collect(),
    )
}

fn candidates_of(value: &Value) -> Vec<WordItem> {
    value
        .as_array()
        .unwrap()
        .iter()
        .map(|item| WordItem {
            pinyin: item["pinyin"].as_str().unwrap().to_string(),
            canonical_pinyin: item["canonical"].as_str().unwrap().to_string(),
            word: item["word"].as_str().unwrap().to_string(),
            weight: item["weight"].as_i64().unwrap(),
            source: source_of(item["source"].as_str().unwrap()),
            fixed_position: item["fixed"].as_i64().unwrap() as i32,
        })
        .collect()
}

fn text<'a>(op: &'a Value, key: &str) -> &'a str {
    op[key].as_str().unwrap_or_else(|| panic!("缺字段 {key}：{op}"))
}
fn int(op: &Value, key: &str) -> i64 {
    op[key].as_i64().unwrap_or_else(|| panic!("缺字段 {key}：{op}"))
}
fn flag(op: &Value, key: &str) -> bool {
    match &op[key] {
        Value::Bool(b) => *b,
        other => other.as_i64().unwrap_or_else(|| panic!("缺字段 {key}：{op}")) != 0,
    }
}
fn strings(op: &Value, key: &str) -> Vec<String> {
    op[key].as_array().unwrap().iter().map(|v| v.as_str().unwrap().to_string()).collect()
}

fn personal_entry(value: &Value) -> Option<PersonalDictionaryEntry> {
    if value.is_null() {
        return None;
    }
    Some(PersonalDictionaryEntry {
        kind: personal_kind(text(value, "kind")),
        key: text(value, "key").to_string(),
        value: text(value, "value").to_string(),
        weight: int(value, "weight"),
    })
}

fn personal_entry_json(entry: &PersonalDictionaryEntry) -> Value {
    json!({"kind": personal_label(entry.kind), "key": entry.key, "value": entry.value, "weight": entry.weight})
}

fn run_op(op: &Value, dir: &Path) -> Value {
    let journal = dir.join("user").join("msime_user.db").to_string_lossy().into_owned();
    let main_db = dir.join("dictionaries").join("msime.db").to_string_lossy().into_owned();
    let english_db = dir.join("dictionaries").join("english.db").to_string_lossy().into_owned();
    let paths = RuntimePaths { user_data: dir.join("user"), dictionaries: dir.join("dictionaries") };
    match text(op, "op") {
        "record_upsert" => json!(record_upsert(&journal, kind_of(text(op, "kind")), text(op, "key"), text(op, "value"), int(op, "weight"), text(op, "display"))),
        "record_user_insert" => json!(record_user_insert(&journal, kind_of(text(op, "kind")), text(op, "key"), text(op, "value"), int(op, "weight"), text(op, "display"))),
        "record_weight_edit" => json!(record_weight_edit(&journal, kind_of(text(op, "kind")), text(op, "key"), text(op, "value"), int(op, "weight"), text(op, "display"))),
        "ensure_user_database" => json!(ensure_user_database(&journal)),
        "set_manifest" => {
            let manifest = dir.join("dictionaries").join("dictionary-manifest.json");
            let bodies = [
                r#"{"files":{"msime.db":{"sha256":"aaaa"}}}"#,
                r#"{"files":{"msime.db":{"sha256":"bbbb"}}}"#,
                "",
                "not json",
                r#"{"files":{"msime.db":{"sha256":""}}}"#,
                r#"{"files":{}}"#,
            ];
            let mode = int(op, "mode") as usize;
            if mode == 2 {
                let _ = fs::remove_file(&manifest);
            } else {
                fs::write(&manifest, bodies[mode]).unwrap();
            }
            json!(true)
        }
        "record_delete" => json!(record_delete(&journal, kind_of(text(op, "kind")), text(op, "key"), text(op, "value"))),
        "set_fixed_position" => json!(set_fixed_position(&journal, text(op, "context"), text(op, "key"), text(op, "value"), int(op, "position") as i32)),
        "clear_fixed_position" => json!(clear_fixed_position(&journal, text(op, "context"), text(op, "key"), text(op, "value"))),
        "is_fixed" => json!(is_fixed(&journal, text(op, "context"), text(op, "key"), text(op, "value"))),
        "apply_fixed_positions" => {
            let mut items = candidates_of(&op["candidates"]);
            // 找不到的候选：值的字节数是偶数就当找到，造一个。
            let find = |key: &str, value: &str| -> Option<WordItem> {
                if value.len() % 2 != 0 {
                    return None;
                }
                Some(WordItem { pinyin: key.to_string(), word: value.to_string(), weight: value.len() as i64, ..WordItem::default() })
            };
            apply_fixed_positions(&journal, text(op, "context"), &mut items, flag(op, "include_missing"), Some(&find), flag(op, "keep_dynamic"));
            candidates_json(&items)
        }
        "adjust_candidate_ranking" => {
            let items = candidates_of(&op["candidates"]);
            let mut changed = false;
            let ok = adjust_candidate_ranking(
                &main_db,
                &journal,
                text(op, "context"),
                &items,
                text(op, "key"),
                text(op, "value"),
                text(op, "mode"),
                int(op, "linear_step") as i32,
                int(op, "trigger") as i32,
                flag(op, "force_top"),
                Some(&mut changed),
                kind_of(text(op, "kind")),
            );
            json!([ok, changed])
        }
        "adjust_english_candidate_ranking" => {
            let items = candidates_of(&op["candidates"]);
            let mut changed = false;
            let ok = adjust_english_candidate_ranking(
                &english_db,
                &journal,
                text(op, "context"),
                &items,
                text(op, "key"),
                text(op, "value"),
                text(op, "mode"),
                int(op, "linear_step") as i32,
                int(op, "trigger") as i32,
                flag(op, "force_top"),
                Some(&mut changed),
            );
            json!([ok, changed])
        }
        "delete_dictionary_candidate" => {
            let kind = kind_of(text(op, "kind"));
            let db = if kind == DictionaryKind::English { &english_db } else { &main_db };
            json!(delete_dictionary_candidate(db, &journal, kind, text(op, "key"), text(op, "value")))
        }
        "bump_wubi_weight" => json!(bump_wubi_weight(&main_db, &journal, text(op, "key"), text(op, "value"))),
        "learn_quick_phrase_selection" => json!(learn_quick_phrase_selection(
            &main_db,
            &journal,
            text(op, "code"),
            text(op, "value"),
            flag(op, "first"),
            text(op, "mode"),
            int(op, "linear_step") as i32,
            int(op, "trigger") as i32
        )),
        "learn_quick_phrase_bypass" => json!(learn_quick_phrase_bypass(
            &journal,
            text(op, "code"),
            int(op, "rank") as i32,
            text(op, "mode"),
            int(op, "linear_step") as i32,
            int(op, "trigger") as i32,
            int(op, "max_slot") as i32
        )),
        "quick_phrase_slot" => json!(quick_phrase_slot(&journal, text(op, "code"))),
        "set_english_slot" => json!(set_english_slot(&journal, text(op, "code"), int(op, "slot") as i32)),
        "learn_english_slot_selection" => json!(learn_english_slot_selection(
            &journal,
            text(op, "code"),
            int(op, "index") as i32,
            text(op, "mode"),
            int(op, "linear_step") as i32,
            int(op, "trigger") as i32
        )),
        "learn_english_slot_bypass" => json!(learn_english_slot_bypass(
            &journal,
            text(op, "code"),
            int(op, "english_index") as i32,
            int(op, "selected") as i32,
            text(op, "mode"),
            int(op, "linear_step") as i32,
            int(op, "trigger") as i32,
            int(op, "max_slot") as i32
        )),
        "english_slot" => english_slot(&journal, text(op, "code")).map_or(Value::Null, |slot| json!(slot)),
        "learn_date_time_selection" => json!(learn_date_time_selection(
            &journal,
            text(op, "category"),
            &strings(op, "ids"),
            text(op, "format"),
            text(op, "mode"),
            int(op, "linear_step") as i32,
            int(op, "trigger") as i32
        )),
        "pin_date_time_format" => json!(pin_date_time_format(&journal, text(op, "category"), &strings(op, "ids"), text(op, "format"))),
        "apply_date_time_order" => {
            let mut items = candidates_of(&op["candidates"]);
            apply_date_time_order(&journal, text(op, "category"), &mut items, flag(op, "learned"));
            candidates_json(&items)
        }
        "learn_entered_english_word" => json!(learn_entered_english_word(&english_db, &journal, text(op, "display"), int(op, "weight"))),
        "record_pinyin_upsert_from_database" => json!(record_pinyin_upsert_from_database(&main_db, text(op, "key"), text(op, "value"), &journal)),
        "journal_queries" => {
            let kind = kind_of(text(op, "kind"));
            if flag(op, "which") {
                json!(is_user_inserted(&journal, kind, text(op, "key"), text(op, "value")))
            } else {
                json!(has_user_upsert_for_key(&journal, kind, text(op, "key")))
            }
        }
        "replay" => {
            let result = replay(&journal, &main_db, &english_db);
            json!([result.applied, result.skipped, result.failed, result.error, result.rebased])
        }
        "edit_personal_dictionary" => {
            let previous = personal_entry(&op["previous"]);
            let replacement = personal_entry(&op["replacement"]);
            let result = edit_personal_dictionary(&paths, previous.as_ref(), replacement.as_ref(), text(op, "request_id"));
            json!([result.success, result.error])
        }
        "personal_dictionary_entries" => {
            let page = personal_dictionary_entries(&paths, int(op, "offset") as usize, int(op, "limit") as usize);
            json!({"entries": page.entries.iter().map(personal_entry_json).collect::<Vec<_>>(), "has_more": page.has_more, "error": page.error})
        }
        "dictionary_state" => match dictionary_state_records(&paths) {
            Ok(records) => Value::Array(
                records
                    .iter()
                    .map(|record| match record {
                        DictionaryStateRecord::Entry(e) => {
                            json!(["entry", personal_label(e.kind), e.key, e.value, e.weight, e.display, e.deleted, e.user_inserted])
                        }
                        DictionaryStateRecord::Position(p) => json!(["position", p.context, p.key, p.value, p.position]),
                        DictionaryStateRecord::Selection(s) => json!(["selection", s.context, s.key, s.value, s.count]),
                    })
                    .collect(),
            ),
            Err(_) => json!("error"),
        },
        other => panic!("没见过的操作 {other}"),
    }
}

#[test]
fn user_dictionary_matches_cpp() {
    let golden = golden_dir();
    let baseline = fs::read_to_string(golden.join("expected_user_dictionary.jsonl")).unwrap();
    let dir = std::env::temp_dir().join(format!("msime_user_dictionary_golden_{}", std::process::id()));
    let mut failures: Vec<String> = Vec::new();
    let (mut scenario, mut step, mut checked, mut skipped_scenario) = (0i64, 0usize, 0usize, false);
    for line in baseline.lines() {
        let record: Value = serde_json::from_str(line).unwrap();
        if record["kind"] == "scenario" {
            scenario = record["id"].as_i64().unwrap();
            step = 0;
            skipped_scenario = false;
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(dir.join("user")).unwrap();
            fs::create_dir_all(dir.join("dictionaries")).unwrap();
            fs::copy(golden.join("user_dictionary_initial_main.db"), dir.join("dictionaries").join("msime.db")).unwrap();
            fs::copy(golden.join("user_dictionary_initial_english.db"), dir.join("dictionaries").join("english.db")).unwrap();
            continue;
        }
        step += 1;
        if skipped_scenario {
            continue;
        }
        let got = run_op(&record, &dir);
        let got_state = format!("{:016x}", fnv(&dump_state(&dir)));
        checked += 1;
        if got != record["ret"] || got_state != record["state"].as_str().unwrap() {
            failures.push(format!(
                "场景 {scenario} 第 {step} 步 {}\n  期望返回 {}\n  实际返回 {}\n  期望状态 {}\n  实际状态 {got_state}",
                record["op"], record["ret"], got, record["state"]
            ));
            // 一步错了后面的状态都会跟着偏，这个场景不再往下比。
            skipped_scenario = true;
        }
    }
    let _ = fs::remove_dir_all(&dir);
    assert!(checked > 2000, "只比了 {checked} 步");
    assert!(failures.is_empty(), "{} 个场景对不上，前几个：\n{}", failures.len(), failures.iter().take(4).cloned().collect::<Vec<_>>().join("\n"));
}

#[test]
fn english_schema_helpers_create_the_same_tables() {
    // ensure_schema 和 upsert_gloss 也是移植来的，这里只核对它们建出的表在 initial_english.db 里的样子。
    let dir = std::env::temp_dir().join(format!("msime_english_schema_{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let path = dir.join("english.db").to_string_lossy().into_owned();
    assert!(english::ensure_schema(&path));
    let db = Connection::open(&path).unwrap();
    let mut names: Vec<String> = db
        .prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .map(Result::unwrap)
        .collect();
    names.sort();
    let reference = Connection::open(golden_dir().join("user_dictionary_initial_english.db")).unwrap();
    let mut expected: Vec<String> = reference
        .prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .map(Result::unwrap)
        .collect();
    expected.sort();
    assert_eq!(names, expected);
    let version: i64 = db.query_row("PRAGMA user_version", [], |row| row.get(0)).unwrap();
    let expected_version: i64 = reference.query_row("PRAGMA user_version", [], |row| row.get(0)).unwrap();
    assert_eq!(version, expected_version);
    drop(db);
    let _ = fs::remove_dir_all(&dir);
}
