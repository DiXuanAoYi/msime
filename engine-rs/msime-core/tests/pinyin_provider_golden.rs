//! 拼音候选提供者对着真实的 C++ `PinyinCandidateProvider` 核对。基线 `expected_pinyin_provider.jsonl` 由
//! `msime_pinyin_provider_probe` 导出：全拼和三套双拼各 300 个随机输入（经输入方案生成请求），随机的末尾辅助码、
//! 纠错开关、模糊音、词格整句、句中辅助码约束；每个输入查一次，随机再把云/AI 候选插进缓存列表后不清缓存再查一次，
//! 短输入还试单字母展开。要 `installer/app_data/` 下的 `msime.db`、`sc.lm`、`helpcodes/`，缺了就跳过。

use msime_core::helpcode;
use msime_core::kenlm::KenLm;
use msime_core::lattice::{Item, Source};
use msime_core::pinyin_provider::PinyinProvider;
use msime_core::request::SchemeType;
use msime_core::scheme::{InputScheme, QuanpinScheme, ShuangpinScheme};
use msime_core::shuangpin::profile;
use msime_core::shuangpin_query::SyllableHelpcode;
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

fn source_name(source: Source) -> &'static str {
    match source {
        Source::Database => "database",
        Source::UserDatabase => "user",
        Source::CloudSuggestion => "cloud",
        Source::AiSuggestion => "ai",
        Source::Generated => "generated",
        Source::Fallback => "fallback",
        _ => "other",
    }
}

fn scheme_number(scheme: SchemeType) -> u32 {
    match scheme {
        SchemeType::Quanpin => 0,
        SchemeType::Shuangpin => 1,
        SchemeType::Wubi => 2,
        SchemeType::JapaneseRomaji => 3,
    }
}

fn item_json(item: &Item) -> String {
    format!(
        "[{},{},{},{},\"{}\",{},{}]",
        quote(&item.pinyin),
        quote(&item.canonical_pinyin),
        quote(&item.word),
        item.weight,
        source_name(item.source),
        i32::from(item.sentence_association),
        scheme_number(item.scheme)
    )
}

fn list_json(items: &[Item]) -> Value {
    let all: String = items.iter().map(|item| item_json(item) + "\n").collect();
    let top: Vec<String> = items.iter().take(8).map(item_json).collect();
    serde_json::from_str(&format!("{{\"n\":{},\"h\":\"{}\",\"top\":[{}]}}", items.len(), fnv(&all), top.join(","))).unwrap()
}

#[test]
fn provider_matches_cpp() {
    let data = app_data();
    let (db, lm, keymap_path) = (data.join("msime.db"), data.join("sc.lm"), data.join("helpcodes").join("xiaohe_helpcode.txt"));
    if !db.exists() || !lm.exists() || !keymap_path.exists() {
        eprintln!("跳过：{} 下缺 msime.db / sc.lm / helpcodes", data.display());
        return;
    }
    let model = KenLm::open(&lm).unwrap_or_else(|e| panic!("载入失败：{e}"));
    let keymap = helpcode::parse_keymap(&fs::read_to_string(keymap_path).unwrap());
    let journal = std::env::temp_dir().join(format!("msime_pinyin_provider_journal_{}.db", std::process::id()));
    let _ = fs::remove_file(&journal);

    let text = fs::read_to_string(golden_dir().join("expected_pinyin_provider.jsonl")).unwrap();
    let records: Vec<Value> = text.lines().map(|l| serde_json::from_str(l).unwrap()).collect();
    let mut providers: std::collections::HashMap<String, PinyinProvider> = std::collections::HashMap::new();
    let (mut cases, mut nonempty, mut dynamic, mut expanded, mut helpcoded, mut fuzzy, mut mid) = (0, 0, 0, 0, 0, 0, 0);
    for record in &records {
        let name = record["profile"].as_str().unwrap();
        let quanpin = name.is_empty();
        let provider = providers.entry(name.to_string()).or_insert_with(|| {
            PinyinProvider::open(profile(if quanpin { "xiaohe" } else { name }), &db, &journal.to_string_lossy(), keymap.clone(), Some(&model))
        });
        let typed = record["typed"].as_str().unwrap();
        let mut scheme: Box<dyn InputScheme> = if quanpin { Box::new(QuanpinScheme::default()) } else { Box::new(ShuangpinScheme::new(profile(name))) };
        scheme.set_raw_input(typed, typed);
        let mut request = scheme.build_request();
        let flag = |key: &str| record[key].as_u64().unwrap() == 1;
        request.enable_quanpin_helpcode = quanpin && flag("hc");
        request.enable_shuangpin_helpcode = !quanpin && flag("hc");
        request.enable_quanpin_autocorrect_transposition = flag("tr");
        request.enable_quanpin_autocorrect_neighbor = flag("nb");
        request.fuzzy_pinyin = record["fuzzy"].as_u64().unwrap() as u32;
        request.sentence_association.word_lattice = flag("lat");
        request.sentence_association.show_next_on_duplicate = flag("next");
        if flag("mid") {
            let constraint = record["constraint"].as_array().unwrap();
            request.enable_mid_sentence_helpcode = true;
            request.syllable_helpcodes.push(SyllableHelpcode { syllable: constraint[0].as_u64().unwrap() as usize, first: constraint[1].as_u64().unwrap() as u8, second: constraint[2].as_u64().unwrap() as u8 });
            mid += 1;
        }
        let label = format!("{name:?} {typed:?} {record}");

        provider.reset_cache();
        let first = provider.query(&request);
        assert_eq!(list_json(&first), record["first"], "第一次 {label}");
        if !record["second"].is_null() {
            let kind = record["kind"].as_u64().unwrap();
            let source = if kind == 2 { Source::AiSuggestion } else { Source::CloudSuggestion };
            let word = record["word"].as_str().unwrap().to_string();
            let result = provider.cache_dynamic_candidate_for_request(&request, &word, source);
            assert_eq!(result as i64, record["rc"].as_i64().unwrap(), "插入返回值 {label}");
            assert_eq!(list_json(&provider.query(&request)), record["second"], "插入后 {label}");
            dynamic += 1;
        }
        if !record["expanded"].is_null() {
            let mut copy = first.clone();
            let changed = provider.expand_initial_candidates(&request, &mut copy);
            assert_eq!(i32::from(changed), record["expanded"]["changed"].as_i64().unwrap() as i32, "展开 {label}");
            assert_eq!(list_json(&copy), record["expanded"]["list"], "展开后 {label}");
            expanded += usize::from(changed);
        }
        cases += 1;
        nonempty += usize::from(!first.is_empty());
        helpcoded += usize::from(flag("hc"));
        fuzzy += usize::from(request.fuzzy_pinyin != 0);
    }
    let _ = fs::remove_file(&journal);
    eprintln!("拼音提供者：{cases} 个，非空 {nonempty}，带辅助码 {helpcoded}，模糊音 {fuzzy}，句中辅助码 {mid}，插动态候选 {dynamic}，展开 {expanded}");
    assert!(cases >= 1200 && nonempty > 900 && dynamic > 200 && helpcoded > 500 && fuzzy > 300 && mid > 50 && expanded > 10);
}
