//! 双拼：九套内置方案、按键到全拼音节的解码、分段，以及全码辅助的判断。
//!
//! 移植自 `engine/shuangpin/{shuangpin_profile,shuangpin_utils}.cpp`。双拼只是全拼的另一种敲法：
//! 解码得到的全拼音节之后走的还是全拼词库，所以这里不碰词库。
//! 方案表由脚本从 C++ 源码机械抽取（没有手抄），`tests/shuangpin_helpcode_golden.rs` 再把表原样和 C++ 导出的表比一遍，
//! 并对所有一到两个字符的按键码比解码结果。
//!
//! 输入按 ASCII 处理，和 C++ 一样按字节切；用户自定义方案（`build_custom_profile`）见下方，测试见
//! `tests/custom_shuangpin_golden.rs`。

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
// 用户自定义方案：移植自 engine/shuangpin/shuangpin_custom_profile.cpp。
// ---------------------------------------------------------------------------------------------

/// 零声母音节怎么编码：`o` 前缀、`a` 前缀、首字母加韵母键，或两个字母的韵母直接拼全。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZeroInitialRule {
    LetterO,
    LetterA,
    FirstLetter,
    FullPinyin,
}

pub fn parse_zero_initial_rule(name: &str) -> Option<ZeroInitialRule> {
    match name {
        "o" => Some(ZeroInitialRule::LetterO),
        "a" => Some(ZeroInitialRule::LetterA),
        "first_letter" => Some(ZeroInitialRule::FirstLetter),
        "full_pinyin" => Some(ZeroInitialRule::FullPinyin),
        _ => None,
    }
}

/// 用户写的布局；键是用户原样写的（可以带大写），建方案时再统一小写。
#[derive(Debug, Clone)]
pub struct CustomLayout {
    pub name: String,
    pub zero_initial_rule: ZeroInitialRule,
    pub initials: BTreeMap<String, String>,
    pub finals: BTreeMap<String, String>,
    pub zero_initial_overrides: BTreeMap<String, String>,
}

const REQUIRED_FINALS: [&str; 33] = [
    "a", "o", "e", "i", "u", "v", "ai", "ei", "ui", "ao", "ou", "iu", "ie", "ue", "ve", "an", "en", "in", "un", "ang", "eng", "ing", "ong",
    "ia", "iao", "ian", "iang", "iong", "ua", "uo", "uai", "uan", "uang",
];
const ZERO_INITIAL_FINALS: [&str; 12] = ["a", "ai", "an", "ang", "ao", "e", "ei", "en", "eng", "er", "o", "ou"];
const ALL_INITIALS: [&str; 23] =
    ["b", "p", "m", "f", "d", "t", "n", "l", "g", "k", "h", "j", "q", "x", "r", "z", "c", "s", "y", "w", "zh", "ch", "sh"];
const RETROFLEX_INITIALS: [&str; 3] = ["zh", "ch", "sh"];
const MAX_REPORTED_CONFLICTS: usize = 8;

fn is_final_key(key: &str) -> bool {
    key.len() == 1 && (key.as_bytes()[0].is_ascii_lowercase() || key == ";")
}

fn zero_initial_code(rule: ZeroInitialRule, final_part: &str, key: &str) -> String {
    match rule {
        ZeroInitialRule::LetterO => return format!("o{key}"),
        ZeroInitialRule::LetterA => return format!("a{key}"),
        ZeroInitialRule::FullPinyin if final_part.len() == 2 => return final_part.to_string(),
        _ => {}
    }
    format!("{}{key}", &final_part[..1])
}

/// 按布局打字时怎么拆一个音节；声母为空表示零声母音节。
fn split_syllable(syllable: &str) -> (&str, &str) {
    let bytes = syllable.as_bytes();
    if bytes.len() > 2 && bytes[1] == b'h' && b"zcs".contains(&bytes[0]) {
        return syllable.split_at(2);
    }
    if b"aeo".contains(&bytes[0]) {
        return ("", syllable);
    }
    syllable.split_at(1)
}

fn check_round_trip(profile: &Profile, errors: &mut Vec<String>) {
    let mut sorted: Vec<&str> = decodable_syllables().iter().copied().collect();
    sorted.sort_unstable();
    let mut conflicts = 0usize;
    for syllable in sorted {
        // j/q/x/y 把 ü 写成 u，解码器不产生它们的 v 写法。
        if matches!(syllable.as_bytes()[0], b'j' | b'q' | b'x' | b'y') && syllable.contains('v') {
            continue;
        }
        let (initial, final_part) = split_syllable(syllable);
        let code = if initial.is_empty() {
            match profile.zero_initials.get(syllable) {
                Some(code) => code.clone(),
                None => continue,
            }
        } else {
            let Some(final_key) = profile.finals.get(final_part).filter(|_| !final_part.is_empty()) else { continue };
            format!("{}{final_key}", profile.initials.get(initial).map(String::as_str).unwrap_or(initial))
        };
        let decoded = to_pinyin(&code, profile);
        if decoded == syllable {
            continue;
        }
        conflicts += 1;
        if conflicts <= MAX_REPORTED_CONFLICTS {
            errors.push(if decoded.is_empty() {
                format!("音节 {syllable} 按 {code} 输入时无法识别")
            } else {
                format!("音节 {syllable} 按 {code} 输入时会被读成 {decoded}")
            });
        }
    }
    if conflicts > MAX_REPORTED_CONFLICTS {
        errors.push(format!("另有 {} 个音节存在同样的冲突", conflicts - MAX_REPORTED_CONFLICTS));
    }
}

/// 由用户布局建方案，并把每个音节按布局打一遍、再用输入时的同一个解码器读回来，证明方案没有歧义。
/// 失败时返回全部错误信息（中文，直接给用户看）。
pub fn build_custom_profile(layout: &CustomLayout) -> Result<Profile, Vec<String>> {
    let mut errors: Vec<String> = Vec::new();
    let mut result = Profile { name: layout.name.clone(), initials: BTreeMap::new(), zero_initials: BTreeMap::new(), finals: BTreeMap::new() };

    // 键 → 用这个键打的声母。没挪过的单字母声母默认就在自己那个键上。
    let mut initial_owner: std::collections::HashMap<String, String> =
        ALL_INITIALS.iter().filter(|i| i.len() == 1).map(|i| (i.to_string(), i.to_string())).collect();
    for (raw_initial, raw_key) in &layout.initials {
        let (initial, key) = (raw_initial.to_ascii_lowercase(), raw_key.to_ascii_lowercase());
        if !ALL_INITIALS.contains(&initial.as_str()) {
            errors.push(format!("未知的声母「{raw_initial}」"));
            continue;
        }
        if key.len() != 1 || !key.as_bytes()[0].is_ascii_lowercase() {
            errors.push(format!("声母 {initial} 的按键必须是一个英文字母，现在是「{raw_key}」"));
            continue;
        }
        result.initials.insert(initial, key);
    }
    for initial in RETROFLEX_INITIALS {
        if !result.initials.contains_key(initial) && errors.is_empty() {
            errors.push(format!("缺少声母 {initial} 的按键"));
        }
    }
    if errors.is_empty() {
        // 被挪走的声母，把默认的那个键让出来。
        for initial in result.initials.keys().filter(|i| i.len() == 1) {
            initial_owner.remove(initial);
        }
        for (initial, key) in &result.initials {
            match initial_owner.get(key) {
                Some(owner) if owner != initial => errors.push(format!("声母 {owner} 和 {initial} 都在 {key} 键上")),
                Some(_) => {}
                None => {
                    initial_owner.insert(key.clone(), initial.clone());
                }
            }
        }
    }

    let mut er_key = String::new();
    for (raw_final, raw_key) in &layout.finals {
        let (final_part, key) = (raw_final.to_ascii_lowercase(), raw_key.to_ascii_lowercase());
        if final_part != "er" && !REQUIRED_FINALS.contains(&final_part.as_str()) {
            errors.push(format!("未知的韵母「{raw_final}」"));
            continue;
        }
        if !is_final_key(&key) {
            errors.push(format!("韵母 {final_part} 的按键必须是一个英文字母或分号，现在是「{raw_key}」"));
            continue;
        }
        if final_part == "er" {
            er_key = key;
        } else {
            result.finals.insert(final_part, key);
        }
    }
    // 只给了 ue 或 ve 之一，另一个照抄。
    if !result.finals.contains_key("ue") {
        if let Some(key) = result.finals.get("ve").cloned() {
            result.finals.insert("ue".into(), key);
        }
    }
    if !result.finals.contains_key("ve") {
        if let Some(key) = result.finals.get("ue").cloned() {
            result.finals.insert("ve".into(), key);
        }
    }
    let missing: Vec<&str> = REQUIRED_FINALS.iter().copied().filter(|f| !result.finals.contains_key(*f)).collect();
    if !missing.is_empty() {
        errors.push(format!("缺少韵母的按键：{}", missing.join("、")));
    }
    if !errors.is_empty() {
        return Err(errors);
    }

    // 多数方案把 er 放在 R 上，文件里没写就用它。
    if er_key.is_empty() {
        er_key = "r".into();
    }
    for final_part in ZERO_INITIAL_FINALS {
        let key = if final_part == "er" { er_key.as_str() } else { result.finals[final_part].as_str() };
        result.zero_initials.insert(final_part.to_string(), zero_initial_code(layout.zero_initial_rule, final_part, key));
    }
    for (raw_final, raw_code) in &layout.zero_initial_overrides {
        let (final_part, code) = (raw_final.to_ascii_lowercase(), raw_code.to_ascii_lowercase());
        if !ZERO_INITIAL_FINALS.contains(&final_part.as_str()) {
            errors.push(format!("「{raw_final}」不是零声母音节，可以单独指定的只有 a、ai、an、ang、ao、e、ei、en、eng、er、o、ou"));
            continue;
        }
        if code.len() != 2 || !code.as_bytes()[0].is_ascii_lowercase() || !is_final_key(&code[1..]) {
            errors.push(format!("零声母 {final_part} 的编码必须是两个键（第二个可以是分号），现在是「{raw_code}」"));
            continue;
        }
        result.zero_initials.insert(final_part, code);
    }
    let mut zero_owner: std::collections::HashMap<&str, &str> = std::collections::HashMap::new();
    for final_part in ZERO_INITIAL_FINALS {
        let code = result.zero_initials[final_part].as_str();
        match zero_owner.get(code) {
            Some(owner) => errors.push(format!("零声母 {owner} 和 {final_part} 的编码都是 {code}")),
            None => {
                zero_owner.insert(code, final_part);
            }
        }
    }
    if !errors.is_empty() {
        return Err(errors);
    }

    check_round_trip(&result, &mut errors);
    if errors.is_empty() {
        Ok(result)
    } else {
        Err(errors)
    }
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
