//! 日语罗马字 ↔ 假名转换，对应 `engine/japanese/romaji_converter.cpp`。
//!
//! 映射表由脚本从 C++ 源码抽出（`romaji_table.rs`），转换规则逐行照 C++ 写。

use crate::romaji_table::ROMAJI_TABLE;

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Conversion {
    pub hiragana: String,
    pub pending: String,
    pub complete: bool,
}

fn is_consonant(ch: u8) -> bool {
    ch.is_ascii_lowercase() && !matches!(ch, b'a' | b'i' | b'u' | b'e' | b'o')
}

fn lookup(key: &[u8]) -> Option<&'static str> {
    ROMAJI_TABLE.iter().find(|(romaji, _)| romaji.as_bytes() == key).map(|(_, kana)| *kana)
}

/// 罗马字 → 平假名；没能转换的尾巴留在 `pending`（`k`、`ky` 这类还没打完的写法）。
pub fn convert(input: &str) -> Conversion {
    let normalized: Vec<u8> = input.bytes().map(|b| b.to_ascii_lowercase()).collect();
    let mut result = Conversion::default();
    let mut index = 0;
    while index < normalized.len() {
        if normalized[index] == b'n' {
            if index + 1 == normalized.len() {
                result.hiragana.push('ん');
                index += 1;
                continue;
            }
            let next = normalized[index + 1];
            // 显式的 nn 在元音前收成 ん（rennai → れんあい）；问候语 konnichiha/konnichiwa 的第二个 n 属于 ni，单独放行。
            let conventional_konni = next == b'n' && index == 2 && normalized.starts_with(b"konni");
            if next == b'\'' || (next == b'n' && !conventional_konni) {
                result.hiragana.push('ん');
                index += 2;
                continue;
            }
            // 长音符 '-' 不是元音，和辅音一样让前面的 n 收成 ん。
            if conventional_konni || next == b'-' || (is_consonant(next) && next != b'y') {
                result.hiragana.push('ん');
                index += 1;
                continue;
            }
        }

        if index + 1 < normalized.len() && normalized[index] == normalized[index + 1] && is_consonant(normalized[index]) && normalized[index] != b'n' {
            result.hiragana.push('っ');
            index += 1;
            continue;
        }

        let remaining = normalized.len() - index;
        let mut matched = false;
        for length in (1..=remaining.min(4)).rev() {
            if let Some(kana) = lookup(&normalized[index..index + length]) {
                result.hiragana.push_str(kana);
                index += length;
                matched = true;
                break;
            }
        }
        if !matched {
            result.pending = String::from_utf8_lossy(&normalized[index..]).into_owned();
            break;
        }
    }
    result.complete = !result.hiragana.is_empty() && result.pending.is_empty();
    result
}

pub fn hiragana_to_katakana(hiragana: &str) -> String {
    hiragana
        .chars()
        .map(|c| if ('ぁ'..='ゖ').contains(&c) { char::from_u32(c as u32 + 0x60).unwrap_or(c) } else { c })
        .collect()
}

/// 刚好一个小写/大写平假名。
pub fn is_single_kana(conversion: &Conversion) -> bool {
    if !conversion.complete || conversion.hiragana.is_empty() {
        return false;
    }
    let mut chars = conversion.hiragana.chars();
    matches!((chars.next(), chars.next()), (Some(c), None) if ('ぁ'..='ゖ').contains(&c))
}

/// 同一个假名有几种等长写法时 C++ 取哪个，取决于 `unordered_map` 的遍历次序和 `std::sort` 的平局处理
/// （实现定义）。这里固定成 MSVC 构建下探针量到的结果，`tests/romaji_wubi_golden.rs` 逐项核对；
/// 换一个标准库实现，C++ 这一侧可能挑到别的写法。
const TIE_CHOICE: &[(&str, &str)] = &[
    ("じ", "zi"),
    ("ふ", "fu"),
    ("しゃ", "sha"),
    ("しゅ", "shu"),
    ("しょ", "sho"),
    ("ちゃ", "cya"),
    ("ちゅ", "cyu"),
    ("ちょ", "cho"),
    ("ぁ", "xa"),
    ("ぃ", "xi"),
    ("ぅ", "xu"),
    ("ぇ", "le"),
    ("ぉ", "xo"),
    ("ゃ", "lya"),
    ("ゅ", "xyu"),
    ("ょ", "xyo"),
    ("っ", "xtsu"),
];

/// 假名 → 罗马字的反查表：每个假名取最长的写法，等长的按 `TIE_CHOICE`；
/// 按假名字节数从长到短排，匹配时先试长的。
fn kana_to_romaji_table() -> Vec<(&'static str, &'static str)> {
    let mut best: Vec<(&'static str, &'static str)> = Vec::new();
    for (romaji, kana) in ROMAJI_TABLE {
        match best.iter_mut().find(|(seen, _)| seen == kana) {
            None => best.push((kana, romaji)),
            Some(entry) => {
                let preferred = TIE_CHOICE.iter().find(|(k, _)| k == kana).map(|(_, r)| *r);
                let better = romaji.len() > entry.1.len() || (romaji.len() == entry.1.len() && preferred == Some(*romaji));
                if better {
                    entry.1 = romaji;
                }
            }
        }
    }
    best.sort_by(|a, b| b.0.len().cmp(&a.0.len()));
    best
}

fn char_len(lead: u8) -> usize {
    if lead < 0x80 {
        1
    } else if lead >> 5 == 0x6 {
        2
    } else if lead >> 4 == 0xE {
        3
    } else {
        4
    }
}

/// 假名 → 罗马字（片假名先折成平假名；认不出的字符跳过）。
pub fn hiragana_to_romaji(kana: &str) -> String {
    let hiragana: String = kana
        .chars()
        .map(|c| if ('ァ'..='ヶ').contains(&c) { char::from_u32(c as u32 - 0x60).unwrap_or(c) } else { c })
        .collect();
    let bytes = hiragana.as_bytes();
    let table = kana_to_romaji_table();
    let find = |index: usize| table.iter().find(|(k, _)| bytes[index..].starts_with(k.as_bytes())).copied();
    let mut romaji = String::new();
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index..].starts_with("っ".as_bytes()) {
            index += 3;
            match find(index.min(bytes.len())) {
                Some((_, next)) if is_consonant(next.as_bytes()[0]) => romaji.push(next.as_bytes()[0] as char),
                _ => romaji.push_str("xtsu"),
            }
            continue;
        }
        if bytes[index..].starts_with("ん".as_bytes()) {
            romaji.push('n');
            index += 3;
            continue;
        }
        match find(index) {
            Some((kana, found)) => {
                romaji.push_str(found);
                index += kana.len();
            }
            None => index += char_len(bytes[index]),
        }
    }
    romaji
}

/// `k`、`ky` 这类罗马字前缀对应的全部假名（排好序、去重）。
pub fn kana_for_prefix(pending: &str) -> Vec<String> {
    if pending.is_empty() {
        return Vec::new();
    }
    let prefix = pending.to_ascii_lowercase();
    let mut kana: Vec<String> = ROMAJI_TABLE
        .iter()
        .filter(|(romaji, _)| romaji.len() >= prefix.len() && romaji.starts_with(&prefix))
        .map(|(_, k)| (*k).to_string())
        .collect();
    kana.sort();
    kana.dedup();
    kana
}
