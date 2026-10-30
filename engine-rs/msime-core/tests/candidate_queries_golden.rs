//! 候选查询对着真实的 C++ `CandidateQueries` 核对。基线 `expected_candidate_queries.jsonl` 由
//! `msime_candidate_queries_probe` 导出：九种本地模式 × 四个方案 × 一批码（948 条），以及 1500 条英文/表情/颜文字混排
//! （随机候选列表、前缀、方案、开关、专用英文与本地模式）。要 `installer/app_data/` 下的 msime.db、others.db、english.db，缺了就跳过。

use msime_core::candidate_queries::{CandidateQueries, EnglishInputOptions, LocalInputMode, MixedExpressiveOptions};
use msime_core::datetime::DateTime;
use msime_core::lattice::{Item, Source};
use msime_core::request::SchemeType;
use msime_core::runtime_paths::RuntimeRoots;
use msime_core::shuangpin::profile;
use serde_json::Value;
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

const SOURCES: [Source; 17] = [
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
];

fn source_number(source: Source) -> usize {
    SOURCES.iter().position(|s| *s == source).unwrap()
}

fn scheme_number(scheme: SchemeType) -> u32 {
    match scheme {
        SchemeType::Quanpin => 0,
        SchemeType::Shuangpin => 1,
        SchemeType::Wubi => 2,
        SchemeType::JapaneseRomaji => 3,
    }
}

fn scheme_of(number: u64) -> SchemeType {
    [SchemeType::Quanpin, SchemeType::Shuangpin, SchemeType::Wubi, SchemeType::JapaneseRomaji][number as usize]
}

fn item_json(item: &Item) -> String {
    format!(
        "[{},{},{},{},{},{},{}]",
        quote(&item.pinyin),
        quote(&item.canonical_pinyin),
        quote(&item.word),
        item.weight,
        source_number(item.source),
        item.fixed_position,
        scheme_number(item.scheme)
    )
}

fn list_json(items: &[Item]) -> Value {
    let all: String = items.iter().map(|item| item_json(item) + "\n").collect();
    let top: Vec<String> = items.iter().take(12).map(item_json).collect();
    serde_json::from_str(&format!("{{\"n\":{},\"h\":\"{}\",\"top\":[{}]}}", items.len(), fnv(&all), top.join(","))).unwrap()
}

fn item_from(value: &Value) -> Item {
    let row = value.as_array().unwrap();
    Item {
        pinyin: row[0].as_str().unwrap().to_string(),
        canonical_pinyin: row[1].as_str().unwrap().to_string(),
        word: row[2].as_str().unwrap().to_string(),
        weight: row[3].as_i64().unwrap(),
        source: SOURCES[row[4].as_u64().unwrap() as usize],
        sentence_association: false,
        input_domains: Vec::new(),
        fuzzy: false,
        corrected_from: String::new(),
        scheme: scheme_of(row[6].as_u64().unwrap()),
        fixed_position: row[5].as_i64().unwrap() as i32,
    }
}

fn mode_of(number: u64) -> LocalInputMode {
    [
        LocalInputMode::None,
        LocalInputMode::Unicode,
        LocalInputMode::DateTime,
        LocalInputMode::QuickPhrase,
        LocalInputMode::Emoji,
        LocalInputMode::Kaomoji,
        LocalInputMode::SuperJianpin,
        LocalInputMode::TemporaryEnglish,
        LocalInputMode::TemporaryJapanese,
    ][number as usize]
}

#[test]
fn candidate_queries_match_cpp() {
    let data = app_data();
    if ["msime.db", "others.db", "english.db"].iter().any(|name| !data.join(name).exists()) {
        eprintln!("跳过：{} 下缺 msime.db / others.db / english.db", data.display());
        return;
    }
    let roots = RuntimeRoots { resources: data.clone(), user_data: data.join("user"), cache: data.join("cache"), dictionaries: data.clone() };
    let mut queries = CandidateQueries::new(roots, profile("xiaohe"));
    let text = fs::read_to_string(golden_dir().join("expected_candidate_queries.jsonl")).unwrap();
    let (mut locals, mut local_nonempty, mut diagnostics, mut mixed, mut mixed_changed) = (0, 0, 0, 0, 0);
    let mut modes_seen = [0usize; 9];
    for line in text.lines() {
        let record: Value = serde_json::from_str(line).unwrap();
        match record["kind"].as_str().unwrap() {
            "local" => {
                let mode = record["mode"].as_u64().unwrap();
                let clock = record["clock"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap() as u32).collect::<Vec<_>>();
                let now = DateTime { year: clock[0], month: clock[1], day: clock[2], weekday: clock[3], hour: clock[4], minute: clock[5], second: clock[6] };
                let engine: Vec<Item> = record["engine"].as_array().unwrap().iter().map(item_from).collect();
                let preedit = record["preedit"].as_str().unwrap();
                let scheme = scheme_of(record["scheme"].as_u64().unwrap());
                let result = queries.local(mode_of(mode), preedit, scheme, &|| now, &engine);
                assert_eq!(list_json(&result.candidates), record["candidates"], "本地模式 {mode} 预编辑 {preedit:?} 方案 {scheme:?}");
                assert_eq!(result.diagnostic.clone().map_or(Value::Null, Value::from), record["diagnostic"], "诊断 {mode} {preedit:?}");
                locals += 1;
                modes_seen[mode as usize] += usize::from(!result.candidates.is_empty());
                local_nonempty += usize::from(!result.candidates.is_empty());
                diagnostics += usize::from(!record["diagnostic"].is_null());
            }
            "mixed" => {
                let input: Vec<Item> = record["input"].as_array().unwrap().iter().map(item_from).collect();
                let english = record["english"].as_array().unwrap();
                let expressive = record["expressive"].as_array().unwrap();
                let prefix = record["prefix"].as_str().unwrap();
                let before = list_json(&input);
                let result = queries.mixed(
                    input,
                    prefix,
                    scheme_of(record["scheme"].as_u64().unwrap()),
                    EnglishInputOptions { mixed_candidates: english[0].as_u64().unwrap() == 1, minimum_prefix: english[1].as_u64().unwrap() as usize },
                    MixedExpressiveOptions { emoji_candidates: expressive[0].as_u64().unwrap() == 1, kaomoji_candidates: expressive[1].as_u64().unwrap() == 1 },
                    record["dedicated"].as_u64().unwrap() == 1,
                    mode_of(record["mode"].as_u64().unwrap()),
                );
                assert_eq!(list_json(&result), record["candidates"], "混排 前缀 {prefix:?} {line:.300}");
                mixed += 1;
                mixed_changed += usize::from(list_json(&result) != before);
            }
            other => panic!("不认识的记录 {other}"),
        }
    }
    eprintln!("候选查询：本地模式 {locals} 条（非空 {local_nonempty}，带诊断 {diagnostics}），各模式非空 {modes_seen:?}，混排 {mixed} 条（有变化 {mixed_changed}）");
    assert!(locals == 948 && local_nonempty > 600 && mixed == 1500 && mixed_changed > 600);
    assert!(modes_seen[1..].iter().all(|n| *n > 5), "每种模式都要有非空结果：{modes_seen:?}");
}
