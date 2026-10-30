//! 直接辅助码的整句解码与解析器对着真实的 C++ `direct_helpcode::decode_best_path` / `Resolver::resolve` 核对。
//! 基线 `expected_direct_helpcode.jsonl` 由 `msime_direct_helpcode_probe` 导出：九套内置方案 × 220 条随机输入串，
//! 词库、码表、辅码谓词都是哈希合成的（这里写同一套），词条权重互不相同，免得束截断的平局次序影响结果。
//! 带语言模型的场景需要 `installer/app_data/sc.lm`（或 `MSIME_LM`），缺了就跳过那些。

use msime_core::direct_lattice::{decode_best_path, DecodeOptions, DecodedPath, WordEdgeMemo};
use msime_core::direct_resolver::{ResolveContext, Resolver};
use msime_core::kenlm::KenLm;
use msime_core::lattice::{LanguageModel, Lexeme};
use msime_core::shuangpin::profile;
use msime_core::spelling_graph::{build_spelling_graph, SpellingKind, SpellingOptions};
use serde_json::Value;
use std::collections::HashMap;
use std::path::PathBuf;

fn fnv(text: &str, seed: u64) -> u64 {
    let mut hash = seed;
    for b in text.bytes() {
        hash ^= u64::from(b);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}
const SEED: u64 = 0xcbf29ce484222325;

fn hanzi(idx: u32) -> String {
    char::from_u32(0x4E00 + idx * 37).unwrap().to_string()
}

fn lexicon(span: &[String]) -> Vec<Lexeme> {
    let key = span.join("'");
    let h = fnv(&key, SEED);
    let count = if h % 25 == 0 { 0 } else { 2 + (h >> 8) % 5 };
    (0..count)
        .map(|r| {
            let hr = fnv(&format!("{key}#{r}"), h);
            let chars = span.len() + usize::from((hr >> 8) % 8 == 0);
            let value: String = (0..chars).map(|i| hanzi((fnv(&format!("{key}#{r}/{i}"), SEED) % 40) as u32)).collect();
            Lexeme {
                key: if hr % 4 == 0 { String::new() } else { key.clone() },
                value,
                weight: (1 + (hr >> 16) % 5_000_000) as i64,
                penalty: if hr % 11 == 0 { 1.0 } else { 0.0 },
                input_domains: Vec::new(),
            }
        })
        .collect()
}

fn aux_code(idx: u32) -> String {
    let mut code = String::new();
    code.push((b'a' + (idx % 26) as u8) as char);
    if idx % 13 != 0 {
        code.push((b'a' + ((idx * 7) % 26) as u8) as char);
    }
    code
}

fn keymap() -> HashMap<String, String> {
    (0..40).filter(|idx| idx % 9 != 0).map(|idx| (hanzi(idx), aux_code(idx))).collect()
}

fn aux_predicate(quanpin: &str, first: u8, second: u8) -> bool {
    let hash = fnv(quanpin, SEED);
    let hash = fnv(&String::from_utf8_lossy(&[first, second]), hash);
    hash % 3 != 0
}

/// 与探针里 `collocation()` 逐字相同的字级搭配回调（见 lattice_golden.rs）。
fn collocation(tail: &str, word: &str, rear: bool) -> f64 {
    let last = tail.chars().last().map(|c| c as u32).unwrap_or(0);
    let first = word.chars().next().map(|c| c as u32).unwrap_or(0);
    let value = last * 31 + first * 17 + tail.chars().count() as u32 * 3 + if rear { 5 } else { 0 };
    ((value % 13) as i32 - 6) as f64 * 0.25
}

fn kind_name(kind: SpellingKind) -> &'static str {
    match kind {
        SpellingKind::Plain => "plain",
        SpellingKind::Aux1 => "aux1",
        SpellingKind::Aux2Slash => "aux2_slash",
        SpellingKind::Aux2Upper => "aux2_upper",
        SpellingKind::Aux2Abbrev => "aux2_abbrev",
        SpellingKind::Initial => "initial",
    }
}

fn rows(kind: &str) -> Vec<Value> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_direct_helpcode.jsonl");
    std::fs::read_to_string(path)
        .unwrap()
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["kind"] == kind)
        .collect()
}

fn model() -> Option<KenLm> {
    let path = std::env::var_os("MSIME_LM")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../installer/app_data/sc.lm"));
    if path.exists() {
        Some(KenLm::open(&path).unwrap_or_else(|e| panic!("载入失败：{e}")))
    } else {
        eprintln!("跳过带语言模型的场景：没有 {}", path.display());
        None
    }
}

fn options<'a>(o: &Value, model: Option<&'a dyn LanguageModel>, scorer: &'a dyn Fn(&str, &str, bool) -> f64) -> DecodeOptions<'a> {
    let mut options = DecodeOptions {
        beam: o["beam"].as_i64().unwrap() as i32,
        span_limit: o["span_limit"].as_i64().unwrap() as i32,
        max_phrase_syllables: o["max_phrase"].as_i64().unwrap() as i32,
        max_sequences_per_start: o["max_sequences"].as_i64().unwrap() as i32,
        initial_penalty: o["initial_penalty"].as_f64().unwrap(),
        unigram_z: o["unigram_z"].as_f64().unwrap(),
        phrase_length_bonus: o["phrase_length_bonus"].as_f64().unwrap(),
        collocation_weight: o["collocation_weight"].as_f64().unwrap(),
        ..DecodeOptions::default()
    };
    if o["collocation"].as_bool().unwrap() {
        options.collocation_scorer = Some(scorer);
    }
    if o["lm"].as_bool().unwrap() {
        options.language_model = model;
    }
    options
}

fn assert_path(got: &Option<DecodedPath>, want: &Value, ctx: &str) {
    if want.is_null() {
        assert!(got.is_none(), "应无路径 {ctx}");
        return;
    }
    let got = got.as_ref().unwrap_or_else(|| panic!("应有路径 {ctx}"));
    let syllables: Vec<(u64, u64, &str)> = got.syllables.iter().map(|s| (s.begin as u64, s.end as u64, kind_name(s.kind))).collect();
    let want_syllables: Vec<(u64, u64, &str)> =
        want["syllables"].as_array().unwrap().iter().map(|s| (s[0].as_u64().unwrap(), s[1].as_u64().unwrap(), s[2].as_str().unwrap())).collect();
    assert_eq!(syllables, want_syllables, "音节 {ctx}");
    assert_eq!(got.sentence, want["sentence"].as_str().unwrap(), "整句 {ctx}");
    assert_eq!(got.key, want["key"].as_str().unwrap(), "键 {ctx}");
    let words: Vec<&str> = want["words"].as_array().unwrap().iter().map(|w| w.as_str().unwrap()).collect();
    assert_eq!(got.words, words, "词 {ctx}");
    assert_eq!(got.complete, want["complete"].as_bool().unwrap(), "完整 {ctx}");
    assert!((got.log_prob - want["log_prob"].as_f64().unwrap()).abs() < 1e-9, "分数 {} ≠ {} {ctx}", got.log_prob, want["log_prob"]);
}

#[test]
fn direct_decoding_equals_cpp() {
    let model = model();
    let keymap = keymap();
    let (mut checked, mut with_path, mut complete, mut with_model, mut memo_checked) = (0, 0, 0, 0, 0);
    for row in rows("decode") {
        let o = &row["options"];
        if o["lm"].as_bool().unwrap() && model.is_none() {
            continue;
        }
        let typed = row["typed"].as_str().unwrap();
        let spelling = SpellingOptions { slash_marker: row["slash"].as_bool().unwrap(), uppercase_marker: row["upper"].as_bool().unwrap() };
        let ctx = format!("{} {typed:?}", row["profile"]);
        let graph = build_spelling_graph(typed, profile(row["profile"].as_str().unwrap()), Some(&aux_predicate), &spelling);
        let lookup = |span: &[String], _: bool| lexicon(span);
        let accept = |hz: &str, first: u8, second: u8| {
            keymap.get(hz).is_some_and(|code| {
                let c = code.as_bytes();
                !c.is_empty() && c[0] == first && (second == 0 || (c.len() > 1 && c[1] == second))
            })
        };
        let scorer = |t: &str, w: &str, r: bool| collocation(t, w, r);
        let opts = options(o, model.as_ref().map(|m| m as &dyn LanguageModel), &scorer);
        let path = decode_best_path(&graph, &lookup, &accept, &opts, None);
        assert_path(&path, &row["path"], &ctx);

        // 词边缓存只影响速度：同一份缓存跑两次，结果都和不带缓存相同。
        let mut memo = WordEdgeMemo::new();
        let first = decode_best_path(&graph, &lookup, &accept, &opts, Some(&mut memo));
        let second = decode_best_path(&graph, &lookup, &accept, &opts, Some(&mut memo));
        assert_eq!(first, path, "带缓存 {ctx}");
        assert_eq!(second, path, "缓存命中 {ctx}");
        memo_checked += i32::from(!memo.is_empty());

        checked += 1;
        with_path += i32::from(path.is_some());
        complete += i32::from(path.as_ref().is_some_and(|p| p.complete));
        with_model += i32::from(o["lm"].as_bool().unwrap());
    }
    assert!(checked >= 1000 && with_path > 900 && complete > 300 && memo_checked > 800, "{checked}/{with_path}/{complete}/{memo_checked}");
    eprintln!("直接辅助码解码：{checked} 次（有解 {with_path}，整句完整 {complete}，带语言模型 {with_model}，词边缓存非空 {memo_checked}）的音节、整句、键、词、分数与 C++ 一致");
}

#[test]
fn resolver_equals_cpp() {
    let model = model();
    let keymap_table = keymap();
    let (mut checked, mut rewritten, mut sentences, mut aux_rewrites) = (0, 0, 0, 0);
    for row in rows("resolve") {
        let o = &row["options"];
        if o["lm"].as_bool().unwrap() && model.is_none() {
            continue;
        }
        let typed = row["typed"].as_str().unwrap();
        let profile = profile(row["profile"].as_str().unwrap());
        let ctx = format!("{} {typed:?}", row["profile"]);
        let spelling = SpellingOptions { slash_marker: row["slash"].as_bool().unwrap(), uppercase_marker: row["upper"].as_bool().unwrap() };

        let lookup = |span: &[String], _: bool| lexicon(span);
        let quanpins: Vec<String> = {
            let mut list = Vec::new();
            for a in b'a'..=b'z' {
                for b in (b'a'..=b'z').chain([b';']) {
                    let code = String::from_utf8(vec![a, b]).unwrap();
                    if msime_core::shuangpin::is_accepted_syllable_code(&code, profile) {
                        list.push(msime_core::shuangpin::to_pinyin(&code, profile));
                    }
                }
            }
            list
        };
        let single_rows = || -> Vec<(String, String)> {
            let mut rows = Vec::new();
            for q in &quanpins {
                for lexeme in lexicon(std::slice::from_ref(q)) {
                    if lexeme.value.chars().count() == 1 {
                        rows.push((q.clone(), lexeme.value));
                    }
                }
            }
            rows
        };
        let scorer = |t: &str, w: &str, r: bool| collocation(t, w, r);
        let context = ResolveContext {
            lookup: Some(&lookup),
            single_char_rows: if row["bulk"].as_bool().unwrap() { Some(&single_rows) } else { None },
            keymap: if row["no_keymap"].as_bool().unwrap() { None } else { Some(&keymap_table) },
            options: options(o, model.as_ref().map(|m| m as &dyn LanguageModel), &scorer),
        };
        let mut resolver = Resolver::new(profile);
        let result = resolver.resolve(typed, &spelling, &context);

        assert_eq!(result.rewrite.is_some(), row["rewritten"].as_bool().unwrap(), "是否改写 {ctx}");
        assert_path(&result.sentence, &row["sentence"], &format!("整句 {ctx}"));
        if let Some(rewrite) = &result.rewrite {
            let want = &row["request"];
            assert_eq!(rewrite.raw_input_with_syllable_helpcodes, want["raw_input_with_syllable_helpcodes"].as_str().unwrap(), "原串 {ctx}");
            assert_eq!(rewrite.raw_input, want["raw_input"].as_str().unwrap(), "去段串 {ctx}");
            assert_eq!(rewrite.raw_input, want["raw_input_with_cases"].as_str().unwrap(), "带大小写 {ctx}");
            let helpcodes: Vec<(u64, u64, u64)> = rewrite.syllable_helpcodes.iter().map(|h| (h.syllable as u64, u64::from(h.first), u64::from(h.second))).collect();
            let want_helpcodes: Vec<(u64, u64, u64)> =
                want["helpcodes"].as_array().unwrap().iter().map(|h| (h[0].as_u64().unwrap(), h[1].as_u64().unwrap(), h[2].as_u64().unwrap())).collect();
            assert_eq!(helpcodes, want_helpcodes, "约束 {ctx}");
            let source: Vec<u64> = rewrite.source_index.iter().map(|&i| i as u64).collect();
            let want_source: Vec<u64> = want["source_index"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap()).collect();
            assert_eq!(source, want_source, "源下标 {ctx}");
            let decorations: Vec<(u64, &str)> = rewrite.decorations.iter().map(|(i, t)| (*i as u64, t.as_str())).collect();
            let want_decorations: Vec<(u64, &str)> =
                want["decorations"].as_array().unwrap().iter().map(|d| (d[0].as_u64().unwrap(), d[1].as_str().unwrap())).collect();
            assert_eq!(decorations, want_decorations, "装饰 {ctx}");
            assert_eq!(rewrite.valid, want["valid"].as_bool().unwrap(), "有效 {ctx}");
            assert_eq!(rewrite.raw_segmentation, want["raw_segmentation"].as_str().unwrap(), "双拼切分 {ctx}");
            assert_eq!(rewrite.normalized_segmentation, want["normalized_segmentation"].as_str().unwrap(), "全拼切分 {ctx}");
            assert_eq!(rewrite.normalized_segmentation, want["segmentation"].as_str().unwrap(), "切分 {ctx}");
            assert_eq!(rewrite.normalized_input, want["normalized_input"].as_str().unwrap(), "归一化输入 {ctx}");
            aux_rewrites += i32::from(!rewrite.syllable_helpcodes.is_empty());
        }
        checked += 1;
        rewritten += i32::from(result.rewrite.is_some());
        sentences += i32::from(result.sentence.is_some());
    }
    assert!(checked >= 1000 && rewritten > 600 && aux_rewrites > 100 && sentences >= 10, "{checked}/{rewritten}/{sentences}/{aux_rewrites}");
    eprintln!("直接辅助码解析器：{checked} 次（改写请求 {rewritten}，其中带辅码约束 {aux_rewrites}，解出整句 {sentences}）的是否改写、请求字段、整句与 C++ 一致");
}
