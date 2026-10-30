//! 输入方案对着真实的 C++ 方案类核对。基线 `expected_scheme.jsonl` 由 `msime_scheme_probe` 导出：
//! 全拼、双拼（九套方案 × 直接辅助码/大写触发开关）、五笔（扩展长度/混输/通配开关）、日语罗马字各吃随机按键序列
//! （字母大小写、撇号、反引号、斜杠、分号、减号、数字、退格、回车、退出）和偶尔的整串替换，每一步比较生成的
//! `QueryRequest` 和预编辑。

use msime_core::request::{QueryRequest, SchemeType};
use msime_core::scheme::{InputScheme, JapaneseRomajiScheme, QuanpinScheme, ShuangpinScheme, WubiScheme};
use msime_core::shuangpin::profile;
use serde_json::Value;
use std::path::PathBuf;

fn records() -> Vec<Value> {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden/expected_scheme.jsonl");
    std::fs::read_to_string(path).unwrap().lines().filter(|l| !l.trim().is_empty()).map(|l| serde_json::from_str(l).unwrap()).collect()
}

fn make(config: &str) -> Box<dyn InputScheme> {
    let parts: Vec<&str> = config.split(':').collect();
    match parts[0] {
        "quanpin" => Box::new(QuanpinScheme::default()),
        "shuangpin" => {
            let flags: u32 = parts[2].parse().unwrap();
            let mut scheme = ShuangpinScheme::new(profile(parts[1]));
            scheme.set_direct_helpcode(flags & 1 != 0);
            scheme.set_mid_sentence_uppercase_trigger(flags & 2 != 0);
            Box::new(scheme)
        }
        "wubi" => {
            let flags: u32 = parts[1].parse().unwrap();
            let mut scheme = WubiScheme::default();
            scheme.set_extended_length_allowed(flags & 1 != 0);
            scheme.set_mixed_pinyin_allowed(flags & 2 != 0);
            scheme.set_z_wildcard(flags & 4 != 0);
            Box::new(scheme)
        }
        "japanese" => Box::new(JapaneseRomajiScheme::default()),
        other => panic!("不认识的方案 {other}"),
    }
}

fn scheme_number(scheme: SchemeType) -> u64 {
    match scheme {
        SchemeType::Quanpin => 0,
        SchemeType::Shuangpin => 1,
        SchemeType::Wubi => 2,
        SchemeType::JapaneseRomaji => 3,
    }
}

fn check(request: &QueryRequest, preedit: &str, want: &Value, label: &str) {
    assert_eq!(scheme_number(request.scheme), want["scheme"].as_u64().unwrap(), "方案 {label}");
    for (field, got) in [
        ("raw", &request.raw_input),
        ("cases", &request.raw_input_with_cases),
        ("norm", &request.normalized_input),
        ("rawseg", &request.raw_segmentation),
        ("normseg", &request.normalized_segmentation),
        ("seg", &request.segmentation),
        ("orig", &request.raw_input_with_syllable_helpcodes),
    ] {
        assert_eq!(got, want[field].as_str().unwrap(), "{field} {label}");
    }
    assert_eq!(preedit, want["preedit"].as_str().unwrap(), "预编辑 {label}");
    assert_eq!(request.valid, want["valid"].as_u64().unwrap() == 1, "valid {label}");
    assert_eq!(request.wubi_z_wildcard, want["wild"].as_u64().unwrap() == 1, "通配 {label}");
    assert_eq!(request.has_syllable_helpcode_layout, want["layout"].as_u64().unwrap() == 1, "layout {label}");

    let helpcodes: Vec<Value> = request.syllable_helpcodes.iter().map(|h| serde_json::json!([h.syllable, h.first as u32, h.second as u32])).collect();
    assert_eq!(Value::Array(helpcodes), want["hc"], "句中辅助码 {label}");
    let index: Vec<Value> = request.syllable_helpcode_source_index.iter().map(|i| serde_json::json!(i)).collect();
    assert_eq!(Value::Array(index), want["index"], "下标映射 {label}");
    let decorations: Vec<Value> = request.syllable_helpcode_decorations.iter().map(|(at, text)| serde_json::json!([at, text])).collect();
    assert_eq!(Value::Array(decorations), want["deco"], "装饰 {label}");
    let strokes: Vec<Value> = request.key_strokes.iter().map(|s| serde_json::json!([s.vk, s.modifiers_down, s.wch])).collect();
    assert_eq!(Value::Array(strokes), want["strokes"], "按键 {label}");
}

#[test]
fn schemes_match_cpp() {
    let (mut steps, mut sequences, mut valid, mut mid, mut wild, mut helpcode, mut sets) = (0, 0, 0, 0, 0, 0, 0);
    for record in records() {
        let config = record["config"].as_str().unwrap();
        let mut scheme = make(config);
        // 探针每个序列开头先 reset；五笔的 reset 会清掉扩展长度，所以这里同样先 reset。
        scheme.reset();
        sequences += 1;
        let mut trail = Vec::new();
        for (index, step) in record["steps"].as_array().unwrap().iter().enumerate() {
            if let Some(set) = step.get("set") {
                let (raw, cases) = (set[0].as_str().unwrap(), set[1].as_str().unwrap());
                scheme.set_raw_input(raw, cases);
                trail.push(format!("set({raw:?},{cases:?})"));
                sets += 1;
            } else {
                let key = step["key"].as_array().unwrap();
                let (vk, wch, modifiers) = (key[0].as_u64().unwrap() as u32, key[1].as_u64().unwrap() as u16, key[2].as_u64().unwrap() as u32);
                scheme.handle_key(vk, modifiers, wch);
                trail.push(format!("key({vk:#x},{wch},{modifiers})"));
            }
            let label = format!("{config} 第 {index} 步 {}", trail.join(" "));
            let request = scheme.build_request();
            check(&request, &scheme.preedit(), &step["state"], &label);
            steps += 1;
            valid += usize::from(request.valid);
            mid += usize::from(request.has_syllable_helpcode_layout);
            wild += usize::from(request.wubi_z_wildcard);
            helpcode += usize::from(request.raw_input_with_cases.bytes().rev().take(2).all(|b| b.is_ascii_uppercase()) && request.raw_input_with_cases.len() > 2);
        }
    }
    eprintln!("输入方案：{sequences} 个序列 {steps} 步（有效请求 {valid}，整串替换 {sets}，句中辅助码 {mid}，五笔通配 {wild}，大写结尾 {helpcode}）");
    assert!(steps > 15000 && valid > 8000 && mid > 100 && wild > 50 && sets > 500);
}
