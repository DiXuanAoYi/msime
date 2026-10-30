//! Rust 里抄的音节表必须和 C++ 那份一模一样；C++ 改了这里就红。

use msime_core::syllables::INTACT;
use std::path::PathBuf;

#[test]
fn syllable_list_matches_the_cpp_engine() {
    let source = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../engine/quanpin/quanpin_utils.cpp");
    let text = std::fs::read_to_string(&source).expect("读不到 engine/quanpin/quanpin_utils.cpp");
    let start = text.find("kList = {").expect("找不到 intact_pinyin_list") + "kList = {".len();
    let end = start + text[start..].find("};").unwrap();
    let cpp: Vec<&str> = text[start..end]
        .split('"')
        .enumerate()
        .filter_map(|(index, piece)| (index % 2 == 1).then_some(piece))
        .collect();
    assert_eq!(cpp, INTACT);
}
