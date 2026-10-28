//! 词库状态暂存和运行时目录对着真实的 C++ 核对。基线 `expected_dictionary_state.jsonl` 由 `msime_dictionary_state_probe`
//! 导出，初始库是用户词库探针的 `user_dictionary_initial_{main,english}.db`：
//!
//! - `stage`：260 个场景的 `stage_dictionary_state`——随机记录（词条、固定位置、选择计数），夹着越界、空键、超长、
//!   身份重复、位置冲突，随机记录上限，新目录/已存在/落在资源目录里三种目标，合法与非法的内容 ID。比成败、目录是否留下、
//!   成功时暂存出的日志与词库转储哈希，以及再从暂存结果导出的状态；
//! - `prepare`：80 个场景的 `prepare_runtime_paths`——新建、重复准备、重复准备前日志里多了改动、根目录重叠、缺文件的旧代、
//!   已有 `.incoming`、非法 ID。比每次调用的成败和结果转储哈希。

use msime_core::runtime_paths::{prepare_runtime_paths, stage_dictionary_state, RuntimeRoots};
use msime_core::user_dictionary::{
    self, dictionary_state_records, DictionaryKind, DictionaryStateEntry, DictionaryStatePosition, DictionaryStateRecord, DictionaryStateSelection, PersonalDictionaryKind,
};
use rusqlite::types::ValueRef;
use rusqlite::{Connection, OpenFlags};
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};

fn golden_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden")
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

fn cell(value: ValueRef) -> String {
    match value {
        ValueRef::Null => "<null>".to_string(),
        ValueRef::Integer(v) => v.to_string(),
        ValueRef::Real(v) => v.to_string(),
        ValueRef::Text(t) => String::from_utf8_lossy(t).into_owned(),
        ValueRef::Blob(b) => String::from_utf8_lossy(b).into_owned(),
    }
}

fn dump_table(db: &Connection, table: &str, columns: &str, order: &str) -> String {
    let mut text = format!("T:{table}\n");
    let Ok(mut statement) = db.prepare(&format!("SELECT {columns} FROM \"{table}\" ORDER BY {order}")) else { return text + "(无)\n" };
    let count = statement.column_count();
    let mut rows = statement.query([]).unwrap();
    while let Some(row) = rows.next().unwrap() {
        let cells: Vec<String> = (0..count).map(|i| cell(row.get_ref(i).unwrap())).collect();
        text += &cells.join("|");
        text.push('\n');
    }
    text
}

fn dump_file(file: &Path, tables: &[(&str, &str, &str)]) -> String {
    let Ok(db) = Connection::open_with_flags(file, OpenFlags::SQLITE_OPEN_READ_ONLY) else { return "(无)\n".to_string() };
    if !file.exists() {
        return "(无)\n".to_string();
    }
    tables.iter().map(|(table, columns, order)| dump_table(&db, table, columns, order)).collect()
}

fn dump_journal(file: &Path) -> String {
    dump_file(
        file,
        &[
            ("user_dictionary_operations", "dictionary,key,value,operation,weight,display,user_inserted", "1,2,3"),
            ("candidate_selection_state", "context_key,entry_key,value,selection_count", "1,2,3"),
            ("fixed_candidate_positions", "context_key,entry_key,value,position", "1,2,3"),
        ],
    )
}

fn dump_dictionaries(dir: &Path) -> String {
    let mut text = String::from("== main\n");
    text += &dump_file(
        &dir.join("msime.db"),
        &[("tbl_1_n", "*", "1,3"), ("tbl_2_n", "*", "1,3"), ("tbl_2_j", "*", "1,3"), ("tbl_3_n", "*", "1,3"), ("wubi86", "*", "1,2"), ("quick_parases", "*", "1,2")],
    );
    text += "== english\n";
    text += &dump_file(&dir.join("english.db"), &[("english_words", "*", "1,2")]);
    text
}

fn state_hash(roots: &RuntimeRoots) -> String {
    let ready = if roots.dictionaries.join(".ready").exists() { "ready\n" } else { "noready\n" };
    fnv(&format!("{}{}{ready}", dump_journal(&roots.user_data.join("msime_user.db")), dump_dictionaries(&roots.dictionaries)))
}

fn kind_name(kind: PersonalDictionaryKind) -> &'static str {
    match kind {
        PersonalDictionaryKind::Pinyin => "pinyin",
        PersonalDictionaryKind::Wubi => "wubi",
        PersonalDictionaryKind::English => "english",
        PersonalDictionaryKind::QuickPhrase => "quick",
    }
}

fn records_json(records: &[DictionaryStateRecord]) -> String {
    let items: Vec<String> = records
        .iter()
        .map(|record| match record {
            DictionaryStateRecord::Entry(v) => format!(
                "[\"entry\",\"{}\",{},{},{},{},{},{}]",
                kind_name(v.kind),
                quote(&v.key),
                quote(&v.value),
                v.weight,
                quote(&v.display),
                i32::from(v.deleted),
                i32::from(v.user_inserted)
            ),
            DictionaryStateRecord::Position(v) => format!("[\"position\",{},{},{},{}]", quote(&v.context), quote(&v.key), quote(&v.value), v.position),
            DictionaryStateRecord::Selection(v) => format!("[\"selection\",{},{},{},{}]", quote(&v.context), quote(&v.key), quote(&v.value), v.count),
        })
        .collect();
    format!("[{}]", items.join(","))
}

fn parse_record(value: &Value) -> DictionaryStateRecord {
    let text = |i: usize| value[i].as_str().unwrap().to_string();
    match value[0].as_str().unwrap() {
        "entry" => DictionaryStateRecord::Entry(DictionaryStateEntry {
            kind: match value[1].as_str().unwrap() {
                "pinyin" => PersonalDictionaryKind::Pinyin,
                "wubi" => PersonalDictionaryKind::Wubi,
                "english" => PersonalDictionaryKind::English,
                _ => PersonalDictionaryKind::QuickPhrase,
            },
            key: text(2),
            value: text(3),
            weight: value[4].as_i64().unwrap(),
            display: text(5),
            deleted: value[6] == 1,
            user_inserted: value[7] == 1,
        }),
        "position" => DictionaryStateRecord::Position(DictionaryStatePosition { context: text(1), key: text(2), value: text(3), position: value[4].as_i64().unwrap() as i32 }),
        _ => DictionaryStateRecord::Selection(DictionaryStateSelection { context: text(1), key: text(2), value: text(3), count: value[4].as_i64().unwrap() as i32 }),
    }
}

fn copy_fixture(resources: &Path) {
    let _ = fs::remove_dir_all(resources);
    fs::create_dir_all(resources).unwrap();
    fs::copy(golden_dir().join("user_dictionary_initial_main.db"), resources.join("msime.db")).unwrap();
    fs::copy(golden_dir().join("user_dictionary_initial_english.db"), resources.join("english.db")).unwrap();
}

fn records() -> Vec<Value> {
    fs::read_to_string(golden_dir().join("expected_dictionary_state.jsonl")).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

#[test]
fn stage_and_prepare_match_cpp() {
    let work = std::env::temp_dir().join(format!("msime_dictionary_state_golden_{}", std::process::id()));
    let _ = fs::remove_dir_all(&work);
    fs::create_dir_all(&work).unwrap();
    let root = work.join("scenario");
    let (mut staged_ok, mut staged_failed, mut prepared) = (0, 0, 0);
    let all = records();
    for record in &all {
        let id = record["id"].as_i64().unwrap();
        let _ = fs::remove_dir_all(&root);
        let resources = root.join("resources");
        copy_fixture(&resources);
        match record["kind"].as_str().unwrap() {
            "stage" => {
                let list: Vec<DictionaryStateRecord> = record["records"].as_array().unwrap().iter().map(parse_record).collect();
                assert_eq!(serde_json::from_str::<Value>(&records_json(&list)).unwrap(), record["records"], "记录往返 {id}");
                let place = record["place"].as_str().unwrap();
                let generation = match place {
                    "inside" => resources.join("gen"),
                    _ => root.join("gen"),
                };
                if place == "exists" {
                    fs::create_dir_all(&generation).unwrap();
                }
                let mut index = 0;
                let mut next = || -> Result<Option<DictionaryStateRecord>, String> {
                    if index == list.len() {
                        return Ok(None);
                    }
                    index += 1;
                    Ok(Some(list[index - 1].clone()))
                };
                let result = stage_dictionary_state(&resources, &generation, record["content"].as_str().unwrap(), &mut next, record["max"].as_u64().unwrap() as usize);
                let ok = result.is_ok();
                assert_eq!(ok, record["ok"].as_bool().unwrap(), "stage {id} {place} content={} records={}", record["content"], record["records"]);
                assert_eq!(generation.exists(), record["gen_exists"].as_bool().unwrap(), "目录是否留下 {id}");
                if let Ok(roots) = result {
                    staged_ok += 1;
                    assert_eq!(state_hash(&roots), record["state"].as_str().unwrap(), "暂存状态 {id}");
                    let exported = dictionary_state_records(&roots.user_paths()).unwrap();
                    assert_eq!(fnv(&records_json(&exported)), record["streamed"].as_str().unwrap(), "导出状态 {id}");
                } else {
                    staged_failed += 1;
                }
            }
            "prepare" => {
                let case = record["case"].as_str().unwrap();
                let content = record["content"].as_str().unwrap();
                let (mut user, mut cache) = (root.join("user"), root.join("cache"));
                match case {
                    "overlap_user" => user = resources.join("user"),
                    "overlap_cache" => cache = user.join("cache"),
                    _ => {}
                }
                let mut log = String::new();
                let mut attempt = |id_value: &str| -> bool {
                    match prepare_runtime_paths(&resources, &user, &cache, id_value) {
                        Ok(roots) => {
                            log += &format!("ok:{};", state_hash(&roots));
                            true
                        }
                        Err(_) => {
                            log += "error;";
                            false
                        }
                    }
                };
                if case == "incoming" {
                    fs::create_dir_all(user.join("dictionaries").join(format!("{content}.incoming"))).unwrap();
                }
                let first = attempt(content);
                let mode = ["fresh", "again", "after_upsert", "overlap_user", "overlap_cache", "incomplete", "incoming", "bad_id"].iter().position(|m| *m == case).unwrap();
                if first && mode >= 1 && !matches!(mode, 3 | 4 | 6 | 7) {
                    let journal = user.join("msime_user.db").to_string_lossy().into_owned();
                    if mode == 2 {
                        assert!(user_dictionary::record_upsert(&journal, DictionaryKind::Pinyin, "ni'hao", "拟好", 777, ""));
                        assert!(user_dictionary::record_user_insert(&journal, DictionaryKind::Wubi, "wq", "测", 123, ""));
                    }
                    if mode == 5 {
                        fs::remove_file(user.join("dictionaries").join(content).join("english.db")).unwrap();
                    }
                    attempt(content);
                    if mode == 2 {
                        attempt(record["third"].as_str().unwrap());
                    }
                }
                prepared += usize::from(first);
                assert_eq!(log, record["log"].as_str().unwrap(), "prepare {id} {case}");
            }
            other => panic!("没见过的记录 {other}"),
        }
    }
    let _ = fs::remove_dir_all(&work);
    eprintln!("词库状态：暂存成功 {staged_ok} 失败 {staged_failed}，prepare 首次成功 {prepared}");
    assert!(staged_ok > 80 && staged_failed > 100 && prepared >= 40);
}
