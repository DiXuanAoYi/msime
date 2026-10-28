//! 双拼：九套内置方案、按键到全拼音节的解码、分段，以及全码辅助的判断。
//!
//! 移植自 `engine/shuangpin/{shuangpin_profile,shuangpin_utils}.cpp`。双拼只是全拼的另一种敲法：
//! 解码得到的全拼音节之后走的还是全拼词库，所以这里不碰词库。
//! 方案表由脚本从 C++ 源码机械抽取（没有手抄），`tests/shuangpin_helpcode_golden.rs` 再把表原样和 C++ 导出的表比一遍，
//! 并对所有一到两个字符的按键码比解码结果。
//!
//! 输入按 ASCII 处理，和 C++ 一样按字节切；用户自定义方案（`BuildCustomShuangpinProfile`）还在 C++。

use crate::syllables::INTACT;
use std::collections::{BTreeMap, HashSet};
use std::sync::OnceLock;

/// 一套双拼方案。三张表都是“全拼里的写法 → 按键”。
#[derive(Debug, Clone, PartialEq)]
pub struct Profile {
    pub name: String,
    /// 声母：`sh` → `u`。没列出的声母就是它自己的字母。
    pub initials: BTreeMap<String, String>,
    /// 零声母音节怎么敲：`a` → `aa`（小鹤）或 `oa`（微软）。
    pub zero_initials: BTreeMap<String, String>,
    /// 韵母：`iu` → `q`。一个键可以对应多个韵母，由声母决定哪个是合法音节。
    pub finals: BTreeMap<String, String>,
}

impl Profile {
    fn new(name: &str, initials: &[(&str, &str)], zero_initials: &[(&str, &str)], finals: &[(&str, &str)]) -> Self {
        let table = |pairs: &[(&str, &str)]| pairs.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect();
        Self { name: name.to_string(), initials: table(initials), zero_initials: table(zero_initials), finals: table(finals) }
    }

    /// 微软、搜狗、紫光把 `ing` 放在 `;` 上，所以 `;` 也是音节第二位的合法按键。
    pub fn uses_semicolon_final(&self) -> bool {
        self.finals.values().any(|code| code == ";")
    }
}

/// 内置方案的名字，顺序同 C++ 的注册顺序。
pub const PROFILE_NAMES: [&str; 9] =
    ["xiaohe", "ziranma", "shoudao", "microsoft", "sogou", "ziguang", "zhinengabc", "guobiao", "pinyinjiajia"];

/// 按名字取内置方案；不认识的名字回到小鹤，和 C++ 的 `GetShuangpinProfile` 一致。
pub fn profile(name: &str) -> &'static Profile {
    static ALL: OnceLock<Vec<Profile>> = OnceLock::new();
    let all = ALL.get_or_init(|| {
        vec![xiaohe(), ziranma(), shoudao(), microsoft(), sogou(), ziguang(), zhinengabc(), guobiao(), pinyinjiajia()]
    });
    all.iter().find(|p| p.name == name).unwrap_or(&all[0])
}

/// 双拼解码认的音节：全拼的完整音节，加上 `eng`，去掉会让双拼分段产生歧义的几个历史排除项。
pub fn decodable_syllables() -> &'static HashSet<&'static str> {
    static SET: OnceLock<HashSet<&'static str>> = OnceLock::new();
    SET.get_or_init(|| {
        let mut set: HashSet<&'static str> = INTACT.iter().copied().collect();
        set.insert("eng");
        for excluded in ["chua", "den", "fiao", "jve", "lo", "lue", "nou", "nue", "nun", "qve", "xve", "yve", "zhei"] {
            set.remove(excluded);
        }
        set
    })
}

fn source_by_code<'a>(mapping: &'a BTreeMap<String, String>, code: &str) -> Option<&'a str> {
    mapping.iter().find(|(_, mapped)| mapped.as_str() == code).map(|(source, _)| source.as_str())
}

/// 一个双拼音节解码成全拼；解不出来是空串。
pub fn to_pinyin(code: &str, profile: &Profile) -> String {
    if let Some(zero) = source_by_code(&profile.zero_initials, code) {
        if !zero.is_empty() {
            return zero.to_string();
        }
    }
    if code.len() != 2 || !code.is_ascii() {
        return String::new();
    }
    let initial = source_by_code(&profile.initials, &code[..1]).unwrap_or(&code[..1]);
    let mut result = String::new();
    for (final_part, key) in &profile.finals {
        if key != &code[1..2] {
            continue;
        }
        // ü 在 j/q/x/y 后面写成 u。
        let normalized = if final_part == "v" && matches!(initial, "j" | "q" | "x" | "y") { "u" } else { final_part };
        let syllable = format!("{initial}{normalized}");
        if decodable_syllables().contains(syllable.as_str()) {
            result = syllable;
        }
    }
    result
}

/// 一段不含分隔符的码是不是分段认的音节。
pub fn is_accepted_syllable_code(code: &str, profile: &Profile) -> bool {
    decodable_syllables().contains(to_pinyin(code, profile).as_str())
}

/// 用单引号切开双拼：从左往右贪心，两个字符能解成音节就吃两个，否则吃一个。
pub fn segment(input: &str, profile: &Profile) -> String {
    let bytes = input.as_bytes();
    if bytes.len() == 1 {
        return input.to_string();
    }
    let mut pieces: Vec<Vec<u8>> = Vec::new();
    let mut start = 0;
    while start < bytes.len() {
        if start + 2 <= bytes.len() {
            let pair = &bytes[start..start + 2];
            let lowered = String::from_utf8_lossy(pair).to_ascii_lowercase();
            if is_accepted_syllable_code(&lowered, profile) {
                pieces.push(pair.to_vec());
                start += 2;
            } else {
                pieces.push(vec![pair[0]]);
                start += 1;
            }
        } else {
            pieces.push(vec![bytes[bytes.len() - 1]]);
            start += 1;
        }
    }
    let joined = pieces.join(&b'\'');
    String::from_utf8_lossy(&joined).into_owned()
}

/// 把切好的双拼转成切好的全拼：两个字符解码，单个字符是声母就还原成全拼的声母。
pub fn segmented_to_pinyin(segmented: &str, profile: &Profile) -> String {
    let mut out: Vec<String> = Vec::new();
    for piece in segmented.split('\'') {
        match piece.len() {
            1 => out.push(source_by_code(&profile.initials, piece).unwrap_or(piece).to_string()),
            2 => out.push(to_pinyin(piece, profile)),
            _ => {}
        }
    }
    out.join("'")
}

/// 前面的部分每个单元都是完整的两个字符（分段里没有单字符单元）。
fn all_units_complete(pure: &str, segmented: &str) -> bool {
    if pure.len() % 2 != 0 {
        return false;
    }
    let bytes = segmented.as_bytes();
    let at = |i: usize| bytes.get(i).copied().unwrap_or(0);
    let mut index = 0;
    while index < bytes.len() {
        if at(index) == b'\'' || at(index + 1) == b'\'' {
            return false;
        }
        index += 3;
    }
    true
}

/// 全码辅助：长度为偶数，前面都是完整的双拼，末尾两个辅助码里至少一个是大写字母。
pub fn is_full_help_mode(input: &str, profile: &Profile) -> bool {
    let bytes = input.as_bytes();
    let len = bytes.len();
    if len == 0 || len == 2 || len % 2 != 0 {
        return false;
    }
    let pure = String::from_utf8_lossy(&bytes[..len - 2]).into_owned();
    if !all_units_complete(&pure, &segment(&pure, profile)) {
        return false;
    }
    bytes[len - 2].is_ascii_uppercase() || bytes[len - 1].is_ascii_uppercase()
}

/// 全码辅助里末尾两个辅助码；第一个是大写时两个码对调（先敲的是第二部件）。
pub fn full_help_codes(input: &str) -> String {
    let bytes = input.as_bytes();
    if bytes.len() < 2 {
        return String::new();
    }
    let reverse = bytes[bytes.len() - 2].is_ascii_uppercase();
    let mut codes = bytes[bytes.len() - 2..].to_ascii_lowercase();
    if reverse {
        codes.swap(0, 1);
    }
    String::from_utf8_lossy(&codes).into_owned()
}

// ---------------------------------------------------------------------------------------------
// 以下九套方案由脚本从 engine/shuangpin/shuangpin_profile.cpp 抽取，不要手改。
// ---------------------------------------------------------------------------------------------

fn xiaohe() -> Profile {
    Profile::new(
        "xiaohe",
        &[
            ("sh", "u"),
            ("ch", "i"),
            ("zh", "v"),
        ],
        &[
            ("a", "aa"),
            ("ai", "ai"),
            ("an", "an"),
            ("ao", "ao"),
            ("ang", "ah"),
            ("e", "ee"),
            ("ei", "ei"),
            ("en", "en"),
            ("eng", "eg"),
            ("er", "er"),
            ("o", "oo"),
            ("ou", "ou"),
        ],
        &[
            ("iu", "q"),
            ("ei", "w"),
            ("e", "e"),
            ("uan", "r"),
            ("ue", "t"),
            ("ve", "t"),
            ("un", "y"),
            ("u", "u"),
            ("i", "i"),
            ("uo", "o"),
            ("o", "o"),
            ("ie", "p"),
            ("a", "a"),
            ("ong", "s"),
            ("iong", "s"),
            ("ai", "d"),
            ("en", "f"),
            ("eng", "g"),
            ("ang", "h"),
            ("an", "j"),
            ("uai", "k"),
            ("ing", "k"),
            ("uang", "l"),
            ("iang", "l"),
            ("ou", "z"),
            ("ua", "x"),
            ("ia", "x"),
            ("ao", "c"),
            ("ui", "v"),
            ("v", "v"),
            ("in", "b"),
            ("iao", "n"),
            ("ian", "m"),
        ],
    )
}

fn ziranma() -> Profile {
    Profile::new(
        "ziranma",
        &[
            ("sh", "u"),
            ("ch", "i"),
            ("zh", "v"),
        ],
        &[
            ("a", "aa"),
            ("ai", "ai"),
            ("an", "an"),
            ("ao", "ao"),
            ("ang", "ah"),
            ("e", "ee"),
            ("ei", "ei"),
            ("en", "en"),
            ("eng", "eg"),
            ("er", "er"),
            ("o", "oo"),
            ("ou", "ou"),
        ],
        &[
            ("iu", "q"),
            ("ia", "w"),
            ("ua", "w"),
            ("e", "e"),
            ("uan", "r"),
            ("ue", "t"),
            ("ve", "t"),
            ("ing", "y"),
            ("uai", "y"),
            ("u", "u"),
            ("i", "i"),
            ("o", "o"),
            ("uo", "o"),
            ("un", "p"),
            ("a", "a"),
            ("iong", "s"),
            ("ong", "s"),
            ("iang", "d"),
            ("uang", "d"),
            ("en", "f"),
            ("eng", "g"),
            ("ang", "h"),
            ("an", "j"),
            ("ao", "k"),
            ("ai", "l"),
            ("ei", "z"),
            ("ie", "x"),
            ("iao", "c"),
            ("ui", "v"),
            ("v", "v"),
            ("ou", "b"),
            ("in", "n"),
            ("ian", "m"),
        ],
    )
}

fn shoudao() -> Profile {
    Profile::new(
        "shoudao",
        &[
            ("sh", "e"),
            ("ch", "i"),
            ("zh", "v"),
        ],
        &[
            ("a", "aa"),
            ("ai", "ai"),
            ("an", "an"),
            ("ao", "ao"),
            ("ang", "ay"),
            ("e", "ue"),
            ("ei", "ui"),
            ("en", "en"),
            ("eng", "uf"),
            ("er", "er"),
            ("o", "oo"),
            ("ou", "ou"),
        ],
        &[
            ("iu", "q"),
            ("ua", "w"),
            ("e", "e"),
            ("ie", "r"),
            ("uan", "t"),
            ("ang", "y"),
            ("u", "u"),
            ("i", "i"),
            ("o", "o"),
            ("uo", "o"),
            ("iao", "p"),
            ("a", "a"),
            ("ou", "s"),
            ("ao", "d"),
            ("eng", "f"),
            ("uai", "g"),
            ("ing", "g"),
            ("ong", "h"),
            ("iong", "h"),
            ("an", "j"),
            ("en", "k"),
            ("ia", "k"),
            ("ai", "l"),
            ("ue", "l"),
            ("un", "z"),
            ("iang", "x"),
            ("uang", "x"),
            ("in", "c"),
            ("v", "v"),
            ("ui", "v"),
            ("ve", "b"),
            ("ian", "n"),
            ("ei", "m"),
        ],
    )
}

fn microsoft() -> Profile {
    Profile::new(
        "microsoft",
        &[
            ("sh", "u"),
            ("ch", "i"),
            ("zh", "v"),
        ],
        &[
            ("a", "oa"),
            ("ai", "ol"),
            ("an", "oj"),
            ("ang", "oh"),
            ("ao", "ok"),
            ("e", "oe"),
            ("ei", "oz"),
            ("en", "of"),
            ("eng", "og"),
            ("er", "or"),
            ("o", "oo"),
            ("ou", "ob"),
        ],
        &[
            ("iu", "q"),
            ("ia", "w"),
            ("ua", "w"),
            ("e", "e"),
            ("uan", "r"),
            ("ue", "t"),
            ("ve", "v"),
            ("uai", "y"),
            ("v", "y"),
            ("u", "u"),
            ("i", "i"),
            ("o", "o"),
            ("uo", "o"),
            ("un", "p"),
            ("a", "a"),
            ("iong", "s"),
            ("ong", "s"),
            ("iang", "d"),
            ("uang", "d"),
            ("en", "f"),
            ("eng", "g"),
            ("ang", "h"),
            ("an", "j"),
            ("ao", "k"),
            ("ai", "l"),
            ("ing", ";"),
            ("ei", "z"),
            ("ie", "x"),
            ("iao", "c"),
            ("ui", "v"),
            ("ou", "b"),
            ("in", "n"),
            ("ian", "m"),
        ],
    )
}

fn sogou() -> Profile {
    Profile::new(
        "sogou",
        &[
            ("sh", "u"),
            ("ch", "i"),
            ("zh", "v"),
        ],
        &[
            ("a", "oa"),
            ("ai", "ol"),
            ("an", "oj"),
            ("ang", "oh"),
            ("ao", "ok"),
            ("e", "oe"),
            ("ei", "oz"),
            ("en", "of"),
            ("eng", "og"),
            ("er", "or"),
            ("o", "oo"),
            ("ou", "ob"),
        ],
        &[
            ("iu", "q"),
            ("ia", "w"),
            ("ua", "w"),
            ("e", "e"),
            ("uan", "r"),
            ("ue", "t"),
            ("ve", "t"),
            ("uai", "y"),
            ("v", "y"),
            ("u", "u"),
            ("i", "i"),
            ("o", "o"),
            ("uo", "o"),
            ("un", "p"),
            ("a", "a"),
            ("iong", "s"),
            ("ong", "s"),
            ("iang", "d"),
            ("uang", "d"),
            ("en", "f"),
            ("eng", "g"),
            ("ang", "h"),
            ("an", "j"),
            ("ao", "k"),
            ("ai", "l"),
            ("ing", ";"),
            ("ei", "z"),
            ("ie", "x"),
            ("iao", "c"),
            ("ui", "v"),
            ("ou", "b"),
            ("in", "n"),
            ("ian", "m"),
        ],
    )
}

fn ziguang() -> Profile {
    Profile::new(
        "ziguang",
        &[
            ("sh", "i"),
            ("ch", "a"),
            ("zh", "u"),
        ],
        &[
            ("a", "oa"),
            ("ai", "op"),
            ("an", "or"),
            ("ang", "os"),
            ("ao", "oq"),
            ("e", "oe"),
            ("ei", "ok"),
            ("en", "ow"),
            ("eng", "ot"),
            ("er", "oj"),
            ("o", "oo"),
            ("ou", "oz"),
        ],
        &[
            ("ao", "q"),
            ("en", "w"),
            ("e", "e"),
            ("an", "r"),
            ("eng", "t"),
            ("in", "y"),
            ("uai", "y"),
            ("u", "u"),
            ("i", "i"),
            ("o", "o"),
            ("uo", "o"),
            ("ai", "p"),
            ("a", "a"),
            ("ang", "s"),
            ("ie", "d"),
            ("ian", "f"),
            ("iang", "g"),
            ("uang", "g"),
            ("ong", "h"),
            ("iong", "h"),
            ("iu", "j"),
            ("ei", "k"),
            ("uan", "l"),
            ("ing", ";"),
            ("ou", "z"),
            ("ia", "x"),
            ("ua", "x"),
            ("v", "v"),
            ("iao", "b"),
            ("ui", "n"),
            ("ue", "n"),
            ("ve", "n"),
            ("un", "m"),
        ],
    )
}

fn zhinengabc() -> Profile {
    Profile::new(
        "zhinengabc",
        &[
            ("sh", "v"),
            ("ch", "e"),
            ("zh", "a"),
        ],
        &[
            ("a", "oa"),
            ("ai", "ol"),
            ("an", "oj"),
            ("ang", "oh"),
            ("ao", "ok"),
            ("e", "oe"),
            ("ei", "oq"),
            ("en", "of"),
            ("eng", "og"),
            ("er", "or"),
            ("o", "oo"),
            ("ou", "ob"),
        ],
        &[
            ("ei", "q"),
            ("ian", "w"),
            ("e", "e"),
            ("iu", "r"),
            ("iang", "t"),
            ("uang", "t"),
            ("ing", "y"),
            ("u", "u"),
            ("i", "i"),
            ("o", "o"),
            ("uo", "o"),
            ("uan", "p"),
            ("a", "a"),
            ("ong", "s"),
            ("iong", "s"),
            ("ia", "d"),
            ("ua", "d"),
            ("en", "f"),
            ("eng", "g"),
            ("ang", "h"),
            ("an", "j"),
            ("ao", "k"),
            ("ai", "l"),
            ("iao", "z"),
            ("ie", "x"),
            ("in", "c"),
            ("uai", "c"),
            ("v", "v"),
            ("ou", "b"),
            ("un", "n"),
            ("ue", "m"),
            ("ui", "m"),
            ("ve", "m"),
        ],
    )
}

fn guobiao() -> Profile {
    Profile::new(
        "guobiao",
        &[
            ("sh", "u"),
            ("ch", "i"),
            ("zh", "v"),
        ],
        &[
            ("a", "aa"),
            ("ai", "ak"),
            ("an", "af"),
            ("ang", "ag"),
            ("ao", "ac"),
            ("e", "ae"),
            ("ei", "ab"),
            ("en", "ar"),
            ("eng", "ah"),
            ("er", "al"),
            ("o", "ao"),
            ("ou", "ap"),
        ],
        &[
            ("ia", "q"),
            ("ua", "q"),
            ("uan", "w"),
            ("e", "e"),
            ("en", "r"),
            ("ie", "t"),
            ("iu", "y"),
            ("uai", "y"),
            ("u", "u"),
            ("i", "i"),
            ("o", "o"),
            ("uo", "o"),
            ("ou", "p"),
            ("a", "a"),
            ("ong", "s"),
            ("iong", "s"),
            ("ian", "d"),
            ("an", "f"),
            ("ang", "g"),
            ("eng", "h"),
            ("ing", "j"),
            ("ai", "k"),
            ("in", "l"),
            ("un", "z"),
            ("ue", "x"),
            ("ve", "x"),
            ("ao", "c"),
            ("v", "v"),
            ("ui", "v"),
            ("ei", "b"),
            ("iang", "n"),
            ("uang", "n"),
            ("iao", "m"),
        ],
    )
}

fn pinyinjiajia() -> Profile {
    Profile::new(
        "pinyinjiajia",
        &[
            ("sh", "i"),
            ("ch", "u"),
            ("zh", "v"),
        ],
        &[
            ("a", "aa"),
            ("ai", "as"),
            ("an", "af"),
            ("ang", "ag"),
            ("ao", "ad"),
            ("e", "ee"),
            ("ei", "ew"),
            ("en", "er"),
            ("eng", "et"),
            ("er", "eq"),
            ("o", "oo"),
            ("ou", "op"),
        ],
        &[
            ("ing", "q"),
            ("ei", "w"),
            ("e", "e"),
            ("en", "r"),
            ("eng", "t"),
            ("ong", "y"),
            ("iong", "y"),
            ("u", "u"),
            ("i", "i"),
            ("o", "o"),
            ("uo", "o"),
            ("ou", "p"),
            ("a", "a"),
            ("ai", "s"),
            ("ao", "d"),
            ("an", "f"),
            ("ang", "g"),
            ("iang", "h"),
            ("uang", "h"),
            ("ian", "j"),
            ("iao", "k"),
            ("in", "l"),
            ("un", "z"),
            ("ue", "x"),
            ("ve", "x"),
            ("uai", "x"),
            ("uan", "c"),
            ("ui", "v"),
            ("v", "v"),
            ("ia", "b"),
            ("ua", "b"),
            ("iu", "n"),
            ("ie", "m"),
        ],
    )
}
