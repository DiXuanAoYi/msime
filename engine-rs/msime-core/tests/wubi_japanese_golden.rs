//! 罗马字转换、五笔、日语对着真实的 C++ 核对。基线 `engine/contracts/golden/expected_wubi_japanese.jsonl`
//! 由 `msime_wubi_japanese_probe` 导出。
//!
//! 需要词库的测试读 `installer/app_data/msime.db`（或 `MSIME_DB`），需要日语模型的读
//! `installer/app_data/dict_japanese.dat`（或 `MSIME_JA_MODEL`）；缺了就跳过并打印原因。

use msime_core::japanese::{self, Decoder, Lemma, Provider};
use msime_core::lattice::Source;
use msime_core::romaji;
use msime_core::wubi::{self, Dictionary as WubiDictionary};
use serde_json::{json, Value};
use std::path::PathBuf;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn rows(kind: &str) -> Vec<Value> {
    let text = std::fs::read_to_string(root().join("engine/contracts/golden/expected_wubi_japanese.jsonl")).unwrap();
    text.lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["kind"] == kind)
        .collect()
}

fn existing(variable: &str, fallback: &str) -> Option<PathBuf> {
    let path = std::env::var_os(variable).map(PathBuf::from).unwrap_or_else(|| root().join(fallback));
    if path.exists() {
        Some(path)
    } else {
        eprintln!("跳过：没有 {}", path.display());
        None
    }
}

fn database() -> Option<PathBuf> {
    existing("MSIME_DB", "installer/app_data/msime.db")
}

fn model() -> Option<PathBuf> {
    existing("MSIME_JA_MODEL", "installer/app_data/dict_japanese.dat")
}

struct Fnv(u64);

impl Fnv {
    fn new() -> Self {
        Fnv(0xcbf29ce484222325)
    }
    fn byte(&mut self, b: u8) {
        self.0 ^= u64::from(b);
        self.0 = self.0.wrapping_mul(0x100000001b3);
    }
    fn field(&mut self, text: &str) {
        text.bytes().for_each(|b| self.byte(b));
        self.byte(0xFF);
    }
    fn hex(&self) -> String {
        format!("{:016x}", self.0)
    }
}

/// 和探针的 `enumerate` 同一个次序：长度从短到长，高位在前。
fn enumerate(alphabet: &str, max_len: usize, mut visit: impl FnMut(&str)) {
    let alphabet: Vec<char> = alphabet.chars().collect();
    for length in 1..=max_len {
        let mut digits = vec![0usize; length];
        loop {
            let text: String = digits.iter().map(|&d| alphabet[d]).collect();
            visit(&text);
            let mut position = length as isize - 1;
            while position >= 0 {
                digits[position as usize] += 1;
                if digits[position as usize] == alphabet.len() {
                    digits[position as usize] = 0;
                    position -= 1;
                } else {
                    break;
                }
            }
            if position < 0 {
                break;
            }
        }
    }
}

/// 把一批行写成探针的 `{"n","h","top"}`：哈希覆盖全部字段（字符串原样、数字十进制），前 `top` 行展开。
fn list(rows: Vec<Vec<Value>>, top: usize) -> Value {
    let mut hash = Fnv::new();
    for row in &rows {
        for cell in row {
            match cell {
                Value::String(s) => hash.field(s),
                other => hash.field(&other.to_string()),
            }
        }
    }
    json!({"n": rows.len(), "h": hash.hex(), "top": rows.into_iter().take(top).collect::<Vec<_>>()})
}

fn lemma_rows(lemmas: Vec<Lemma>) -> Vec<Vec<Value>> {
    lemmas
        .into_iter()
        .map(|l| vec![json!(l.reading), json!(l.surface), json!(l.left_id), json!(l.right_id), json!(l.word_cost), json!(l.token_id)])
        .collect()
}

fn sentence_rows(sentences: Vec<japanese::SentenceCandidate>) -> Vec<Vec<Value>> {
    sentences.into_iter().map(|s| vec![json!(s.text), json!(s.cost)]).collect()
}

fn wubi_rows(items: &[wubi::Candidate]) -> Vec<Vec<Value>> {
    // scheme 一列：五笔候选显式标成 Wubi（枚举值 2）。
    items.iter().map(|c| vec![json!(c.pinyin), json!(c.canonical_pinyin), json!(c.word), json!(c.weight), json!(c.source as i64), json!(2)]).collect()
}

fn japanese_rows(items: &[japanese::Candidate]) -> Vec<Vec<Value>> {
    // 日语候选标上自己的方案（JapaneseRomaji = 3），否则调频、置顶、造词会把罗马音当拼音键写进拼音日志（#869）。
    items.iter().map(|c| vec![json!(c.pinyin), json!(c.canonical_pinyin), json!(c.word), json!(c.weight), json!(c.source as i64), json!(3)]).collect()
}

fn strings(value: &Value) -> Vec<String> {
    value.as_array().unwrap().iter().map(|v| v.as_str().unwrap().to_string()).collect()
}

// ---------- 罗马字 ----------

#[test]
fn every_short_romaji_string_converts_like_cpp() {
    let want = &rows("romaji_all")[0];
    let (mut hash, mut count) = (Fnv::new(), 0u64);
    let mut visit = |text: &str| {
        let c = romaji::convert(text);
        hash.field(text);
        hash.field(&c.hiragana);
        hash.field(&c.pending);
        hash.byte(u8::from(c.complete));
        hash.byte(0xFE);
        count += 1;
    };
    enumerate("abcdefghijklmnopqrstuvwxyz'-", 3, &mut visit);
    enumerate("abcdefghijklmnopqrstuvwxyz", 4, |text| {
        if text.len() == 4 {
            visit(text);
        }
    });
    assert_eq!(count, want["convert"]["n"].as_u64().unwrap());
    assert_eq!(hash.hex(), want["convert"]["h"].as_str().unwrap(), "≤3 位（含 ' 和 -）与 4 位字母串的转换结果");

    let (mut prefix_hash, mut prefix_count) = (Fnv::new(), 0u64);
    enumerate("abcdefghijklmnopqrstuvwxyz", 4, |text| {
        prefix_hash.field(text);
        romaji::kana_for_prefix(text).iter().for_each(|k| prefix_hash.field(k));
        prefix_hash.byte(0xFE);
        prefix_count += 1;
    });
    assert_eq!(prefix_count, want["prefix"]["n"].as_u64().unwrap());
    assert_eq!(prefix_hash.hex(), want["prefix"]["h"].as_str().unwrap(), "前缀对应的假名集合");
    eprintln!("罗马字：{count} 个输入的转换结果、{prefix_count} 个前缀的假名集合与 C++ 一致");
}

#[test]
fn sampled_romaji_conversions_equal_cpp() {
    let (mut checked, mut pending, mut incomplete_empty) = (0, 0, 0);
    for row in rows("romaji") {
        let input = row["input"].as_str().unwrap();
        let c = romaji::convert(input);
        assert_eq!(c.hiragana, row["hiragana"].as_str().unwrap(), "{input:?} 的假名");
        assert_eq!(c.pending, row["pending"].as_str().unwrap(), "{input:?} 的尾巴");
        assert_eq!(c.complete, row["complete"].as_bool().unwrap(), "{input:?} complete");
        assert_eq!(romaji::is_single_kana(&c), row["single"].as_bool().unwrap(), "{input:?} 单假名");
        assert_eq!(romaji::hiragana_to_katakana(&c.hiragana), row["katakana"].as_str().unwrap(), "{input:?} 片假名");
        assert_eq!(romaji::hiragana_to_romaji(&c.hiragana), row["back"].as_str().unwrap(), "{input:?} 转回罗马字");
        assert_eq!(romaji::kana_for_prefix(&c.pending), strings(&row["prefix_kana"]), "{input:?} 前缀假名");
        checked += 1;
        pending += usize::from(!c.pending.is_empty());
        incomplete_empty += usize::from(c.hiragana.is_empty());
    }
    assert!(checked >= 1500 && pending > 200 && incomplete_empty > 50, "{checked} 条，没打完 {pending}，纯尾巴 {incomplete_empty}");
    eprintln!("罗马字：{checked} 条随机输入（没打完的 {pending} 条）逐项与 C++ 一致");
}

#[test]
fn kana_back_to_romaji_equals_cpp_including_ties() {
    let mut checked = 0;
    for row in rows("kana_romaji") {
        let kana = row["kana"].as_str().unwrap();
        assert_eq!(romaji::hiragana_to_romaji(kana), row["romaji"].as_str().unwrap(), "{kana:?}");
        checked += 1;
    }
    assert!(checked > 900, "{checked}");
    eprintln!("假名转回罗马字：{checked} 条（含全部单假名、拗音组合、っ/ん 夹杂）与 C++ 一致");
}

// ---------- 五笔 ----------

fn replay_wubi(row: &Value) -> (Vec<Value>, wubi::Scheme) {
    let mut scheme = wubi::Scheme::new();
    scheme.set_mixed_pinyin_allowed(row["mixed"].as_bool().unwrap());
    scheme.set_z_wildcard(row["wild"].as_bool().unwrap());
    let mut snapshots = Vec::new();
    for op in row["ops"].as_array().unwrap() {
        match op[0].as_str().unwrap() {
            "k" => scheme.handle_key(op[1].as_u64().unwrap() as u32, op[2].as_u64().unwrap() as u32, op[3].as_u64().unwrap() as u16),
            "x" => scheme.set_extended_length_allowed(op[1].as_u64().unwrap() == 1),
            "r" => scheme.set_raw_input(op[1].as_str().unwrap(), op[2].as_str().unwrap()),
            "z" => scheme.reset(),
            other => panic!("未知操作 {other}"),
        }
        snapshots.push(json!([scheme.preedit(), scheme.has_complete_code()]));
    }
    (snapshots, scheme)
}

#[test]
fn wubi_key_handling_equals_cpp() {
    let (mut checked, mut with_input, mut wildcard, mut full) = (0, 0, 0, 0);
    for (number, row) in rows("wubi_scheme").into_iter().enumerate() {
        let (snapshots, scheme) = replay_wubi(&row);
        assert_eq!(Value::Array(snapshots), row["snapshots"], "场景 {number}：每步的预编辑");
        let request = scheme.build_request();
        let want_strings = strings(&row["strings"]);
        assert!(want_strings.iter().all(|s| *s == request.code), "场景 {number}：{want_strings:?} ≠ {:?}", request.code);
        let strokes: Vec<Value> = request.key_strokes.iter().map(|k| json!([k.vk, k.modifiers, k.wch])).collect();
        assert_eq!(Value::Array(strokes), row["strokes"], "场景 {number}：按键记录");
        assert_eq!(request.z_wildcard, row["wildcard"].as_bool().unwrap(), "场景 {number}：通配");
        assert_eq!(request.valid, row["valid"].as_bool().unwrap(), "场景 {number}：valid");
        checked += 1;
        with_input += usize::from(request.valid);
        wildcard += usize::from(request.z_wildcard);
        full += usize::from(scheme.has_complete_code());
    }
    assert!(checked >= 350 && with_input > 150 && wildcard > 8 && full > 20, "{checked}/{with_input}/{wildcard}/{full}");
    eprintln!("五笔按键：{checked} 个场景（有输入 {with_input}，通配 {wildcard}，满四码 {full}）的预编辑、请求与 C++ 一致");
}

#[test]
fn wubi_table_queries_equal_cpp() {
    let Some(path) = database() else { return };
    let dictionary = WubiDictionary::open(&path).expect("打不开词库");
    let (mut checked, mut empty, mut wildcard, mut deduped) = (0, 0, 0, 0);
    for row in rows("wubi_query") {
        let code = row["code"].as_str().unwrap();
        let mut scheme = wubi::Scheme::new();
        scheme.set_z_wildcard(row["wild"].as_bool().unwrap());
        scheme.set_raw_input(code, "");
        let request = scheme.build_request();
        assert_eq!(request.valid, row["valid"].as_bool().unwrap(), "{code:?} valid");
        let items = dictionary.query(&request);
        assert_eq!(list(wubi_rows(&items), 6), row["rows"], "{code:?}（通配 {}）", row["wild"]);
        checked += 1;
        empty += usize::from(items.is_empty());
        wildcard += usize::from(request.z_wildcard);
        deduped += usize::from(items.len() > 30);
    }
    assert!(checked >= 1000 && wildcard > 150 && deduped > 20 && empty > 30, "{checked}/{wildcard}/{deduped}/{empty}");
    eprintln!("五笔码表：{checked} 次查询（通配 {wildcard}，结果超过 30 条的 {deduped}，空 {empty}）的候选、权重、次序与 C++ 一致");
}

// ---------- 日语方案 ----------

#[test]
fn japanese_key_handling_equals_cpp() {
    let (mut checked, mut with_input, mut with_apostrophe, mut with_minus) = (0, 0, 0, 0);
    for (number, row) in rows("ja_scheme").into_iter().enumerate() {
        let mut scheme = japanese::Scheme::new();
        let mut snapshots = Vec::new();
        for op in row["ops"].as_array().unwrap() {
            scheme.handle_key(op[0].as_u64().unwrap() as u32, op[1].as_u64().unwrap() as u32, op[2].as_u64().unwrap() as u16);
            snapshots.push(json!(scheme.preedit()));
        }
        assert_eq!(Value::Array(snapshots), row["snapshots"], "场景 {number}：每步的预编辑");
        let request = scheme.build_request();
        let got = vec![request.raw_input.clone(), request.raw_input_with_cases.clone(), request.normalized_input.clone(), request.raw_segmentation.clone(), request.normalized_segmentation.clone(), request.segmentation.clone()];
        assert_eq!(got, strings(&row["strings"]), "场景 {number}：请求里的六个字符串");
        let strokes: Vec<Value> = request.key_strokes.iter().map(|k| json!([k.vk, k.modifiers, k.wch])).collect();
        assert_eq!(Value::Array(strokes), row["strokes"], "场景 {number}：按键记录");
        assert_eq!(request.valid, row["valid"].as_bool().unwrap());
        checked += 1;
        with_input += usize::from(request.valid);
        with_apostrophe += usize::from(request.raw_input.contains('\''));
        with_minus += usize::from(request.raw_input.contains('-'));
    }
    for row in rows("ja_set_raw") {
        let raw = row["raw"].as_str().unwrap();
        // 探针随机选了“带大小写串”或“不带”，两种都要能还原：基线里 raw_input_with_cases 就是答案。
        let mut scheme = japanese::Scheme::new();
        scheme.set_raw_input(raw, raw);
        let request = scheme.build_request();
        let got = vec![request.raw_input, request.raw_input_with_cases, request.normalized_input, request.raw_segmentation, request.normalized_segmentation, request.segmentation];
        assert_eq!(got, strings(&row["strings"]), "set_raw_input({raw:?})");
        assert_eq!(request.valid, row["valid"].as_bool().unwrap());
    }
    assert!(checked >= 300 && with_input > 150 && with_apostrophe > 10 && with_minus > 10, "{checked}/{with_input}/{with_apostrophe}/{with_minus}");
    eprintln!("日语按键：{checked} 个场景（有输入 {with_input}，含 ' {with_apostrophe}，含 - {with_minus}）与 C++ 一致");
}

// ---------- 日语模型 ----------

#[test]
fn connection_costs_equal_cpp() {
    let Some(path) = model() else { return };
    let decoder = Decoder::open(&path).unwrap();
    let (mut checked, mut out_of_range) = (0, 0);
    for row in rows("ja_connection") {
        let (right, left) = (row["right"].as_u64().unwrap() as u16, row["left"].as_u64().unwrap() as u16);
        let cost = decoder.connection_cost(right, left);
        assert_eq!(i64::from(cost), row["cost"].as_i64().unwrap(), "({right}, {left})");
        checked += 1;
        out_of_range += usize::from(cost == 10000);
    }
    assert!(checked >= 1500 && out_of_range > 50, "{checked}/{out_of_range}");
    eprintln!("日语连接成本：{checked} 对（越界 {out_of_range}）与 C++ 一致，词条 {} 个", decoder.token_count());
}

#[test]
fn single_kana_prefix_lemmas_equal_cpp() {
    let Some(path) = model() else { return };
    let decoder = Decoder::open(&path).unwrap();
    let mut checked = 0;
    for row in rows("ja_prefix") {
        let prefix = row["prefix"].as_str().unwrap();
        assert_eq!(list(lemma_rows(decoder.prefix_lemmas(prefix, 24)), 6), row["p24"], "{prefix:?} 前 24");
        assert_eq!(list(lemma_rows(decoder.prefix_lemmas(prefix, 100)), 4), row["p100"], "{prefix:?} 前 100");
        checked += 1;
    }
    assert!(checked >= 120);
    eprintln!("日语前缀词条：{checked} 个假名前缀（前 24 与前 100）与 C++ 一致");
}

#[test]
fn lemma_lookups_sentence_decoding_and_matrix_search_equal_cpp() {
    let Some(path) = model() else { return };
    let decoder = Decoder::open(&path).unwrap();
    let (mut checked, mut pending, mut sentences) = (0, 0, 0);
    for row in rows("ja_decode") {
        let input = row["input"].as_str().unwrap();
        let c = romaji::convert(&input.to_ascii_lowercase());
        assert_eq!(c.hiragana, row["hiragana"].as_str().unwrap(), "{input:?}");
        assert_eq!(c.pending, row["pending"].as_str().unwrap(), "{input:?}");
        let h = c.hiragana.as_str();
        assert_eq!(list(lemma_rows(decoder.exact_lemmas(h, 16)), 5), row["exact"], "{input:?} 精确词条");
        assert_eq!(list(lemma_rows(decoder.prefix_lemmas(h, 24)), 5), row["prefix"], "{input:?} 前缀词条");
        assert_eq!(list(lemma_rows(decoder.longer_prefix_lemmas(h, 16)), 5), row["longer"], "{input:?} 更长的前缀词条");
        if c.pending.is_empty() {
            assert!(row["continuing"].is_null());
        } else {
            let next = romaji::kana_for_prefix(&c.pending);
            assert_eq!(list(lemma_rows(decoder.prefix_lemmas_continuing(h, &next, 32)), 5), row["continuing"], "{input:?} 接续词条");
            pending += 1;
        }
        assert_eq!(list(sentence_rows(decoder.decode(h, 8)), 8), row["decode"], "{input:?} 整句解码");
        let found = japanese::search_converted(&decoder, &c, 12);
        sentences += usize::from(!found.is_empty());
        assert_eq!(list(sentence_rows(found), 12), row["search"], "{input:?} 矩阵搜索");
        checked += 1;
    }
    assert!(checked >= 200 && pending > 20 && sentences > 20, "{checked}/{pending}/{sentences}");
    eprintln!("日语解码：{checked} 个输入（没打完的 {pending}）的词条查询、整句解码、矩阵搜索与 C++ 一致");
}

// ---------- 日语候选提供者 ----------

fn request_for(input: &str) -> japanese::Request {
    let mut scheme = japanese::Scheme::new();
    scheme.set_raw_input(input, input);
    scheme.build_request()
}

#[test]
fn japanese_provider_candidates_equal_cpp() {
    let (Some(db), Some(model)) = (database(), model()) else { return };
    let provider = Provider::open(&db, &model).unwrap();
    let (mut checked, mut long_readings, mut generated_first, mut with_rows) = (0, 0, 0, 0);
    for row in rows("ja_query") {
        let input = row["input"].as_str().unwrap();
        let request = request_for(input);
        assert_eq!(request.raw_input, row["raw"].as_str().unwrap());
        let items = provider.query(&request, &[]);
        assert_eq!(list(japanese_rows(&items), 24), row["rows"], "输入 {input:?}");
        checked += 1;
        long_readings += usize::from(romaji::convert(&request.raw_input).hiragana.len() >= 6);
        generated_first += usize::from(items.first().is_some_and(|c| c.source == Source::Generated));
        with_rows += usize::from(items.len() > 3);
    }
    assert!(checked >= 500 && long_readings > 50 && generated_first > 10 && with_rows > 300, "{checked}/{long_readings}/{generated_first}/{with_rows}");
    eprintln!("日语候选：{checked} 个输入（读音 ≥ 6 字节的 {long_readings}，首位是假名直出的 {generated_first}）的候选、权重、来源、次序与 C++ 一致");
}

#[test]
fn cached_cloud_candidates_are_inserted_like_cpp() {
    let (Some(db), Some(model)) = (database(), model()) else { return };
    let provider = Provider::open(&db, &model).unwrap();
    for row in rows("ja_dynamic") {
        let input = row["input"].as_str().unwrap();
        let request = request_for(input);
        // 同一个来源再缓存会替换上一条，所以只有最后一条留着。
        let last = strings(&row["cached"]).pop().unwrap();
        let cached = [japanese::Candidate { pinyin: request.raw_input.clone(), canonical_pinyin: request.raw_input.clone(), word: last, weight: 1, source: Source::CloudSuggestion }];
        let items = provider.query(&request, &cached);
        assert_eq!(list(japanese_rows(&items), 12), row["rows"], "输入 {input:?}");
    }
}
