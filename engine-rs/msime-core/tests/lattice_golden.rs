//! 词格对着真实的 C++ `decode_word_lattice` / `merge_lattice_candidates` 核对。
//!
//! 基线 `engine/contracts/golden/expected_lattice.jsonl` 由 `msime_lattice_probe` 生成：每个场景带着解码过程中
//! 实际被查询到的词表，所以这里用同一份词表回放，不需要复现随机数。基线里没有语言模型（C++ 的模型不能被替换），
//! 核对的是启发式打分、惩罚分、字级搭配回调、字约束、束宽与 n-best 去重，以及合并进候选列表的全部来源规则。

use msime_core::kenlm::KenLm;
use msime_core::lattice::{
    decode_word_lattice, merge_lattice_candidates, CharConstraint, Item, LanguageModel, Lexeme, Options, Path, Source, SourcedReranker,
};
use serde_json::Value;
use std::collections::HashMap;
use std::path::PathBuf;

fn scenarios(kind: &str) -> Vec<Value> {
    let file = if kind == "decode_lm" { "expected_lattice_lm.jsonl" } else { "expected_lattice.jsonl" };
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden").join(file);
    std::fs::read_to_string(path)
        .unwrap()
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["kind"] == kind)
        .collect()
}

fn strings(value: &Value) -> Vec<String> {
    value.as_array().unwrap().iter().map(|v| v.as_str().unwrap().to_string()).collect()
}

fn lexicon(value: &Value) -> HashMap<String, Vec<Lexeme>> {
    value
        .as_object()
        .unwrap()
        .iter()
        .map(|(key, rows)| {
            let rows = rows
                .as_array()
                .unwrap()
                .iter()
                .map(|r| Lexeme {
                    key: r["key"].as_str().unwrap().to_string(),
                    value: r["value"].as_str().unwrap().to_string(),
                    weight: r["weight"].as_i64().unwrap(),
                    penalty: r["penalty"].as_f64().unwrap(),
                    input_domains: strings(&r["domains"]),
                })
                .collect();
            (key.clone(), rows)
        })
        .collect()
}

/// 与探针里 `collocation()` 逐字相同的字级搭配回调。
fn collocation(tail: &str, word: &str, rear: bool) -> f64 {
    let last = tail.chars().last().map(|c| c as u32).unwrap_or(0);
    let first = word.chars().next().map(|c| c as u32).unwrap_or(0);
    let value = last * 31 + first * 17 + tail.chars().count() as u32 * 3 + if rear { 5 } else { 0 };
    ((value % 13) as i32 - 6) as f64 * 0.25
}

struct Settings {
    beam: i32,
    nbest: i32,
    span_limit: i32,
    max_phrase_syllables: i32,
    unigram_z: f64,
    phrase_length_bonus: f64,
    collocation: bool,
    collocation_weight: f64,
    include_lattice_best: bool,
    include_collocation_best: bool,
    show_next_on_duplicate: bool,
    dictionary_tiebreak: f64,
    reading_prior: f64,
    reading_prior_share: f64,
    reading_prior_floor: f64,
}

fn settings(o: &Value) -> Settings {
    Settings {
        beam: o["beam"].as_i64().unwrap() as i32,
        nbest: o["nbest"].as_i64().unwrap() as i32,
        span_limit: o["span_limit"].as_i64().unwrap() as i32,
        max_phrase_syllables: o["max_phrase_syllables"].as_i64().unwrap() as i32,
        unigram_z: o["unigram_z"].as_f64().unwrap(),
        phrase_length_bonus: o["phrase_length_bonus"].as_f64().unwrap(),
        collocation: o["collocation"].as_bool().unwrap(),
        collocation_weight: o["collocation_weight"].as_f64().unwrap(),
        include_lattice_best: o["include_lattice_best"].as_bool().unwrap(),
        include_collocation_best: o["include_collocation_best"].as_bool().unwrap(),
        show_next_on_duplicate: o["show_next_on_duplicate"].as_bool().unwrap(),
        dictionary_tiebreak: o["dictionary_tiebreak"].as_f64().unwrap(),
        reading_prior: o["reading_prior"].as_f64().unwrap(),
        reading_prior_share: o["reading_prior_share"].as_f64().unwrap(),
        reading_prior_floor: o["reading_prior_floor"].as_f64().unwrap(),
    }
}

fn apply<'a>(options: &mut Options<'a>, s: &Settings, scorer: &'a dyn Fn(&str, &str, bool) -> f64) {
    options.beam = s.beam;
    options.nbest = s.nbest;
    options.span_limit = s.span_limit;
    options.max_phrase_syllables = s.max_phrase_syllables;
    options.unigram_z = s.unigram_z;
    options.phrase_length_bonus = s.phrase_length_bonus;
    options.include_lattice_best = s.include_lattice_best;
    options.include_collocation_best = s.include_collocation_best;
    options.show_next_on_duplicate = s.show_next_on_duplicate;
    options.dictionary_tiebreak = s.dictionary_tiebreak;
    options.reading_prior = s.reading_prior;
    options.reading_prior_share = s.reading_prior_share;
    options.reading_prior_floor = s.reading_prior_floor;
    if s.collocation {
        options.collocation_scorer = Some(scorer);
        options.collocation_weight = s.collocation_weight;
    }
}

fn first_codepoint(text: &str) -> u32 {
    text.chars().next().map(|c| c as u32).unwrap_or(0)
}

fn close(a: f64, b: f64) -> bool {
    (a - b).abs() <= 1e-9 * a.abs().max(b.abs()).max(1.0)
}

fn lookup_in(map: &HashMap<String, Vec<Lexeme>>) -> impl Fn(&[String]) -> Vec<Lexeme> + '_ {
    move |span| map.get(&span.join("'")).cloned().unwrap_or_default()
}

/// 回放一个解码场景并和基线比；返回 (路径数, 是否带字约束, 是否带搭配回调)。
fn check_decode(number: usize, row: &Value, model: Option<&dyn LanguageModel>) -> (usize, bool, bool) {
    let syllables = strings(&row["syllables"]);
    let s = settings(&row["options"]);
    let main = lexicon(&row["lexicon"]);
    let constrained_map = lexicon(&row["constrained"]);
    let lookup = lookup_in(&main);
    let constrained_lookup = lookup_in(&constrained_map);

    // 每个约束一个闭包：首个码点 % modulus != reject。
    let rules: Vec<(usize, u32, u32)> = row["constraints"]
        .as_array()
        .unwrap()
        .iter()
        .map(|c| (c["syllable"].as_u64().unwrap() as usize, c["modulus"].as_u64().unwrap() as u32, c["reject"].as_u64().unwrap() as u32))
        .collect();
    let accepts: Vec<Box<dyn Fn(&str) -> bool>> =
        rules.iter().map(|&(_, modulus, reject)| Box::new(move |h: &str| first_codepoint(h) % modulus != reject) as Box<dyn Fn(&str) -> bool>).collect();

    let mut options = Options::default();
    apply(&mut options, &s, &collocation);
    options.language_model = model;
    for (index, &(syllable, _, _)) in rules.iter().enumerate() {
        options.char_constraints.push(CharConstraint { syllable, accept: accepts[index].as_ref() });
    }
    if row["separate_constrained_lookup"].as_bool().unwrap() {
        options.constrained_lookup = Some(&constrained_lookup);
    }

    let paths = decode_word_lattice(&syllables, &lookup, &options);
    let expected = row["paths"].as_array().unwrap();
    let label = format!("解码场景 {number}（{} 个音节，beam {}，nbest {}）", syllables.len(), s.beam, s.nbest);
    assert_eq!(paths.len(), expected.len(), "{label}：路径条数 {:?}", paths.iter().map(|p| &p.sentence).collect::<Vec<_>>());
    for (index, (got, want)) in paths.iter().zip(expected).enumerate() {
        assert_eq!(got.sentence, want["sentence"].as_str().unwrap(), "{label}：第 {index} 条句子；Rust 全部：{:?}", paths.iter().map(|p| (&p.sentence, p.log_prob)).collect::<Vec<_>>());
        assert_eq!(got.key, want["key"].as_str().unwrap(), "{label}：第 {index} 条键");
        assert_eq!(got.words, strings(&want["words"]), "{label}：第 {index} 条词序");
        assert_eq!(got.input_domains, strings(&want["domains"]), "{label}：第 {index} 条输入域");
        assert!(close(got.log_prob, want["log_prob"].as_f64().unwrap()), "{label}：第 {index} 条分数 {} ≠ {}", got.log_prob, want["log_prob"]);
    }
    (paths.len(), !rules.is_empty(), s.collocation && s.collocation_weight > 0.0)
}

#[test]
fn decoded_n_best_equals_the_cpp_decoder() {
    let (mut checked, mut with_paths, mut path_count, mut constrained, mut collocated) = (0, 0, 0, 0, 0);
    for (number, row) in scenarios("decode").into_iter().enumerate() {
        let (paths, has_constraint, has_collocation) = check_decode(number, &row, None);
        checked += 1;
        with_paths += usize::from(paths > 0);
        path_count += paths;
        constrained += usize::from(has_constraint);
        collocated += usize::from(has_collocation);
    }
    assert!(checked >= 150 && with_paths >= 140, "{checked} 个场景、{with_paths} 个有解");
    eprintln!("词格解码（无模型）：{checked} 个场景（有解 {with_paths}，共 {path_count} 条路径，带字约束 {constrained}，带搭配回调 {collocated}），句子、键、词序、输入域、分数与 C++ 一致");
}

/// 带真实 kenlm 模型的解码：Rust 的 `KenLm` 与 C++ 的 `ngram::LanguageModel` 打同样的分，
/// 词库平局项和读音先验也在这条路径上，所以这里一并核对。需要 `installer/app_data/sc.lm`（或 `MSIME_LM`）。
#[test]
fn decoded_n_best_with_the_real_language_model_equals_the_cpp_decoder() {
    let path = std::env::var_os("MSIME_LM").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../installer/app_data/sc.lm"));
    if !path.exists() {
        eprintln!("跳过：没有 {}", path.display());
        return;
    }
    let model = KenLm::open(&path).unwrap_or_else(|e| panic!("载入失败：{e}"));
    let (mut checked, mut with_paths, mut path_count) = (0, 0, 0);
    for (number, row) in scenarios("decode_lm").into_iter().enumerate() {
        assert!(row["options"]["language_model"].as_bool().unwrap());
        let (paths, _, _) = check_decode(number, &row, Some(&model));
        checked += 1;
        with_paths += usize::from(paths > 0);
        path_count += paths;
    }
    assert!(checked >= 80 && with_paths >= 75, "{checked} 个场景、{with_paths} 个有解");
    eprintln!("词格解码（真实 kenlm）：{checked} 个场景（有解 {with_paths}，共 {path_count} 条路径），句子、键、词序、分数与 C++ 一致");
}

fn source_of(name: &str) -> Source {
    match name {
        "Database" => Source::Database,
        "UserDatabase" => Source::UserDatabase,
        "Fallback" => Source::Fallback,
        "Generated" => Source::Generated,
        "Collocation" => Source::Collocation,
        "NeuralDesktop" => Source::NeuralDesktop,
        "NeuralKeyboard" => Source::NeuralKeyboard,
        "QuickPhrase" => Source::QuickPhrase,
        "Emoji" => Source::Emoji,
        other => panic!("基线里有没映射的来源 {other}"),
    }
}

fn items(value: &Value) -> Vec<Item> {
    value
        .as_array()
        .unwrap()
        .iter()
        .map(|i| Item {
            pinyin: i["pinyin"].as_str().unwrap().to_string(),
            canonical_pinyin: i["canonical"].as_str().unwrap().to_string(),
            word: i["word"].as_str().unwrap().to_string(),
            weight: i["weight"].as_i64().unwrap(),
            source: source_of(i["source"].as_str().unwrap()),
            sentence_association: i["sentence_association"].as_bool().unwrap(),
            input_domains: strings(&i["domains"]),
        })
        .collect()
}

fn run_reranker(mode: i64, paths: &mut Vec<Path>) -> bool {
    match mode {
        0 => false,
        1 => {
            paths.reverse();
            true
        }
        2 => {
            if paths.len() > 1 {
                paths.rotate_left(1);
            }
            true
        }
        _ => true,
    }
}

#[test]
fn merged_candidate_lists_equal_the_cpp_merge() {
    let (mut checked, mut changed, mut with_rerankers) = (0, 0, 0);
    for (number, row) in scenarios("merge").into_iter().enumerate() {
        let syllables = strings(&row["syllables"]);
        let typed = row["typed"].as_str().unwrap();
        let s = settings(&row["options"]);
        let main = lexicon(&row["lexicon"]);
        let lookup = lookup_in(&main);
        let mut options = Options::default();
        apply(&mut options, &s, &collocation);

        let specs: Vec<(Source, i64)> = row["rerankers"]
            .as_array()
            .unwrap()
            .iter()
            .map(|r| (source_of(r["source"].as_str().unwrap()), r["mode"].as_i64().unwrap()))
            .collect();
        let functions: Vec<Box<dyn Fn(&mut Vec<Path>) -> bool>> =
            specs.iter().map(|&(_, mode)| Box::new(move |paths: &mut Vec<Path>| run_reranker(mode, paths)) as Box<dyn Fn(&mut Vec<Path>) -> bool>).collect();
        let rerankers: Vec<SourcedReranker> =
            specs.iter().zip(&functions).map(|(&(source, _), f)| SourcedReranker { rerank: f.as_ref(), source }).collect();

        let before = items(&row["candidates"]);
        let mut result = before.clone();
        merge_lattice_candidates(&mut result, &syllables, &lookup, typed, &options, &rerankers);

        let expected = items(&row["result"]);
        let label = format!("合并场景 {number}（{} 个音节，{} 个重排器）", syllables.len(), specs.len());
        assert_eq!(result.len(), expected.len(), "{label}：候选条数\n得到 {:?}\n期望 {:?}", result.iter().map(|i| (&i.word, i.source)).collect::<Vec<_>>(), expected.iter().map(|i| (&i.word, i.source)).collect::<Vec<_>>());
        for (index, (got, want)) in result.iter().zip(&expected).enumerate() {
            assert_eq!(got.word, want.word, "{label}：第 {index} 项词");
            assert_eq!(got.source, want.source, "{label}：第 {index} 项来源");
            assert_eq!(got.pinyin, want.pinyin, "{label}：第 {index} 项输入码");
            assert_eq!(got.canonical_pinyin, want.canonical_pinyin, "{label}：第 {index} 项规范读音");
            assert_eq!(got.sentence_association, want.sentence_association, "{label}：第 {index} 项整句标记");
            assert_eq!(got.input_domains, want.input_domains, "{label}：第 {index} 项输入域");
            assert!((got.weight - want.weight).abs() <= 1, "{label}：第 {index} 项权重 {} ≠ {}", got.weight, want.weight);
        }
        checked += 1;
        changed += usize::from(result.len() != before.len());
        with_rerankers += usize::from(!specs.is_empty());
    }
    assert!(checked >= 150 && changed >= 80, "{checked} 个场景、改动了候选的 {changed} 个");
    eprintln!("词格合并：{checked} 个场景（候选列表有变化 {changed}，带重排器 {with_rerankers}），来源、次序、整句标记、权重与 C++ 一致");
}
