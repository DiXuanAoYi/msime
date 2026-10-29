//! 输入会话对着真实的 C++ `ImeSession` 核对。基线 `expected_ime_session.jsonl` 由 `msime_ime_session_probe` 导出：
//! 全拼、双拼、五笔（含拼音混输与通配）、日语各 120 个序列，每个序列随机一组会话开关（辅助码、句中辅助码与大写触发、
//! 纠错、模糊音、词格整句、五笔混输/通配、前文），再跑随机操作：按键、整串替换、重置、清缓存、动态候选、单字母展开、
//! 方案切换、光标前缀查询、不带句中辅助码的再查；每步比较预编辑、请求的主要字段和候选列表。
//! 后 240 个序列从双拼开始并打开直接辅助码（斜杠、大写两种终止标记随机），请求在查询前由解析器改写。要 `installer/app_data/` 下的词库、语言模型、辅助码表和日语词典，缺了就跳过。

use msime_core::helpcode;
use msime_core::ime_session::{ImeSession, WubiInputOptions};
use msime_core::kenlm::KenLm;
use msime_core::lattice::{Item, Source};
use msime_core::pinyin_provider::PinyinProvider;
use msime_core::provider::ProviderRegistry;
use msime_core::quanpin_dictionary::SentenceAssociation;
use msime_core::request::SchemeType;
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

fn scheme_of(number: u64) -> SchemeType {
    [SchemeType::Quanpin, SchemeType::Shuangpin, SchemeType::Wubi, SchemeType::JapaneseRomaji][number as usize]
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

fn state_value(session: &ImeSession) -> Value {
    let request = session.request();
    serde_json::json!({
        "scheme": scheme_number(session.current_scheme_type()),
        "preedit": session.preedit(),
        "raw": request.raw_input,
        "seg": request.segmentation,
        "rawseg": request.raw_segmentation,
        "valid": u32::from(request.valid),
        "wild": u32::from(request.wubi_z_wildcard),
        "complete": u32::from(session.wubi_code_is_complete()),
        "candidates": list_json(session.candidates()),
    })
}

#[test]
fn session_matches_cpp() {
    let data = app_data();
    let (db, lm, keymap_path, model_path) = (data.join("msime.db"), data.join("sc.lm"), data.join("helpcodes").join("xiaohe_helpcode.txt"), data.join("dict_japanese.dat"));
    if !db.exists() || !lm.exists() || !keymap_path.exists() || !model_path.exists() {
        eprintln!("跳过：{} 下缺 msime.db / sc.lm / helpcodes / dict_japanese.dat", data.display());
        return;
    }
    let model = KenLm::open(&lm).unwrap_or_else(|e| panic!("载入失败：{e}"));
    let keymap = helpcode::parse_keymap(&fs::read_to_string(keymap_path).unwrap());
    let journal = std::env::temp_dir().join(format!("msime_ime_session_journal_{}.db", std::process::id()));

    let text = fs::read_to_string(golden_dir().join("expected_ime_session.jsonl")).unwrap();
    let (mut sequences, mut steps, mut nonempty, mut mixed, mut dynamic, mut raw_queries, mut switches, mut rewritten) = (0, 0, 0, 0, 0, 0, 0, 0);
    for line in text.lines() {
        let record: Value = serde_json::from_str(line).unwrap();
        let _ = fs::remove_file(&journal);
        let pinyin = PinyinProvider::open(profile("xiaohe"), &db, &journal.to_string_lossy(), keymap.clone(), Some(&model));
        let registry = ProviderRegistry::new(pinyin, &db, &journal.to_string_lossy(), &model_path);
        let mut session = ImeSession::new(scheme_of(record["start"].as_u64().unwrap()), profile("xiaohe"), registry);
        session.set_helpcode_keymap(keymap.clone());
        let flag = |key: &str| record[key].as_u64().unwrap() == 1;
        session.set_shuangpin_helpcode_enabled(flag("sh"));
        session.set_mid_sentence_helpcode_enabled(flag("mid"));
        session.set_mid_sentence_uppercase_trigger_enabled(flag("upper"));
        session.set_quanpin_helpcode_enabled(flag("qh"));
        session.set_quanpin_autocorrect_types(record["ac"].as_u64().unwrap() as u32);
        session.set_fuzzy_pinyin_options(record["fuzzy"].as_u64().unwrap() as u32);
        session.set_sentence_association(SentenceAssociation { word_lattice: flag("lat"), ..Default::default() });
        session.set_wubi_input_options(WubiInputOptions { mixed_pinyin: flag("mixed"), z_wildcard: flag("wildcard") });
        let wubi_mixed = flag("mixed");
        // 探针里前文只在设置后读一次请求（此时请求还空）：这里按基线里记下的前文设置。
        session.set_rescoring_context(record["ctx"].as_str().unwrap());
        let direct = record.get("direct").is_some_and(|v| v.as_u64() == Some(1));
        if direct {
            session.set_direct_helpcode_enabled(true);
            session.set_direct_helpcode_markers(record["slash"].as_u64().unwrap() == 1, record["upm"].as_u64().unwrap() == 1);
        }

        let mut trail: Vec<String> = Vec::new();
        for (index, step) in record["ops"].as_array().unwrap().iter().enumerate() {
            let op = step["op"].as_str().unwrap();
            let mut extra = Value::Null;
            match op {
                "key" => session.handle_key(step["a"].as_u64().unwrap() as u32, 0, step["b"].as_u64().unwrap() as u16),
                "replace" => session.replace_active_raw_input(step["s"].as_str().unwrap(), step["c"].as_str().unwrap()),
                "reset" => session.reset(),
                "reset_cache" => session.reset_cache(),
                "dynamic" => {
                    let source = if step["cloud"].as_u64().unwrap() == 1 { Source::CloudSuggestion } else { Source::AiSuggestion };
                    extra = serde_json::json!(session.apply_dynamic_candidate(step["w"].as_str().unwrap(), source));
                    dynamic += 1;
                }
                "expand" => extra = serde_json::json!(u32::from(session.expand_initial_candidates())),
                "switch" => {
                    session.switch_scheme(scheme_of(step["to"].as_u64().unwrap()));
                    switches += 1;
                }
                "raw_query" => {
                    let prefix = step["s"].as_str().unwrap();
                    let raw = list_json(&session.query_raw_candidates(prefix, prefix));
                    let plain = list_json(&session.query_without_syllable_helpcodes());
                    extra = serde_json::json!({"raw": raw, "plain": plain});
                    raw_queries += 1;
                }
                other => panic!("不认识的操作 {other}"),
            }
            trail.push(format!("{op}({})", step.get("s").or(step.get("a")).map_or(String::new(), |v| v.to_string())));
            let label = format!("第 {} 序列 第 {index} 步 {} 起始 {} 开关 {line:.200}", record["round"], trail.join(" "), record["start"]);
            assert_eq!(extra, step["extra"], "额外结果 {label}");
            assert_eq!(state_value(&session), step["state"], "状态 {label}");
            steps += 1;
            nonempty += usize::from(!session.candidates().is_empty());
            rewritten += usize::from(direct && session.request().has_syllable_helpcode_layout);
            mixed += usize::from(session.current_scheme_type() == SchemeType::Wubi && wubi_mixed && !session.candidates().is_empty());
        }
        sequences += 1;
    }
    let _ = fs::remove_file(&journal);
    eprintln!("会话：{sequences} 个序列 {steps} 步，候选非空 {nonempty}，五笔混输非空 {mixed}，动态候选 {dynamic}，前缀查询 {raw_queries}，切方案 {switches}，直接辅助码改写 {rewritten}");
    assert!(sequences >= 720 && steps > 6000 && nonempty > 4000 && mixed > 300 && dynamic > 400 && raw_queries > 50 && switches > 100 && rewritten > 100);
}
