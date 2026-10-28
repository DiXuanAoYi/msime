//! V 模式（数字转中文、金额大写、算式求值）和 Unicode 模式，对应
//! `engine/local_modes/{v_mode_query,unicode_query}.cpp`。

use crate::lattice::Source;
use crate::local::Item;

const READING_DIGITS: [&str; 10] = ["零", "一", "二", "三", "四", "五", "六", "七", "八", "九"];
const FINANCIAL_DIGITS: [&str; 10] = ["零", "壹", "贰", "叁", "肆", "伍", "陆", "柒", "捌", "玖"];
/// 逐位读数字用 〇，和日期里的「二〇二四年」一样。
const SPELLED_DIGITS: [&str; 10] = ["〇", "一", "二", "三", "四", "五", "六", "七", "八", "九"];
const READING_UNITS: [&str; 4] = ["", "十", "百", "千"];
const FINANCIAL_UNITS: [&str; 4] = ["", "拾", "佰", "仟"];
const GROUP_UNITS: [&str; 4] = ["", "万", "亿", "万亿"];
/// 万亿以上没有通用的读法，读法和大写都只到 16 位。
const MAX_INTEGER_DIGITS: usize = 16;
/// 括号嵌套的上限，防止病态输入把递归下降打爆栈。
const MAX_EXPRESSION_DEPTH: i32 = 64;

fn all_digits(text: &str) -> bool {
    !text.is_empty() && text.bytes().all(|b| b.is_ascii_digit())
}

fn strip_leading_zeros(digits: &str) -> &str {
    digits.trim_start_matches('0')
}

/// 四位以内的一节，节内的零按「中间有零读一个零，末尾的零不读」处理。
fn group_text(group: u32, digits: &[&str; 10], units: &[&str; 4]) -> String {
    const POWERS: [u32; 4] = [1000, 100, 10, 1];
    let mut text = String::new();
    let mut pending_zero = false;
    for (position, power) in POWERS.iter().enumerate() {
        let digit = (group / power % 10) as usize;
        if digit == 0 {
            pending_zero = !text.is_empty();
            continue;
        }
        if pending_zero {
            text.push_str(digits[0]);
            pending_zero = false;
        }
        text.push_str(digits[digit]);
        text.push_str(units[3 - position]);
    }
    text
}

/// 按四位一节从高到低拼，节与节之间：前面有数、这一节不满千或中间隔了全零的节，就补一个零。
fn integer_text(digits_text: &str, financial: bool) -> String {
    let digits = if financial { &FINANCIAL_DIGITS } else { &READING_DIGITS };
    let units = if financial { &FINANCIAL_UNITS } else { &READING_UNITS };
    let digits_only = strip_leading_zeros(digits_text);
    if digits_only.is_empty() {
        return digits[0].to_string();
    }
    if digits_only.len() > MAX_INTEGER_DIGITS {
        return String::new();
    }
    let group_count = digits_only.len().div_ceil(4);
    let head = digits_only.len() - (group_count - 1) * 4;
    let mut text = String::new();
    let mut need_zero = false;
    for index in 0..group_count {
        let start = if index == 0 { 0 } else { head + (index - 1) * 4 };
        let length = if index == 0 { head } else { 4 };
        let group: u32 = digits_only[start..start + length].parse().unwrap_or(0);
        if group == 0 {
            need_zero = true;
            continue;
        }
        if !text.is_empty() && (need_zero || group < 1000) {
            text.push_str(digits[0]);
        }
        need_zero = false;
        text.push_str(&group_text(group, digits, units));
        text.push_str(GROUP_UNITS[group_count - 1 - index]);
    }
    // 读法里打头的「一十」说成「十」：十五、十万；大写照写「壹拾」。
    if !financial && text.starts_with("一十") {
        text.replace_range(.."一".len(), "");
    }
    text
}

fn spelled_digits(digits: &str, table: &[&str; 10]) -> String {
    digits.bytes().map(|b| table[usize::from(b - b'0')]).collect()
}

fn with_thousands_separators(digits: &str) -> String {
    let mut text = String::new();
    for (index, ch) in digits.chars().enumerate() {
        if index > 0 && (digits.len() - index) % 3 == 0 {
            text.push(',');
        }
        text.push(ch);
    }
    text
}

/// 纯数字：整数，或「整数.小数」，各至少一位。
fn split_number(input: &str) -> Option<(&str, &str)> {
    let (integer, fraction, has_dot) = match input.find('.') {
        Some(dot) => (&input[..dot], &input[dot + 1..], true),
        None => (input, "", false),
    };
    (all_digits(integer) && (!has_dot || all_digits(fraction))).then_some((integer, fraction))
}

fn number_candidates(integer: &str, fraction: &str) -> Vec<String> {
    let reading = integer_text(integer, false);
    let financial = integer_text(integer, true);
    if fraction.is_empty() {
        let digits_only = strip_leading_zeros(integer);
        let mut texts = vec![reading, financial, chinese_money(integer, ""), spelled_digits(integer, &SPELLED_DIGITS)];
        if digits_only.len() >= 4 {
            texts.push(with_thousands_separators(digits_only));
        }
        texts
    } else {
        // 带小数时多半是金额，金额排第一。
        let mut texts = vec![chinese_money(integer, fraction)];
        if !reading.is_empty() {
            texts.push(format!("{reading}点{}", spelled_digits(fraction, &READING_DIGITS)));
        }
        if !financial.is_empty() {
            texts.push(format!("{financial}点{}", spelled_digits(fraction, &FINANCIAL_DIGITS)));
        }
        texts
    }
}

/// + - 一层、* / 一层、一元正负号和括号，按通常的优先级和左结合求值。
struct Parser<'a> {
    text: &'a [u8],
    position: usize,
}

impl Parser<'_> {
    fn parse(&mut self) -> Option<f64> {
        let value = self.expression(0)?;
        (self.position == self.text.len()).then_some(value)
    }

    fn peek(&self) -> Option<u8> {
        self.text.get(self.position).copied()
    }

    fn expression(&mut self, depth: i32) -> Option<f64> {
        let mut value = self.term(depth)?;
        while let Some(op @ (b'+' | b'-')) = self.peek() {
            self.position += 1;
            let right = self.term(depth)?;
            value = if op == b'+' { value + right } else { value - right };
        }
        Some(value)
    }

    fn term(&mut self, depth: i32) -> Option<f64> {
        let mut value = self.factor(depth)?;
        while let Some(op @ (b'*' | b'/')) = self.peek() {
            self.position += 1;
            let right = self.factor(depth)?;
            if op == b'/' && right == 0.0 {
                return None;
            }
            value = if op == b'*' { value * right } else { value / right };
        }
        Some(value)
    }

    fn factor(&mut self, depth: i32) -> Option<f64> {
        if depth > MAX_EXPRESSION_DEPTH {
            return None;
        }
        match self.peek()? {
            sign @ (b'+' | b'-') => {
                self.position += 1;
                let value = self.factor(depth + 1)?;
                Some(if sign == b'-' { -value } else { value })
            }
            b'(' => {
                self.position += 1;
                let value = self.expression(depth + 1)?;
                if self.peek() != Some(b')') {
                    return None;
                }
                self.position += 1;
                Some(value)
            }
            _ => self.number(),
        }
    }

    /// 数字至少一位，小数点两边都要有数字：1.、.5、1.2.3 都不认。
    fn number(&mut self) -> Option<f64> {
        let start = self.position;
        while self.peek().is_some_and(|b| b.is_ascii_digit()) {
            self.position += 1;
        }
        if self.position == start {
            return None;
        }
        if self.peek() == Some(b'.') {
            self.position += 1;
            let fraction_start = self.position;
            while self.peek().is_some_and(|b| b.is_ascii_digit()) {
                self.position += 1;
            }
            if self.position == fraction_start {
                return None;
            }
        }
        std::str::from_utf8(&self.text[start..self.position]).ok()?.parse().ok()
    }
}

/// 等价于 C 的 `%.15g`（只用在绝对值不小于 1e15 的数上，所以一定是科学计数法）。
fn format_general_15(value: f64) -> String {
    let scientific = format!("{value:.14e}");
    let (mantissa, exponent) = scientific.split_once('e').expect("科学计数法");
    let mantissa = if mantissa.contains('.') { mantissa.trim_end_matches('0').trim_end_matches('.') } else { mantissa };
    let exponent: i32 = exponent.parse().expect("指数");
    format!("{mantissa}e{}{:02}", if exponent < 0 { '-' } else { '+' }, exponent.abs())
}

/// 至多十位小数，去掉末尾的零：0.1+0.2 给 0.3，1/3 给 0.3333333333。太大的数用有效数字表示。
fn format_result(value: f64) -> String {
    if !value.is_finite() {
        return String::new();
    }
    if value.abs() >= 1e15 {
        return format_general_15(value);
    }
    let mut text = format!("{value:.10}");
    text.truncate(text.trim_end_matches('0').len());
    if text.ends_with('.') {
        text.pop();
    }
    if text == "-0" { "0".to_string() } else { text }
}

fn has_operator(input: &str) -> bool {
    input.contains(['+', '-', '*', '/', '(', ')'])
}

/// 读法：一百二十三、十五、一万零一十；超过 16 位返回空串。
pub fn chinese_number_reading(digits: &str) -> String {
    if all_digits(digits) { integer_text(digits, false) } else { String::new() }
}

/// 大写：壹佰贰拾叁、壹拾伍；超过 16 位返回空串。
pub fn chinese_financial_number(digits: &str) -> String {
    if all_digits(digits) { integer_text(digits, true) } else { String::new() }
}

/// 金额大写：整数部分加至多两位小数，壹佰贰拾叁元肆角伍分；超出范围返回空串。
pub fn chinese_money(integer_digits: &str, fraction_digits: &str) -> String {
    if !all_digits(integer_digits) || fraction_digits.len() > 2 || (!fraction_digits.is_empty() && !all_digits(fraction_digits)) {
        return String::new();
    }
    let integer = integer_text(integer_digits, true);
    if integer.is_empty() {
        return String::new();
    }
    let integer_zero = strip_leading_zeros(integer_digits).is_empty();
    let fraction = fraction_digits.as_bytes();
    let jiao = fraction.first().map_or(0, |b| usize::from(b - b'0'));
    let fen = fraction.get(1).map_or(0, |b| usize::from(b - b'0'));

    let mut text = if integer_zero { String::new() } else { format!("{integer}元") };
    if jiao == 0 && fen == 0 {
        return if integer_zero { format!("{}元整", FINANCIAL_DIGITS[0]) } else { format!("{text}整") };
    }
    if jiao > 0 {
        text.push_str(FINANCIAL_DIGITS[jiao]);
        text.push('角');
    } else if !integer_zero {
        text.push_str(FINANCIAL_DIGITS[0]); // 壹佰元零伍分
    }
    if fen > 0 {
        text.push_str(FINANCIAL_DIGITS[fen]);
        text.push('分');
    }
    text
}

/// 算式求值的结果文本，不是合法算式时返回 `None`。
pub fn evaluate_expression(expression: &str) -> Option<String> {
    if expression.is_empty() {
        return None;
    }
    let value = Parser { text: expression.as_bytes(), position: 0 }.parse()?;
    let result = format_result(value);
    (!result.is_empty()).then_some(result)
}

/// V 模式的候选，`input` 是 V（或全拼的 v）后面那一串。
pub fn query_v_mode(input: &str, limit: i32) -> Vec<Item> {
    if limit <= 0 || input.is_empty() {
        return Vec::new();
    }
    let mut texts: Vec<String> = Vec::new();
    if !has_operator(input) {
        if let Some((integer, fraction)) = split_number(input) {
            texts = number_candidates(integer, fraction);
        }
    } else if let Some(result) = evaluate_expression(input) {
        texts.push(result.clone());
        texts.push(format!("{input}={result}"));
    }
    texts.retain(|t| !t.is_empty());
    let mut words: Vec<String> = Vec::new();
    for text in texts {
        if words.contains(&text) {
            continue;
        }
        if words.len() as i32 >= limit {
            break;
        }
        words.push(text);
    }
    let count = words.len();
    words.into_iter().enumerate().map(|(index, word)| Item::new("", &word, (count - index) as i64, Source::Generated)).collect()
}

/// Unicode 模式：`hex_part` 是 U 后面的部分，可选的加号加一到六位十六进制数。
pub fn query_unicode(hex_part: &str, limit: i32) -> Vec<Item> {
    if limit <= 0 {
        return Vec::new();
    }
    let hex = hex_part.strip_prefix('+').unwrap_or(hex_part);
    if hex.is_empty() || hex.len() > 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Vec::new();
    }
    let codepoint = u32::from_str_radix(hex, 16).expect("十六进制");
    // Unicode 标量值：不超过 10FFFF，也不在代理区。
    let Some(character) = char::from_u32(codepoint) else { return Vec::new() };
    vec![Item::new(&format!("U+{codepoint:04X}"), &character.to_string(), hex.len() as i64, Source::Generated)]
}
