//! 辅助码：汉字 → 两个字母的码表、给词算码、按用户敲的码筛选和重排候选。
//!
//! 移植自 `engine/common/helpcode_utils.cpp`。码表是 `engine/helpcode/helpcodes/*.txt`，每行 `字=码`，
//! 码取前两个小写字母；单字取两个码，多字词取首字和末字各自的第一个码。
//! 用户在全拼里敲大写字母表示辅助码：末尾一个大写是单码（重排），末尾两个大写是双码（筛选）。
//! 双拼的“全码辅助”判断在 [`crate::shuangpin`]。

use std::collections::HashMap;

pub type Keymap = HashMap<String, String>;

/// 内置方案和它的码表文件（相对 `helpcodes/` 的上一级，即 `engine/helpcode`）。
pub const BUILTIN_SCHEMAS: [(&str, &str); 6] = [
    ("lantian", "helpcodes/helpcode.txt"),
    ("ziranma", "helpcodes/zrm_helpcode_big_unique.txt"),
    ("shouyou2_0", "helpcodes/shouyou2_0_helpcode.txt"),
    ("shouyouplus", "helpcodes/shouyouplus_helpcode.txt"),
    ("xiaohe", "helpcodes/xiaohe_helpcode.txt"),
    ("jiajia", "helpcodes/jiajia_helpcode.txt"),
];

/// 解析码表文本。规则同 C++：去掉开头的 BOM 和行尾的 `\r`，`#` 开头是注释，
/// 没有 `=` 或 `=` 在行首的行跳过，`=` 后面取两个字节、必须全是小写字母；同一个字重复出现以后者为准。
pub fn parse_keymap(text: &str) -> Keymap {
    let mut keymap = Keymap::new();
    for (index, raw) in text.split('\n').enumerate() {
        let mut line = raw;
        if index == 0 {
            line = line.strip_prefix('\u{feff}').unwrap_or(line);
        }
        line = line.strip_suffix('\r').unwrap_or(line);
        if line.starts_with('#') {
            continue;
        }
        let Some(position) = line.find('=') else { continue };
        if position == 0 {
            continue;
        }
        let tail = &line.as_bytes()[position + 1..];
        let code = &tail[..tail.len().min(2)];
        if code.is_empty() || !code.iter().all(|b| b.is_ascii_lowercase()) {
            continue;
        }
        keymap.insert(line[..position].to_string(), String::from_utf8_lossy(code).into_owned());
    }
    keymap
}

fn is_han(c: char) -> bool {
    let cp = c as u32;
    cp == 0x3007
        || (0x3400..=0x4DBF).contains(&cp)
        || (0x4E00..=0x9FFF).contains(&cp)
        || (0xF900..=0xFAFF).contains(&cp)
        || (0x20000..=0x2FA1F).contains(&cp)
        || (0x30000..=0x323AF).contains(&cp)
}

pub fn first_han_char(words: &str) -> String {
    words.chars().find(|c| is_han(*c)).map(String::from).unwrap_or_default()
}

pub fn last_han_char(words: &str) -> String {
    words.chars().rev().find(|c| is_han(*c)).map(String::from).unwrap_or_default()
}

/// 名字叫“汉字个数”，其实 C++ 数的是所有字符，不管是不是汉字；保持一致。
pub fn count_han_chars(words: &str) -> usize {
    words.chars().count()
}

pub fn count_utf8_chars(text: &str) -> usize {
    text.chars().count()
}

/// 给词算辅助码，格式 `(xY)`：单字取字的两个码，多字词取首字、末字各自的第一个码；
/// 第二个码大写（`uppercase_all` 时两个都大写）。任何一个字不在码表里就是空串。
pub fn compute_helpcodes(words: &str, uppercase_all: bool, keymap: &Keymap) -> String {
    let mut codes = String::new();
    if count_han_chars(words) == 1 {
        if let Some(code) = keymap.get(words) {
            codes.push_str(code);
        }
    } else {
        let Some(first) = keymap.get(&first_han_char(words)) else { return String::new() };
        codes.push_str(&first[..1]);
        let Some(last) = keymap.get(&last_han_char(words)) else { return String::new() };
        codes.push_str(&last[..1]);
    }
    if codes.is_empty() {
        return codes;
    }
    if uppercase_all {
        codes = codes.to_ascii_uppercase();
    } else if codes.len() >= 2 {
        let upper = codes[1..2].to_ascii_uppercase();
        codes.replace_range(1..2, &upper);
    }
    format!("({codes})")
}

/// 全拼里最后一个字母是大写，且不是双码：单码辅助。
pub fn is_single_help_mode(pinyin_with_cases: &str) -> bool {
    if pinyin_with_cases.len() <= 1 || is_double_help_mode(pinyin_with_cases) {
        return false;
    }
    pinyin_with_cases.as_bytes()[pinyin_with_cases.len() - 1].is_ascii_uppercase()
}

/// 全拼里最后两个字母都是大写：双码辅助。
pub fn is_double_help_mode(pinyin_with_cases: &str) -> bool {
    let bytes = pinyin_with_cases.as_bytes();
    bytes.len() > 2 && bytes[bytes.len() - 2].is_ascii_uppercase() && bytes[bytes.len() - 1].is_ascii_uppercase()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SingleMatch {
    None,
    First,
    Last,
    Both,
}

/// 单码和一个词的码对不对得上：单字比它的第一个、第二个码，多字词比首字、末字的第一个码。
pub fn match_single_helpcode(word: &str, code: &str, keymap: &Keymap) -> SingleMatch {
    if word.is_empty() || code.len() != 1 {
        return SingleMatch::None;
    }
    let wanted = code.as_bytes()[0];
    let (matches_first, matches_last) = if count_han_chars(word) == 1 {
        let Some(found) = keymap.get(word) else { return SingleMatch::None };
        let bytes = found.as_bytes();
        (bytes[0] == wanted, bytes.len() > 1 && bytes[1] == wanted)
    } else {
        let first = keymap.get(&first_han_char(word));
        let last = keymap.get(&last_han_char(word));
        (
            first.is_some_and(|c| c.as_bytes()[0] == wanted),
            last.is_some_and(|c| c.as_bytes()[0] == wanted),
        )
    };
    match (matches_first, matches_last) {
        (true, true) => SingleMatch::Both,
        (true, false) => SingleMatch::First,
        (false, true) => SingleMatch::Last,
        (false, false) => SingleMatch::None,
    }
}

/// 双码和一个词的码对不对得上：单字比两个码，多字词比首字、末字的第一个码。
pub fn matches_double_helpcodes(word: &str, codes: &str, keymap: &Keymap) -> bool {
    if word.is_empty() || codes.len() != 2 {
        return false;
    }
    let wanted = codes.as_bytes();
    if count_han_chars(word) == 1 {
        return keymap.get(word).is_some_and(|c| c.len() > 1 && c.as_bytes()[0] == wanted[0] && c.as_bytes()[1] == wanted[1]);
    }
    match (keymap.get(&first_han_char(word)), keymap.get(&last_han_char(word))) {
        (Some(first), Some(last)) => first.as_bytes()[0] == wanted[0] && last.as_bytes()[0] == wanted[1],
        _ => false,
    }
}

/// 单码重排：首字匹配的在前（包括首尾都匹配的），其次是末字匹配的，其余保持原序。码长不是 1 时原样返回。
pub fn reorder_with_single_helpcode<T: Clone>(list: &[T], word_of: impl Fn(&T) -> &str, code: &str, keymap: &Keymap) -> Vec<T> {
    if code.len() != 1 {
        return list.to_vec();
    }
    let (mut first, mut last, mut rest) = (Vec::new(), Vec::new(), Vec::new());
    for item in list {
        match match_single_helpcode(word_of(item), code, keymap) {
            SingleMatch::First | SingleMatch::Both => first.push(item.clone()),
            SingleMatch::Last => last.push(item.clone()),
            SingleMatch::None => rest.push(item.clone()),
        }
    }
    first.extend(last);
    first.extend(rest);
    first
}

/// 双码筛选：只留匹配的，次序不变。码长不是 2 时是空。
pub fn filter_with_double_helpcodes<T: Clone>(list: &[T], word_of: impl Fn(&T) -> &str, codes: &str, keymap: &Keymap) -> Vec<T> {
    if codes.len() != 2 {
        return Vec::new();
    }
    list.iter().filter(|item| matches_double_helpcodes(word_of(item), codes, keymap)).cloned().collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn keymap() -> Keymap {
        parse_keymap("\u{feff}# 注释\n阿=ek\r\n你=nu\n好=hv\n八=k\n坏=BAD\n=xx\n无等号\n你=ni\n")
    }

    #[test]
    fn parsing_follows_the_cpp_loader() {
        let map = keymap();
        assert_eq!(map.get("阿").map(String::as_str), Some("ek"), "BOM 和 \\r 要去掉");
        assert_eq!(map.get("你").map(String::as_str), Some("ni"), "重复的字以后者为准");
        assert!(!map.contains_key("坏"), "码必须是小写字母");
        assert_eq!(map.len(), 4, "阿 你 好 八，注释、空字、无等号的行都跳过");
    }

    #[test]
    fn single_letter_codes_are_kept_when_they_are_lowercase_ascii() {
        // C++ 取 `=` 后最多两个字节、全是小写字母就收，所以一个字母的码也收。
        let map = parse_keymap("八=k\n");
        assert_eq!(map.get("八").map(String::as_str), Some("k"));
    }

    #[test]
    fn words_get_a_code_with_the_second_letter_capitalised() {
        let map = parse_keymap("你=nu\n好=hv\n");
        assert_eq!(compute_helpcodes("你", false, &map), "(nU)");
        assert_eq!(compute_helpcodes("你", true, &map), "(NU)");
        assert_eq!(compute_helpcodes("你好", false, &map), "(nH)");
        assert_eq!(compute_helpcodes("你坏", false, &map), "", "有一个字不在码表里就没有码");
        assert_eq!(compute_helpcodes("", false, &map), "");
    }

    #[test]
    fn single_and_double_codes_match_the_right_characters() {
        let map = parse_keymap("你=nu\n好=hv\n们=mn\n");
        assert_eq!(match_single_helpcode("你", "n", &map), SingleMatch::First);
        assert_eq!(match_single_helpcode("你", "u", &map), SingleMatch::Last);
        assert_eq!(match_single_helpcode("你好", "h", &map), SingleMatch::Last);
        assert_eq!(match_single_helpcode("你们", "n", &map), SingleMatch::First);
        assert_eq!(match_single_helpcode("你", "ab", &map), SingleMatch::None);
        assert!(matches_double_helpcodes("你", "nu", &map));
        assert!(matches_double_helpcodes("你好", "nh", &map));
        assert!(!matches_double_helpcodes("你好", "hn", &map));
    }

    #[test]
    fn reordering_puts_first_matches_before_last_matches() {
        let map = parse_keymap("你=nu\n好=hv\n们=mn\n");
        let words: Vec<String> = ["好", "们", "你", "你好"].iter().map(|s| s.to_string()).collect();
        // 码 n：你（首码 n）和 你好（首字 你 的首码 n）在前，们（第二个码 n）在后。
        let order = reorder_with_single_helpcode(&words, |w| w.as_str(), "n", &map);
        assert_eq!(order, ["你", "你好", "们", "好"]);
        assert_eq!(reorder_with_single_helpcode(&words, |w| w.as_str(), "nx", &map), words);
    }

    #[test]
    fn modes_look_at_the_trailing_capitals() {
        assert!(is_single_help_mode("niH"));
        assert!(!is_single_help_mode("niHA"));
        assert!(is_double_help_mode("niHA"));
        assert!(!is_double_help_mode("HA"), "长度不超过 2 不算");
        assert!(!is_single_help_mode("H"), "长度不超过 1 不算");
    }
}
