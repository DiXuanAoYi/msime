//! kenlm 读取器对着真实的 C++ kenlm 核对：用同一份出货的 `sc.lm`（摘要见 `language-model/lock.json`），
//! 比词下标和逐词分数。基线 `engine/contracts/golden/expected_lm.jsonl` 由 `msime_lm_probe` 导出，
//! 词表是 `engine/contracts/golden/lm_words.txt`。
//!
//! 模型默认读 `installer/app_data/sc.lm`，也可以用环境变量 `MSIME_LM` 指定；两者都没有就跳过。

use msime_core::kenlm::KenLm;
use serde_json::Value;
use std::path::PathBuf;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn open() -> Option<KenLm> {
    let path = std::env::var_os("MSIME_LM").map(PathBuf::from).unwrap_or_else(|| root().join("installer/app_data/sc.lm"));
    if !path.exists() {
        eprintln!("跳过：没有 {}", path.display());
        return None;
    }
    Some(KenLm::open(&path).unwrap_or_else(|e| panic!("载入失败：{e}")))
}

fn rows(kind: &str) -> Vec<Value> {
    let text = std::fs::read_to_string(root().join("engine/contracts/golden/expected_lm.jsonl")).unwrap();
    text.lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str::<Value>(l).unwrap())
        .filter(|row| row["kind"] == kind)
        .collect()
}

#[test]
fn the_model_loads_with_the_shipped_shape() {
    let Some(model) = open() else { return };
    assert_eq!(model.order(), 3);
    let expected = rows("info")[0]["unknown_penalty"].as_f64().unwrap() as f32;
    assert_eq!(model.unknown_penalty(), expected, "未登录词罚分");
}

#[test]
fn every_word_index_and_single_word_score_equals_cpp() {
    let Some(model) = open() else { return };
    let (mut checked, mut unknown) = (0, 0);
    for row in rows("words")[0]["rows"].as_array().unwrap() {
        let word = row[0].as_str().unwrap();
        let index = model.index(word);
        assert_eq!(u64::from(index), row[1].as_u64().unwrap(), "{word} 的下标");
        let (score, _) = model.score_word(&model.null_state(), word);
        let want = row[2].as_f64().unwrap() as f32;
        assert_eq!(score.to_bits(), want.to_bits(), "{word} 的单词分数 {score} ≠ {want}");
        checked += 1;
        unknown += usize::from(index == 0);
    }
    assert!(checked > 3000 && unknown > 100 && unknown < checked / 2, "{checked} 个词，其中未登录 {unknown} 个");
    eprintln!("kenlm 词表：{checked} 个词的下标和单词分数与 C++ 逐位相同（其中未登录词 {unknown} 个）");
}

#[test]
fn sentence_scores_with_backoff_equal_cpp_bit_for_bit() {
    let Some(model) = open() else { return };
    let (mut sentences, mut steps, mut contextual) = (0, 0, 0);
    for row in rows("sentence") {
        let words: Vec<&str> = row["words"].as_array().unwrap().iter().map(|w| w.as_str().unwrap()).collect();
        for (start, key) in [(model.null_state(), "null_scores"), (model.begin_state(), "begin_scores")] {
            let wanted = row[key].as_array().unwrap();
            let mut state = start;
            for (i, word) in words.iter().enumerate() {
                let (score, next) = model.score_word(&state, word);
                state = next;
                let want = wanted[i].as_f64().unwrap() as f32;
                assert_eq!(score.to_bits(), want.to_bits(), "{words:?} 从 {key} 起第 {i} 个词「{word}」：{score} ≠ {want}");
                steps += 1;
                // 分数和孤立词的一元分数不同，说明用上了上下文（二元、三元或回退）。
                contextual += usize::from(score.to_bits() != model.score_word(&model.null_state(), word).0.to_bits());
            }
        }
        sentences += 1;
    }
    assert!(sentences >= 700 && steps > 5000, "{sentences} 句、{steps} 步");
    assert!(contextual > 300, "只有 {contextual} 步用到了上下文，高阶和回退路径没被覆盖");
    eprintln!("kenlm 句子：{sentences} 句、{steps} 步（其中用到上下文的 {contextual} 步，含回退）的分数与 C++ 逐位相同");
}

#[test]
fn the_lattice_can_use_the_real_model_through_the_trait() {
    use msime_core::lattice::{decode_word_lattice, Lexeme, Options};
    let Some(model) = open() else { return };
    // 两个常用词各有一个高频但不通顺的竞争者；真实模型应该把通顺的“你好”推到最前。
    let lookup = |span: &[String]| -> Vec<Lexeme> {
        let row = |value: &str, weight: i64| Lexeme { value: value.into(), weight, ..Default::default() };
        match span.join("'").as_str() {
            "ni" => vec![row("你", 1000), row("泥", 90_000)],
            "hao" => vec![row("好", 1000), row("号", 90_000)],
            _ => vec![],
        }
    };
    let options = Options { language_model: Some(&model), ..Default::default() };
    let paths = decode_word_lattice(&["ni".to_string(), "hao".to_string()], &lookup, &options);
    assert_eq!(paths[0].sentence, "你好", "{:?}", paths.iter().map(|p| (&p.sentence, p.log_prob)).collect::<Vec<_>>());
}
