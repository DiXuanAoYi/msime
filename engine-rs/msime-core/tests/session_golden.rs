//! 平台入口 `Session`对着真实的 C++ 核对。基线 `expected_session.jsonl` 由 `msime_session_probe` 导出：
//! 300 个序列，每个序列随机一组会话选项（纠错、辅助码、标点、学习、模糊音、调频、本地模式、英文与表情混排、五笔混输与通配），
//! 一半从全拼开始并多半打开九键，再跑随机操作：字符（九键开着时偏向数字键）、命令、选词键、标点、选词、边缘字、置顶、删词、
//! 固定位置、整句上屏、选九键拼写、开关九键、方案切换、辅助码开关、专用英文、五笔混输、标点开关、云/AI 候选；
//! 每步比较操作结果和快照（方案、本地模式、预编辑、切分、候选、专用英文、编辑文本、光标、九键拼写、在线查询）。
//!
//! 日期时间本地模式读系统时钟，两边都关掉。学习、调频、置顶会改写词库，所以把 msime.db、english.db 拷到临时目录，
//! 序列之间的改动照常累积（探针同样如此）。要 `installer/app_data/` 下的词库、语言模型、辅助码表和日语词典，缺了就跳过。

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
use msime_core::request::SchemeType;
use msime_core::runtime_paths::RuntimeRoots;
use msime_core::session::{Session, SessionOptions};
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
        "[{},{},{},{},{},{},{},{},{}]",
        quote(&item.pinyin),
        quote(&item.canonical_pinyin),
        quote(&item.word),
        item.weight,
        SOURCES.iter().position(|s| *s == item.source).unwrap(),
        item.fixed_position,
        scheme_number(item.scheme),
        i32::from(item.sentence_association),
        i32::from(item.fuzzy)
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

fn snapshot_value(session: &Session) -> Value {
    let view = session.snapshot();
    json!({
        "scheme": scheme_number(view.scheme),
        "mode": mode_number(view.local_mode),
        "preedit": view.preedit,
        "rawseg": view.raw_segmentation,
        "normseg": view.normalized_segmentation,
        "dedicated": u32::from(view.dedicated_english),
        "editing": view.editing_text,
        "caret": view.caret_position,
        "online": online_json(&session.online_query()),
        "candidates": list_json(&view.candidates),
    })
}

fn flag(record: &Value, key: &str) -> bool {
    record[key].as_u64().unwrap() == 1
}

fn frequency_mode(number: u64) -> FrequencyAdjustmentMode {
    [FrequencyAdjustmentMode::Disabled, FrequencyAdjustmentMode::Pin, FrequencyAdjustmentMode::Halve, FrequencyAdjustmentMode::Linear, FrequencyAdjustmentMode::Promote][number as usize]
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

#[test]
fn session_matches_cpp() {
    let data = app_data();
    let (db, english, lm, keymap_path, model_path) = (data.join("msime.db"), data.join("english.db"), data.join("sc.lm"), data.join("helpcodes").join("xiaohe_helpcode.txt"), data.join("dict_japanese.dat"));
    if [&db, &english, &lm, &keymap_path, &model_path].iter().any(|path| !path.exists()) {
        eprintln!("跳过：{} 下缺 msime.db / english.db / sc.lm / helpcodes / dict_japanese.dat", data.display());
        return;
    }
    let work = std::env::temp_dir().join(format!("msime_session_{}", std::process::id()));
    let _ = fs::remove_dir_all(&work);
    let dictionaries = work.join("dicts");
    fs::create_dir_all(&dictionaries).unwrap();
    fs::copy(&db, dictionaries.join("msime.db")).unwrap();
    fs::copy(&english, dictionaries.join("english.db")).unwrap();
    let user = work.join("user");

    let model = KenLm::open(&lm).unwrap_or_else(|e| panic!("载入失败：{e}"));
    let keymap = helpcode::parse_keymap(&fs::read_to_string(keymap_path).unwrap());
    let text = fs::read_to_string(golden_dir().join("expected_session.jsonl")).unwrap();
    let (mut sequences, mut steps, mut committed) = (0, 0, 0);
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
        let mut input = InputSession::new(engine, roots.clone(), profile("xiaohe"));
        input.set_helpcode_keymap(keymap.clone());

        let freq = record["freq"].as_array().unwrap();
        let frequency = FrequencyAdjustmentOptions {
            mode: frequency_mode(freq[0].as_u64().unwrap()),
            trigger_count: freq[1].as_i64().unwrap() as i32,
            linear_step: freq[2].as_i64().unwrap() as i32,
        };
        let local: Vec<bool> = record["local"].as_array().unwrap().iter().map(|v| v.as_u64() == Some(1)).collect();
        let english_options = record["english"].as_array().unwrap();
        let expressive = record["expressive"].as_array().unwrap();
        let wubi = record["wubi"].as_array().unwrap();
        let options = SessionOptions {
            autocorrect_types: record["ac"].as_u64().unwrap() as u32,
            helpcode: flag(&record, "help"),
            chinese_punctuation: flag(&record, "punct"),
            learning: flag(&record, "learn"),
            fuzzy_pinyin: record["fuzzy"].as_u64().unwrap() as u32,
            frequency,
            local_modes: LocalModeOptions {
                unicode: local[0],
                date_time: false,
                quick_phrase: local[1],
                quick_phrase_candidates: local[2],
                quick_phrase_frequency: local[3],
                emoji: local[4],
                kaomoji: local[5],
                super_jianpin: local[6],
                temporary_english: local[7],
                temporary_japanese: local[8],
            },
            english: EnglishInputOptions { mixed_candidates: english_options[0].as_u64() == Some(1), minimum_prefix: english_options[1].as_u64().unwrap() as usize },
            expressive: MixedExpressiveOptions { emoji_candidates: expressive[0].as_u64() == Some(1), kaomoji_candidates: expressive[1].as_u64() == Some(1) },
            wubi: WubiInputOptions { mixed_pinyin: wubi[0].as_u64() == Some(1), z_wildcard: wubi[1].as_u64() == Some(1) },
        };
        let mut session = Session::new(input, &options).expect("会话选项");
        assert_eq!(snapshot_value(&session), record["init"], "初始状态 第 {} 序列", record["round"]);

        let mut trail: Vec<String> = Vec::new();
        for (index, step) in record["ops"].as_array().unwrap().iter().enumerate() {
            let op = step["op"].as_str().unwrap();
            let mut result = Value::Null;
            match op {
                "char" => result = key_result_json(&session.character(step["c"].as_u64().unwrap() as u8 as char, flag(step, "shift"))),
                "command" => result = key_result_json(&session.command(command_of(step["c"].as_u64().unwrap()))),
                "candidate_key" => result = key_result_json(&session.candidate_key(step["c"].as_u64().unwrap() as u8 as char)),
                "punctuation" => result = key_result_json(&session.punctuation(step["c"].as_u64().unwrap() as u8 as char)),
                "select" => result = key_result_json(&session.select(step["i"].as_u64().unwrap() as usize)),
                "edge" => result = key_result_json(&session.select_edge(step["i"].as_u64().unwrap() as usize, if flag(step, "last") { CandidateEdge::LastHan } else { CandidateEdge::FirstHan })),
                "pin" => result = key_result_json(&session.pin(step["i"].as_u64().unwrap() as usize)),
                "remove" => result = key_result_json(&session.remove(step["i"].as_u64().unwrap() as usize)),
                "fix" => result = key_result_json(&session.fix_position(step["i"].as_u64().unwrap() as usize, step["p"].as_i64().unwrap() as i32)),
                "clear" => result = key_result_json(&session.clear_position(step["i"].as_u64().unwrap() as usize)),
                "finish" => result = key_result_json(&session.finish(step["i"].as_u64().unwrap() as usize)),
                "switch" => session.switch_scheme(scheme_of(step["to"].as_u64().unwrap())),
                "helpcode" => session.set_helpcode_enabled(flag(step, "v")),
                "dedicated" => session.set_dedicated_english(flag(step, "v")),
                "wubi_mixed" => session.set_wubi_mixed_pinyin(flag(step, "v")),
                "punct" => session.set_chinese_punctuation_enabled(flag(step, "v")),
                "online" => {
                    let query = session.online_query();
                    assert_eq!(online_json(&query), step["q"], "在线查询 {op} 第 {index} 步");
                    let source = if flag(step, "cloud") { Source::CloudSuggestion } else { Source::AiSuggestion };
                    let applied = query.as_ref().is_some_and(|query| session.apply_online_candidate(query, step["w"].as_str().unwrap(), source));
                    result = json!(u32::from(applied));
                }
                other => panic!("不认识的操作 {other}"),
            }
            trail.push(format!("{op}({})", step.get("c").or(step.get("i")).or(step.get("v")).map_or(String::new(), |v| v.to_string())));
            let label = format!("第 {} 序列 第 {index} 步 起始 {} 步骤 {} 选项 {line:.300}", record["round"], record["start"], trail.join(" "));
            if result != step["result"] && std::env::var_os("MSIME_DEBUG").is_some() {
                eprintln!("RUST 状态 {}\nC++ 状态 {}", snapshot_value(&session)["candidates"], step["state"]["candidates"]);
            }
            assert_eq!(result, step["result"], "操作结果 {label}");
            let state = snapshot_value(&session);
            assert_eq!(state, step["state"], "状态 {label}");
            steps += 1;
            committed += usize::from(result["c"].is_string());
        }
        sequences += 1;
    }
    let _ = fs::remove_dir_all(&work);
    eprintln!("会话门面：{sequences} 个序列 {steps} 步，有上屏 {committed}");
    assert!(sequences == 420 && steps > 14000 && committed > 1000);
}
