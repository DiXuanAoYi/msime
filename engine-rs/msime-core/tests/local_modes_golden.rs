//! 英文词典与本地模式对着真实的 C++ 核对。基线 `engine/contracts/golden/expected_local_modes.jsonl`
//! 由 `msime_local_modes_probe` 导出。
//!
//! 查词库的测试读 `installer/app_data` 下的 `msime.db`、`others.db`、`english.db`（环境变量 `MSIME_DATA`
//! 可指向别处的数据目录）；缺了就跳过并打印原因。

use msime_core::datetime::{self, DateTime};
use msime_core::english::EnglishDictionary;
use msime_core::lattice::Source;
use msime_core::local::{self, Item};
use msime_core::shuangpin;
use msime_core::vmode;
use rusqlite::Connection;
use serde_json::{json, Value};
use std::path::PathBuf;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn rows(kind: &str) -> Vec<Value> {
    let text = std::fs::read_to_string(root().join("engine/contracts/golden/expected_local_modes.jsonl")).unwrap();
    text.lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["kind"] == kind)
        .collect()
}

fn data_file(name: &str) -> Option<PathBuf> {
    let directory = std::env::var_os("MSIME_DATA").map(PathBuf::from).unwrap_or_else(|| root().join("installer/app_data"));
    let path = directory.join(name);
    if path.exists() {
        Some(path)
    } else {
        eprintln!("跳过：没有 {}", path.display());
        None
    }
}

fn source_from(index: i64) -> Source {
    [
        Source::Database,
        Source::UserDatabase,
        Source::CloudSuggestion,
        Source::AiSuggestion,
        Source::EnglishDictionary,
        Source::QuickPhrase,
        Source::Emoji,
        Source::Kaomoji,
        Source::Generated,
        Source::Fallback,
        Source::NeuralDesktop,
        Source::NeuralKeyboard,
        Source::DateTime,
        Source::Collocation,
        Source::AppDictionary,
        Source::AiCompletion,
        Source::AiQuestion,
    ][index as usize]
}

fn items_value(items: &[Item]) -> Value {
    Value::Array(items.iter().map(|i| json!([i.pinyin, i.canonical_pinyin, i.word, i.weight, i.source as i64, i.fixed_position])).collect())
}

fn items_from(value: &Value) -> Vec<Item> {
    value
        .as_array()
        .unwrap()
        .iter()
        .map(|row| Item {
            pinyin: row[0].as_str().unwrap().to_string(),
            canonical_pinyin: row[1].as_str().unwrap().to_string(),
            word: row[2].as_str().unwrap().to_string(),
            weight: row[3].as_i64().unwrap(),
            source: source_from(row[4].as_i64().unwrap()),
            fixed_position: row[5].as_i64().unwrap() as i32,
        })
        .collect()
}

fn result_value(result: &local::QueryResult) -> Value {
    json!({"items": items_value(&result.items), "diagnostic": result.diagnostic})
}

fn date_time(value: &Value) -> DateTime {
    let n = |i: usize| value[i].as_u64().unwrap() as u32;
    DateTime { year: n(0), month: n(1), day: n(2), weekday: n(3), hour: n(4), minute: n(5), second: n(6) }
}

#[test]
fn unicode_mode_equals_cpp() {
    let (mut checked, mut produced) = (0, 0);
    for row in rows("unicode") {
        let items = vmode::query_unicode(row["input"].as_str().unwrap(), 8);
        assert_eq!(items_value(&items), row["items"], "{}", row["input"]);
        checked += 1;
        produced += usize::from(!items.is_empty());
    }
    assert!(checked >= 300 && produced > 80, "{checked}/{produced}");
    eprintln!("Unicode 模式：{checked} 个输入（出候选 {produced}）与 C++ 一致");
}

#[test]
fn v_mode_numbers_money_and_expressions_equal_cpp() {
    let (mut checked, mut numbers, mut expressions, mut produced) = (0, 0, 0, 0);
    for row in rows("vmode") {
        let input = row["input"].as_str().unwrap();
        let items = vmode::query_v_mode(input, 10);
        assert_eq!(items_value(&items), row["items"], "{input:?}");
        assert_eq!(items_value(&vmode::query_v_mode(input, 3)), row["limit3"], "{input:?} 限 3 条");
        let evaluated = vmode::evaluate_expression(input);
        assert_eq!(evaluated.as_deref(), row["eval"].as_str(), "{input:?} 求值");
        checked += 1;
        produced += usize::from(!items.is_empty());
        numbers += usize::from(input.bytes().all(|b| b.is_ascii_digit() || b == b'.'));
        expressions += usize::from(evaluated.is_some());
    }
    for row in rows("number_words") {
        let (digits, fraction) = (row["digits"].as_str().unwrap(), row["fraction"].as_str().unwrap());
        assert_eq!(vmode::chinese_number_reading(digits), row["reading"].as_str().unwrap(), "{digits:?} 读法");
        assert_eq!(vmode::chinese_financial_number(digits), row["financial"].as_str().unwrap(), "{digits:?} 大写");
        assert_eq!(vmode::chinese_money(digits, fraction), row["money"].as_str().unwrap(), "{digits:?}.{fraction:?} 金额");
    }
    assert!(checked >= 500 && numbers > 200 && expressions > 120 && produced > 300, "{checked}/{numbers}/{expressions}/{produced}");
    eprintln!("V 模式：{checked} 个输入（数字 {numbers}、可求值的算式 {expressions}）与 C++ 一致");
}

#[test]
fn date_time_formats_lunar_and_specific_inputs_equal_cpp() {
    for row in rows("datetime_meta") {
        let input = row["input"].as_str().unwrap();
        assert_eq!(datetime::is_date_time_keyword(input), row["keyword"].as_bool().unwrap(), "{input:?}");
        assert_eq!(datetime::date_time_category(input), row["category"].as_str().unwrap(), "{input:?}");
        let ids: Vec<&str> = row["ids"].as_array().unwrap().iter().map(|v| v.as_str().unwrap()).collect();
        assert_eq!(datetime::date_time_format_ids(input), ids, "{input:?} 格式 ID");
        let menu = datetime::date_time_menu_item(input);
        assert_eq!(items_value(&[menu.clone()]), json!([row["menu"][0]]), "{input:?} 菜单项");
        assert_eq!(datetime::date_time_menu_keyword(&menu), row["menu_keyword"].as_str().unwrap());
    }
    let (mut checked, mut produced, mut lunar, mut specific) = (0, 0, 0, 0);
    for row in rows("datetime") {
        let input = row["input"].as_str().unwrap();
        let now = date_time(&row["now"]);
        let limit = row["limit"].as_i64().unwrap() as i32;
        assert_eq!(datetime::is_date_time_query(input, &now), row["is_query"].as_bool().unwrap(), "{input:?} {now:?}");
        assert_eq!(datetime::date_time_category(input), row["category"].as_str().unwrap(), "{input:?}");
        let items = datetime::query_date_time(input, &now, limit);
        assert_eq!(items_value(&items), row["items"], "{input:?} {now:?}");
        checked += 1;
        produced += usize::from(!items.is_empty());
        lunar += usize::from(items.iter().any(|i| i.pinyin == "date:lunar"));
        specific += usize::from(input.as_bytes().first().is_some_and(u8::is_ascii_digit) && !items.is_empty());
    }
    assert!(checked >= 900 && produced > 500 && lunar > 50 && specific > 200, "{checked}/{produced}/{lunar}/{specific}");
    eprintln!("日期时间：{checked} 个输入（出候选 {produced}，含农历 {lunar}，指定日期时间 {specific}）与 C++ 一致");
}

#[test]
fn emoji_kaomoji_and_super_jianpin_equal_cpp() {
    let (Some(main), Some(others)) = (data_file("msime.db"), data_file("others.db")) else { return };
    let profile = shuangpin::profile("xiaohe");
    let (mut emoji, mut kaomoji, mut jianpin, mut produced, mut diagnostics) = (0, 0, 0, 0, 0);
    for row in rows("emoji") {
        let (code, sp, limit) = (row["code"].as_str().unwrap(), row["shuangpin"].as_bool().unwrap(), row["limit"].as_i64().unwrap() as i32);
        let result = local::query_emoji(code, sp, &others, limit, profile);
        assert_eq!(result_value(&result), row["result"], "表情 {code:?} 双拼 {sp}");
        emoji += 1;
        produced += usize::from(!result.items.is_empty());
    }
    for row in rows("kaomoji") {
        let (code, sp, limit) = (row["code"].as_str().unwrap(), row["shuangpin"].as_bool().unwrap(), row["limit"].as_i64().unwrap() as i32);
        let result = local::query_kaomoji(code, sp, &others, limit, profile);
        assert_eq!(result_value(&result), row["result"], "颜文字 {code:?} 双拼 {sp}");
        kaomoji += 1;
        produced += usize::from(!result.items.is_empty());
    }
    let missing = local::query_emoji("xiao", false, std::path::Path::new(""), 10, profile);
    assert_eq!(result_value(&missing), rows("emoji_missing")[0]["result"]);
    for row in rows("jianpin") {
        let (code, sp, limit) = (row["code"].as_str().unwrap(), row["shuangpin"].as_bool().unwrap(), row["limit"].as_i64().unwrap() as i32);
        assert_eq!(local::jianpin_ranking_context(code, sp, profile), row["context"].as_str().unwrap(), "简拼上下文 {code:?}");
        let result = local::query_jianpin(code, sp, &main, limit, profile);
        assert_eq!(result_value(&result), row["result"], "超级简拼 {code:?} 双拼 {sp}");
        jianpin += 1;
        produced += usize::from(!result.items.is_empty());
        diagnostics += usize::from(result.diagnostic.is_some());
    }
    assert!(emoji >= 300 && kaomoji >= 300 && jianpin >= 500 && produced > 400, "{emoji}/{kaomoji}/{jianpin}/{produced}");
    eprintln!("表情 {emoji}、颜文字 {kaomoji}、超级简拼 {jianpin} 次查询（出候选共 {produced}，带诊断 {diagnostics}）与 C++ 一致");
}

#[test]
fn quick_phrase_queries_and_placement_equal_cpp() {
    let fixture = &rows("qp_fixture")[0];
    let path = std::env::temp_dir().join(format!("msime_quick_fixture_{}.db", std::process::id()));
    let _ = std::fs::remove_file(&path);
    {
        let connection = Connection::open(&path).unwrap();
        connection.execute("CREATE TABLE quick_parases(key TEXT, value TEXT, weight INTEGER)", []).unwrap();
        for row in fixture["rows"].as_array().unwrap() {
            connection.execute("INSERT INTO quick_parases VALUES(?1,?2,?3)", rusqlite::params![row[0].as_str().unwrap(), row[1].as_str().unwrap(), row[2].as_i64().unwrap()]).unwrap();
        }
    }
    let (mut checked, mut produced) = (0, 0);
    for row in rows("quick_phrase") {
        let (code, limit) = (row["code"].as_str().unwrap(), row["limit"].as_i64().unwrap() as i32);
        let prefix = local::query_quick_phrases(code, &path, limit);
        let exact = local::query_quick_phrases_by_code(code, &path, limit);
        assert_eq!(result_value(&prefix), row["prefix"], "前缀 {code:?}");
        assert_eq!(result_value(&exact), row["exact"], "精确 {code:?}");
        checked += 1;
        produced += usize::from(!prefix.items.is_empty());
    }
    std::fs::remove_file(&path).ok();
    assert!(checked >= 90 && produced >= 40, "{checked}/{produced}");

    let (mut placed, mut with_fixed) = (0, 0);
    for row in rows("place_qp") {
        let mut candidates = items_from(&row["candidates"]);
        with_fixed += usize::from(candidates.iter().any(|c| c.fixed_position > 0));
        local::place_quick_phrases(&mut candidates, items_from(&row["phrases"]), row["slot"].as_u64().unwrap() as usize);
        assert_eq!(items_value(&candidates), row["result"], "插入位置 {row}");
        placed += 1;
    }
    assert!(placed >= 300 && with_fixed > 100);
    eprintln!("快捷短语：{checked} 个编码的前缀与精确查询、{placed} 个插入位置场景（带固定位置 {with_fixed}）与 C++ 一致");
}

fn english() -> Option<EnglishDictionary> {
    let db = data_file("english.db")?;
    let translations = root().join("engine/contracts/golden/custom_translations_fixture.txt");
    Some(EnglishDictionary::open(&db, Some(&translations)))
}

#[test]
fn english_prefix_completion_equals_cpp() {
    let Some(dictionary) = english() else { return };
    let (mut checked, mut produced) = (0, 0);
    for row in rows("english_prefix") {
        let (prefix, limit) = (row["prefix"].as_str().unwrap(), row["limit"].as_u64().unwrap() as usize);
        let items = dictionary.query_prefix(prefix, limit);
        assert_eq!(items_value(&items), row["items"], "{prefix:?} 限 {limit}");
        checked += 1;
        produced += usize::from(!items.is_empty());
    }
    assert!(checked >= 300 && produced > 200, "{checked}/{produced}");
    eprintln!("英文前缀：{checked} 次查询（出候选 {produced}）与 C++ 一致");
}

#[test]
fn english_glosses_with_custom_overrides_and_disabled_packs_equal_cpp() {
    let Some(mut dictionary) = english() else { return };
    let (mut checked, mut non_empty, mut merged) = (0, 0, 0);
    for row in rows("english_gloss") {
        let (word, disabled) = (row["word"].as_str().unwrap(), row["disabled"].as_str().unwrap());
        dictionary.set_disabled_dictionaries(disabled);
        let gloss = if row["direction"] == "zh" { dictionary.query_chinese_gloss(word) } else { dictionary.query_english_gloss(word) };
        assert_eq!(gloss, row["gloss"].as_str().unwrap(), "{} {word:?}（禁用 {disabled:?}）", row["direction"]);
        checked += 1;
        non_empty += usize::from(!gloss.is_empty());
        merged += usize::from(gloss.contains("; "));
    }
    dictionary.set_disabled_dictionaries("");
    let mut translations = 0;
    for row in rows("english_translation") {
        let (word, language) = (row["word"].as_str().unwrap(), row["language"].as_str().unwrap());
        assert_eq!(dictionary.query_translation_gloss(word, language), row["gloss"].as_str().unwrap(), "{language} {word:?}");
        translations += 1;
    }
    assert!(checked >= 400 && non_empty > 200 && merged > 20 && translations > 500, "{checked}/{non_empty}/{merged}/{translations}");
    eprintln!("英文释义：{checked} 次（非空 {non_empty}，多义项合并 {merged}）、翻译词包 {translations} 次与 C++ 一致");
}
