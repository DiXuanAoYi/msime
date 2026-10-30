//! 神经重排对着真实的 C++ `neural::SentenceModel` 核对。基线 `expected_neural.jsonl` 由 `msime_neural_probe` 在随包
//! `sentence-model.safetensors` 上导出：
//!
//! - `load`：整份文件、各个截断长度、头里改一个字节的载入成败，成功时的窗口长度、署名、许可和若干字符的 token id；
//! - `next`：40 段文本的 `next_log_probabilities`（前 5 名与整个分布的和）；
//! - `score`：约 200 批 `score_sentences`（随机前文、空句、超长句、前文超过窗口、同一前文第二批命中缓存）；
//! - `rerank`：120 个 `rerank_order`（随机静态分、λ、路径数上限、前文，含长度不一致）；
//! - `last`：`last_characters`。
//!
//! 分数按容差比（C++ 的 `exp`/`log` 来自 MSVC，Rust 的来自各自的数学库，末位可能差 1 ulp，几十层累加后放大），
//! `rerank` 的次序和 token id 必须逐项相同。要 `installer/app_data/sentence-model.safetensors`（或 `MSIME_APP_DATA` 指向的目录），缺了就跳过。

use msime_core::neural::{last_characters, rerank_order, RerankOptions, SentenceModel};
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};

fn app_data() -> PathBuf {
    std::env::var_os("MSIME_APP_DATA").map(PathBuf::from).unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../installer/app_data"))
}

fn records() -> Vec<Value> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_neural.jsonl");
    fs::read_to_string(path).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn strings(value: &Value) -> Vec<String> {
    value.as_array().unwrap().iter().map(|s| s.as_str().unwrap().to_string()).collect()
}

fn floats(value: &Value) -> Vec<f64> {
    value.as_array().unwrap().iter().map(|s| s.as_f64().unwrap()).collect()
}

fn pool_chars() -> Vec<char> {
    // 和探针里的字符池同一份（常用字、标点、字母数字、词表外字符）。
    "的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强放决西被干做必战先回则任取据处队南给色光门即保治北造百规热领七海口东导器压志世金增争济阶油思术极交受联什认六共权收证改清己美再采转更单风切打白教速花带安场身车例真务具万每目至达走积示议声报斗完类八离华名确才科张信马节话米整空元况今集温传土许步群广石记需段研界拉林律叫且究观越织装影算低持音众书布复容儿须际商非验连断深难近矿千周委素技备半办青省列习响约支般史感劳便团往酸历市克何除消构府称太准精值号率族维划选标写存候毛亲快效斯院查江型眼王按格养易置派层片始却专状育厂京识适属圆包火住调满县局照参红细引听该铁价严，。！？、abcXYZ019 𠀀😀龘㐀".chars().collect()
}

#[test]
fn model_matches_cpp() {
    let path = app_data().join("sentence-model.safetensors");
    let Ok(bytes) = fs::read(&path) else {
        eprintln!("跳过：{} 不存在", path.display());
        return;
    };
    let model = SentenceModel::load(&bytes).expect("随包模型应能载入");
    let (mut scores, mut max_diff, mut orders, mut order_changes, mut nexts) = (0usize, 0.0f64, 0usize, 0usize, 0usize);
    let mut max_next_diff = 0.0f64;
    let diff = |got: f64, want: f64| (got - want).abs() / want.abs().max(1.0);
    for record in records() {
        match record["kind"].as_str().unwrap() {
            "load" => match record["what"].as_str().unwrap() {
                "full" => {
                    assert_eq!(model.context_length() as u64, record["context"].as_u64().unwrap());
                    assert_eq!(model.attribution(), record["attribution"].as_str().unwrap());
                    assert_eq!(model.license(), record["license"].as_str().unwrap());
                    let want: Vec<i64> = record["tokens"].as_array().unwrap().iter().map(|v| v.as_i64().unwrap()).collect();
                    let got: Vec<i64> = pool_chars().iter().map(|&c| model.token(c).map_or(-1, i64::from)).collect();
                    assert_eq!(got, want, "token id");
                }
                "cut" => {
                    let length = record["length"].as_u64().unwrap() as usize;
                    assert_eq!(SentenceModel::load(&bytes[..length]).is_some(), record["ok"].as_bool().unwrap(), "截断 {length}");
                }
                _ => {
                    let at = record["at"].as_u64().unwrap() as usize;
                    let mut copy = bytes.clone();
                    copy[at] ^= 0x20;
                    assert_eq!(SentenceModel::load(&copy).is_some(), record["ok"].as_bool().unwrap(), "改字节 {at}");
                }
            },
            "next" => {
                let text = record["text"].as_str().unwrap();
                let distribution = model.next_log_probabilities(text);
                assert_eq!(distribution.len() as u64, record["size"].as_u64().unwrap());
                let mut order: Vec<usize> = (0..distribution.len()).collect();
                order.sort_by(|&a, &b| distribution[b].partial_cmp(&distribution[a]).unwrap().then(a.cmp(&b)));
                for (k, want) in record["top"].as_array().unwrap().iter().enumerate() {
                    assert_eq!(order[k] as u64, want[0].as_u64().unwrap(), "前五名的 id {text:?} #{k}");
                    max_next_diff = max_next_diff.max(diff(f64::from(distribution[order[k]]), want[1].as_f64().unwrap()));
                }
                let total: f64 = distribution.iter().map(|&v| f64::from(v)).sum();
                max_next_diff = max_next_diff.max(diff(total, record["sum"].as_f64().unwrap()) / 1000.0);
                nexts += 1;
            }
            "score" => {
                let (context, texts) = (record["context"].as_str().unwrap(), strings(&record["texts"]));
                let got = model.score_sentences(context, &texts);
                let want = floats(&record["ret"]);
                assert_eq!(got.len(), want.len(), "条数 {context:?} {texts:?}");
                for (g, w) in got.iter().zip(&want) {
                    let d = diff(*g, *w);
                    max_diff = max_diff.max(d);
                    assert!(d < 2e-3, "分数 {g} 对 {w}，前文 {context:?} 句子 {texts:?}");
                }
                scores += want.len();
            }
            "rerank" => {
                let sentences = strings(&record["sentences"]);
                let statics = floats(&record["statics"]);
                let options = RerankOptions { lambda: record["lambda"].as_f64().unwrap(), max_paths: record["max_paths"].as_u64().unwrap() as usize, context_chars: record["context_chars"].as_u64().unwrap() as usize };
                let got = rerank_order(&model, record["context"].as_str().unwrap(), &sentences, &statics, &options);
                let want: Vec<usize> = record["ret"].as_array().unwrap().iter().map(|v| v.as_u64().unwrap() as usize).collect();
                assert_eq!(got, want, "重排 {sentences:?} {statics:?} {options:?}");
                orders += 1;
                order_changes += usize::from(!want.is_empty() && want.iter().enumerate().any(|(i, &v)| i != v));
            }
            "last" => {
                assert_eq!(last_characters(record["text"].as_str().unwrap(), record["count"].as_u64().unwrap() as usize), record["ret"].as_str().unwrap());
            }
            other => panic!("没见过的记录 {other}"),
        }
    }
    eprintln!("神经重排：next {nexts} 条（最大相对差 {max_next_diff:.2e}），score {scores} 个（最大相对差 {max_diff:.2e}），rerank {orders} 个，其中改变次序的 {order_changes}");
    assert!(scores > 500 && orders >= 100 && order_changes > 30);
}
