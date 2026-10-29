//! 输入会话对着真实的 C++ `InputSession` 核对。基线 `expected_input_session.jsonl` 由 `msime_input_session_probe` 导出：
//! 全拼、双拼、五笔（含拼音混输与通配）、日语各 100 个序列，每个序列随机一组会话选项（辅助码、直接辅助码、纠错、模糊音、
//! 本地模式开关、英文与表情混排、调频策略、固定位置），再跑随机操作：字符、命令、选词键、标点、选词、整句上屏、边缘字、
//! 置顶、删词、固定位置、光标、整串替换、重置、清缓存、方案切换、云/AI 候选、专用英文、选项切换；
//! 每步比较操作结果和会话状态（预编辑、编辑文本、光标、各种切分、候选、云查询状态、在线查询）。
//!
//! 学习、造词、删词会改写词库，所以测试把 msime.db、english.db 拷到临时目录，序列之间的改动照常累积（探针同样如此）。
//! 要 `installer/app_data/` 下的词库、语言模型、辅助码表和日语词典，缺了就跳过。

use msime_core::datetime::DateTime;
use msime_core::helpcode;
use msime_core::ime_session::{ImeSession, WubiInputOptions};
use msime_core::input_session::{
    CandidateEdge, Command, EnglishInputOptions, FrequencyAdjustmentMode, FrequencyAdjustmentOptions, InputSession, KeyResult, LocalInputMode, LocalModeOptions, MixedExpressiveOptions,
    OnlineQuery,
};
use msime_core::kenlm::KenLm;
use msime_core::lattice::{Item, Source};
use msime_core::pinyin_provider::PinyinProvider;
use msime_core::provider::ProviderRegistry;
use msime_core::quanpin_dictionary::SentenceAssociation;
use msime_core::request::SchemeType;
use msime_core::runtime_paths::RuntimeRoots;
use msime_core::shuangpin::profile;
use serde_json::{json, Value};
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

fn mode_number(mode: LocalInputMode) -> u32 {
    match mode {
        LocalInputMode::None => 0,
        LocalInputMode::Unicode => 1,
        LocalInputMode::DateTime => 2,
        LocalInputMode::QuickPhrase => 3,
        LocalInputMode::Emoji => 4,
        LocalInputMode::Kaomoji => 5,
        LocalInputMode::SuperJianpin => 6,
        LocalInputMode::TemporaryEnglish => 7,
        LocalInputMode::TemporaryJapanese => 8,
    }
}

fn item_json(item: &Item) -> String {
    format!(
        "[{},{},{},{},{},{},{},{}]",
        quote(&item.pinyin),
        quote(&item.canonical_pinyin),
        quote(&item.word),
        item.weight,
        SOURCES.iter().position(|s| *s == item.source).unwrap(),
        item.fixed_position,
        scheme_number(item.scheme),
        i32::from(item.sentence_association)
    )
}

fn list_json(items: &[Item]) -> Value {
    let all: String = items.iter().map(|item| item_json(item) + "\n").collect();
    let top: Vec<String> = items.iter().take(8).map(item_json).collect();
    serde_json::from_str(&format!("{{\"n\":{},\"h\":\"{}\",\"top\":[{}]}}", items.len(), fnv(&all), top.join(","))).unwrap()
}

fn key_result_json(result: &KeyResult) -> Value {
    json!({"h": u32::from(result.handled), "c": result.commit, "d": result.diagnostic})
}

fn online_json(query: &Option<OnlineQuery>) -> Value {
    match query {
        None => Value::Null,
        Some(q) => json!({
            "scheme": scheme_number(q.scheme),
            "identity": q.identity,
            "text": q.query_text,
            "key": q.cache_key,
            "segments": q.pinyin_segments,
            "cloud": u32::from(q.cloud_eligible),
            "ai": u32::from(q.ai_eligible),
        }),
    }
}

fn state_value(session: &InputSession) -> Value {
    let cloud = session.get_cloud_query_state();
    let forms = session.get_shuangpin_preedit_forms();
    json!({
        "scheme": scheme_number(session.scheme()),
        "engine_scheme": scheme_number(session.current_scheme_type()),
        "mode": mode_number(session.local_input_mode()),
        "has": u32::from(session.has_composition()),
        "preedit": session.preedit(),
        "editing": session.editing_text(),
        "caret": session.caret_position(),
        "rawseg": session.raw_segmentation(),
        "normseg": session.normalized_segmentation(),
        "seq": session.get_pinyin_sequence(),
        "cases": session.get_pinyin_sequence_with_cases(),
        "pure": session.get_pure_pinyin_sequence(),
        "seg": session.get_pinyin_segmentation(),
        "segcases": session.get_pinyin_segmentation_with_cases(),
        "forms": [forms.raw, forms.quanpin],
        "quanpin": session.get_quanpin(),
        "complete": u32::from(session.is_all_complete_pure_pinyin()),
        "reads": u32::from(session.reads_as_pinyin()),
        "wubi_unique": u32::from(session.wubi_unique_four_code()),
        "wubi_complete": u32::from(session.wubi_four_code_is_complete()),
        "active_help": u32::from(session.has_active_helpcode()),
        "mid_help": u32::from(session.has_mid_sentence_helpcode()),
        "prefix_end": session.prefix_end(),
        "pending": session.pending_suffix(),
        "bounds": session.segment_raw_boundaries(),
        "cloud": [u32::from(cloud.should_query), cloud.query_text, cloud.cache_key, cloud.committed_pinyin],
        "online": online_json(&session.online_query()),
        "candidates": list_json(session.candidates()),
    })
}

fn flag(record: &Value, key: &str) -> bool {
    record[key].as_u64().unwrap() == 1
}

#[test]
fn session_matches_cpp() {
    let data = app_data();
    let (db, english, lm, keymap_path, model_path) = (data.join("msime.db"), data.join("english.db"), data.join("sc.lm"), data.join("helpcodes").join("xiaohe_helpcode.txt"), data.join("dict_japanese.dat"));
    if [&db, &english, &lm, &keymap_path, &model_path].iter().any(|path| !path.exists()) {
        eprintln!("跳过：{} 下缺 msime.db / english.db / sc.lm / helpcodes / dict_japanese.dat", data.display());
        return;
    }
    let work = std::env::temp_dir().join(format!("msime_input_session_{}", std::process::id()));
    let _ = fs::remove_dir_all(&work);
    let dictionaries = work.join("dicts");
    fs::create_dir_all(&dictionaries).unwrap();
    fs::copy(&db, dictionaries.join("msime.db")).unwrap();
    fs::copy(&english, dictionaries.join("english.db")).unwrap();
    let user = work.join("user");

    let model = KenLm::open(&lm).unwrap_or_else(|e| panic!("载入失败：{e}"));
    let keymap = helpcode::parse_keymap(&fs::read_to_string(keymap_path).unwrap());
    let text = fs::read_to_string(golden_dir().join("expected_input_session.jsonl")).unwrap();
    let (mut sequences, mut steps, mut with_composition, mut local_steps, mut committed, mut caret_steps, mut online_applied) = (0, 0, 0, 0, 0, 0, 0);
    for line in text.lines() {
        let record: Value = serde_json::from_str(line).unwrap();
        let _ = fs::remove_dir_all(&user);
        fs::create_dir_all(&user).unwrap();
        let journal = user.join("msime_user.db");
        let start = scheme_of(record["start"].as_u64().unwrap());
        let pinyin = PinyinProvider::open(profile("xiaohe"), &dictionaries.join("msime.db"), &journal.to_string_lossy(), keymap.clone(), Some(&model));
        let registry = ProviderRegistry::new(pinyin, &dictionaries.join("msime.db"), &journal.to_string_lossy(), &model_path);
        let engine = ImeSession::new(start, profile("xiaohe"), registry);
        let roots = RuntimeRoots { resources: data.clone(), user_data: user.clone(), cache: work.join("cache"), dictionaries: dictionaries.clone() };
        let mut session = InputSession::new(engine, roots, profile("xiaohe"));
        session.set_quanpin_autocorrect_types(record["ac"].as_u64().unwrap() as u32);
        session.set_chinese_punctuation_enabled(flag(&record, "punct"));
        session.set_candidate_learning_enabled(flag(&record, "learn"));
        session.set_helpcode_keymap(keymap.clone());

        session.set_shuangpin_helpcode_enabled(flag(&record, "sh"));
        session.set_mid_sentence_helpcode_enabled(flag(&record, "mid"));
        session.set_mid_sentence_uppercase_trigger_enabled(flag(&record, "upper"));
        session.set_quanpin_helpcode_enabled(flag(&record, "qh"));
        session.set_fuzzy_pinyin_options(record["fuzzy"].as_u64().unwrap() as u32);
        session.set_sentence_association(SentenceAssociation { word_lattice: flag(&record, "lat"), ..Default::default() });
        session.set_wubi_input_options(WubiInputOptions { mixed_pinyin: flag(&record, "mixed"), z_wildcard: flag(&record, "wildcard") });
        if flag(&record, "direct") {
            session.set_direct_helpcode_enabled(true);
            session.set_direct_helpcode_markers(flag(&record, "slash"), flag(&record, "upm"));
        }
        let local: Vec<bool> = record["local"].as_array().unwrap().iter().map(|v| v.as_u64() == Some(1)).collect();
        session.set_local_mode_options(LocalModeOptions {
            unicode: local[0],
            date_time: local[1],
            quick_phrase: local[2],
            quick_phrase_candidates: local[3],
            quick_phrase_frequency: local[4],
            emoji: local[5],
            kaomoji: local[6],
            super_jianpin: local[7],
            temporary_english: local[8],
            temporary_japanese: local[9],
        });
        let english_options = record["english"].as_array().unwrap();
        session.set_english_input_options(EnglishInputOptions { mixed_candidates: english_options[0].as_u64() == Some(1), minimum_prefix: english_options[1].as_u64().unwrap() as usize });
        let expressive = record["expressive"].as_array().unwrap();
        session.set_mixed_expressive_options(MixedExpressiveOptions { emoji_candidates: expressive[0].as_u64() == Some(1), kaomoji_candidates: expressive[1].as_u64() == Some(1) });
        if let Some(freq) = record["freq"].as_array() {
            session.set_frequency_adjustment(FrequencyAdjustmentOptions {
                mode: frequency_mode(freq[0].as_u64().unwrap()).unwrap(),
                trigger_count: freq[1].as_i64().unwrap() as i32,
                linear_step: freq[2].as_i64().unwrap() as i32,
            });
        }
        if flag(&record, "fixed") {
            session.enable_fixed_positions();
        }
        let clock = DateTime { year: 2024, month: 12, day: 25, weekday: 3, hour: 14, minute: 30, second: 15 };
        session.set_local_date_time_provider(Box::new(move || clock));
        assert_eq!(state_value(&session), record["init"], "初始状态 第 {} 序列 {line:.300}", record["round"]);

        let mut trail: Vec<String> = Vec::new();
        for (index, step) in record["ops"].as_array().unwrap().iter().enumerate() {
            let op = step["op"].as_str().unwrap();
            let mut result = Value::Null;
            match op {
                "char" => result = key_result_json(&session.handle_character(step["c"].as_u64().unwrap() as u8 as char, flag(step, "shift"))),
                "command" => result = key_result_json(&session.handle_command(command_of(step["c"].as_u64().unwrap()))),
                "candidate_key" => result = key_result_json(&session.handle_candidate_key(step["c"].as_u64().unwrap() as u8 as char)),
                "punctuation" => result = key_result_json(&session.handle_punctuation(step["c"].as_u64().unwrap() as u8 as char)),
                "select" => result = key_result_json(&session.select_candidate(step["i"].as_u64().unwrap() as usize)),
                "finish" => result = key_result_json(&session.finish_composition(step["i"].as_u64().unwrap() as usize)),
                "select_word" => result = key_result_json(&session.select_candidate_by_word(step["w"].as_str().unwrap())),
                "edge" => result = key_result_json(&session.select_candidate_edge(step["i"].as_u64().unwrap() as usize, if flag(step, "last") { CandidateEdge::LastHan } else { CandidateEdge::FirstHan })),
                "pin" => result = key_result_json(&session.pin_candidate(step["i"].as_u64().unwrap() as usize)),
                "remove" => result = key_result_json(&session.remove_candidate(step["i"].as_u64().unwrap() as usize)),
                "position" => result = key_result_json(&session.set_candidate_position(step["i"].as_u64().unwrap() as usize, step["p"].as_i64().unwrap() as i32)),
                "caret" => {
                    let value = step["v"].as_i64().unwrap();
                    session.set_caret(if value < 0 { None } else { Some(value as usize) });
                    session.recompute_candidates();
                    caret_steps += 1;
                }
                "sequence" => {
                    let raw = step["s"].as_str().unwrap();
                    session.set_pinyin_sequence(raw);
                    if flag(step, "cases") {
                        session.set_pinyin_sequence_with_cases(raw);
                    }
                    session.recompute_candidates();
                }
                "reset_state" => session.reset_state(),
                "reset_cache" => session.reset_cache(),
                "switch" => session.switch_scheme(scheme_of(step["to"].as_u64().unwrap())),
                "online" => {
                    let query = session.online_query();
                    assert_eq!(online_json(&query), step["q"], "在线查询 {op} 第 {index} 步");
                    let source = if flag(step, "cloud") { Source::CloudSuggestion } else { Source::AiSuggestion };
                    let applied = query.as_ref().is_some_and(|query| session.apply_online_candidate(query, step["w"].as_str().unwrap(), source));
                    online_applied += usize::from(applied);
                    result = json!(u32::from(applied));
                }
                "cache_dynamic" => {
                    let source = if flag(step, "cloud") { Source::CloudSuggestion } else { Source::AiSuggestion };
                    let pinyin = session.get_pinyin_sequence().to_string();
                    result = json!(session.cache_dynamic_candidate(&pinyin, step["w"].as_str().unwrap(), source));
                }
                "dedicated" => session.set_dedicated_english_mode(flag(step, "v")),
                "expressive" => {
                    let english = step["english"].as_array().unwrap();
                    result = json!(u32::from(session.set_english_input_options(EnglishInputOptions { mixed_candidates: english[0].as_u64() == Some(1), minimum_prefix: english[1].as_u64().unwrap() as usize })));
                    let expressive = step["expressive"].as_array().unwrap();
                    session.set_mixed_expressive_options(MixedExpressiveOptions { emoji_candidates: expressive[0].as_u64() == Some(1), kaomoji_candidates: expressive[1].as_u64() == Some(1) });
                }
                "expand" => result = json!(u32::from(session.expand_initial_candidates())),
                "accepts" => {
                    let caret = session.caret_position();
                    result = json!([
                        u32::from(session.accepts_mid_sentence_helpcode_marker()),
                        u32::from(session.accepts_mid_sentence_helpcode_marker_at(caret)),
                        u32::from(session.accepts_direct_helpcode_slash_at(caret))
                    ]);
                }
                "frequency" => {
                    // 模式 5 在 C++ 里是不认识的枚举值，设置被拒绝。
                    let accepted = frequency_mode(step["mode"].as_u64().unwrap()).is_some_and(|mode| {
                        session.set_frequency_adjustment(FrequencyAdjustmentOptions { mode, trigger_count: step["trigger"].as_i64().unwrap() as i32, linear_step: step["step"].as_i64().unwrap() as i32 })
                    });
                    result = json!(u32::from(accepted));
                }
                other => panic!("不认识的操作 {other}"),
            }
            trail.push(format!("{op}({})", step.get("c").or(step.get("i")).or(step.get("s")).or(step.get("w")).map_or(String::new(), |v| v.to_string())));
            let label = format!("第 {} 序列 第 {index} 步 起始 {} 步骤 {} 选项 {line:.400}", record["round"], record["start"], trail.join(" "));
            if result != step["result"] && std::env::var_os("MSIME_DEBUG").is_some() {
                eprintln!("RUST 状态 {}
C++ 状态 {}", state_value(&session)["candidates"], step["state"]["candidates"]);
            }
            assert_eq!(result, step["result"], "操作结果 {label}");
            assert_eq!(state_value(&session), step["state"], "状态 {label}");
            steps += 1;
            with_composition += usize::from(session.has_composition());
            local_steps += usize::from(session.local_input_mode() != LocalInputMode::None);
            committed += usize::from(result["c"].is_string());
        }
        sequences += 1;
    }
    let _ = fs::remove_dir_all(&work);
    eprintln!("会话：{sequences} 个序列 {steps} 步，有组合 {with_composition}，本地模式 {local_steps}，有上屏 {committed}，光标 {caret_steps}，在线候选 {online_applied}");
    assert!(sequences == 400 && steps > 7000 && with_composition > 3500 && local_steps > 500 && committed > 600 && caret_steps > 300 && online_applied > 20);
}

fn frequency_mode(number: u64) -> Option<FrequencyAdjustmentMode> {
    [FrequencyAdjustmentMode::Disabled, FrequencyAdjustmentMode::Pin, FrequencyAdjustmentMode::Halve, FrequencyAdjustmentMode::Linear, FrequencyAdjustmentMode::Promote].get(number as usize).copied()
}

fn command_of(number: u64) -> Command {
    [
        Command::Backspace,
        Command::CommitCandidate,
        Command::CommitRaw,
        Command::Cancel,
        Command::MoveLeft,
        Command::MoveRight,
        Command::MoveHome,
        Command::MoveEnd,
        Command::DeleteForward,
    ][number as usize]
}
