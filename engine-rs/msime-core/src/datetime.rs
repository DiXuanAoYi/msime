//! Shift+T 日期时间模式，对应 `engine/local_modes/date_time_query.cpp` 和输入形状 `engine/contracts/date_time_input.h`。
//!
//! 和 C++ 一样，所有函数都接受调用方给的“此刻”，不读时钟；农历表（1900–2100）由脚本从 C++ 源码抽出。

use crate::lattice::Source;
use crate::local::Item;
use crate::lunar_table::LUNAR_YEAR_INFO;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DateTime {
    pub year: u32,
    pub month: u32,
    pub day: u32,
    pub weekday: u32,
    pub hour: u32,
    pub minute: u32,
    pub second: u32,
}

// ---------- 输入形状 ----------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Shape {
    None,
    Date,
    DateTime,
    YearMonthDay,
    YearMonth,
    MonthDay,
    Time,
}

#[derive(Debug, Clone, Copy, Default)]
struct Group {
    start: usize,
    length: usize,
}

struct Match {
    shape: Shape,
    group_count: usize,
    groups: [Group; 3],
}

struct Pattern {
    separator: u8,
    max_groups: usize,
    min_complete_groups: usize,
    min_length: [usize; 3],
    max_length: [usize; 3],
    shape_by_groups: [Shape; 3],
}

/// 顺序就是写完时的认定顺序：20241225 同时是 YYYYMMDD 写完和 YYYYMMDDhh 没写完，按前者算。
const PATTERNS: [Pattern; 5] = [
    Pattern { separator: 0, max_groups: 1, min_complete_groups: 1, min_length: [8, 0, 0], max_length: [8, 0, 0], shape_by_groups: [Shape::Date, Shape::None, Shape::None] },
    Pattern { separator: b':', max_groups: 3, min_complete_groups: 1, min_length: [10, 2, 2], max_length: [10, 2, 2], shape_by_groups: [Shape::DateTime, Shape::DateTime, Shape::DateTime] },
    Pattern { separator: b'/', max_groups: 3, min_complete_groups: 2, min_length: [4, 1, 1], max_length: [4, 2, 2], shape_by_groups: [Shape::None, Shape::YearMonth, Shape::YearMonthDay] },
    Pattern { separator: b'/', max_groups: 2, min_complete_groups: 2, min_length: [1, 1, 0], max_length: [2, 2, 0], shape_by_groups: [Shape::None, Shape::MonthDay, Shape::None] },
    Pattern { separator: b':', max_groups: 3, min_complete_groups: 2, min_length: [1, 2, 2], max_length: [2, 2, 2], shape_by_groups: [Shape::None, Shape::Time, Shape::Time] },
];

const MAX_BODY_LENGTH: usize = 16;

/// 把 T 后面那一串切成数字段。只能含数字、/ 和 :，最多三段；有多个分隔符时必须是同一种。
fn split(body: &[u8]) -> Option<([Group; 3], usize, u8)> {
    let mut groups = [Group::default(); 3];
    let mut count = 1;
    let mut separator = 0u8;
    for (index, &ch) in body.iter().enumerate() {
        if ch.is_ascii_digit() {
            groups[count - 1].length += 1;
            continue;
        }
        if ch != b'/' && ch != b':' {
            return None;
        }
        if (separator != 0 && ch != separator) || count == 3 {
            return None;
        }
        separator = ch;
        groups[count] = Group { start: index + 1, length: 0 };
        count += 1;
    }
    Some((groups, count, separator))
}

fn fits(pattern: &Pattern, groups: &[Group; 3], count: usize, separator: u8) -> bool {
    if count > pattern.max_groups || (count > 1 && separator != pattern.separator) || count < pattern.min_complete_groups {
        return false;
    }
    groups.iter().take(count).enumerate().all(|(index, group)| group.length <= pattern.max_length[index] && group.length >= pattern.min_length[index])
}

/// T 后面那一串写完了时是哪种形状，各段在哪里；没写完或不是这些形状时 shape 为 `None`。
fn match_complete(body: &[u8]) -> Match {
    let none = Match { shape: Shape::None, group_count: 0, groups: [Group::default(); 3] };
    if body.is_empty() || body.len() > MAX_BODY_LENGTH {
        return none;
    }
    let Some((groups, count, separator)) = split(body) else { return none };
    for pattern in &PATTERNS {
        if fits(pattern, &groups, count, separator) {
            return Match { shape: pattern.shape_by_groups[count - 1], group_count: count, groups };
        }
    }
    none
}

/// T 后面那一串是否以数字开头，即按指定日期时间而不是唤醒词处理。
fn is_specific_body(body: &str) -> bool {
    body.as_bytes().first().is_some_and(u8::is_ascii_digit)
}

// ---------- 数字和文字 ----------

const WEEKDAYS: [&str; 7] = ["星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"];
const SHORT_WEEKDAYS: [&str; 7] = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"];
const ENGLISH_WEEKDAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const ENGLISH_FULL_WEEKDAYS: [&str; 7] = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];

fn is_date_keyword(keyword: &str) -> bool {
    matches!(keyword, "rq" | "riqi" | "date")
}
fn is_time_keyword(keyword: &str) -> bool {
    matches!(keyword, "sj" | "shijian" | "time")
}
fn is_week_keyword(keyword: &str) -> bool {
    matches!(keyword, "xq" | "xingqi" | "week")
}

fn weekday_index(now: &DateTime) -> usize {
    now.weekday.min(6) as usize
}

fn chinese_digits(value: u32) -> String {
    const DIGITS: [&str; 10] = ["〇", "一", "二", "三", "四", "五", "六", "七", "八", "九"];
    value.to_string().bytes().map(|b| DIGITS[usize::from(b - b'0')]).collect()
}

fn chinese_number(value: u32) -> String {
    const DIGITS: [&str; 10] = ["", "一", "二", "三", "四", "五", "六", "七", "八", "九"];
    match value {
        0..=9 => DIGITS[value as usize].to_string(),
        10 => "十".to_string(),
        11..=19 => format!("十{}", DIGITS[(value - 10) as usize]),
        _ if value % 10 == 0 => format!("{}十", DIGITS[(value / 10) as usize]),
        _ => format!("{}十{}", DIGITS[(value / 10) as usize], DIGITS[(value % 10) as usize]),
    }
}

fn financial_digits(value: u32, minimum_digits: usize) -> String {
    const DIGITS: [&str; 10] = ["零", "壹", "贰", "叁", "肆", "伍", "陆", "柒", "捌", "玖"];
    format!("{value:0minimum_digits$}").bytes().map(|b| DIGITS[usize::from(b - b'0')]).collect()
}

// ---------- 农历 ----------

fn lunar_info(year: u32) -> u32 {
    LUNAR_YEAR_INFO[(year - 1900) as usize]
}

fn lunar_month_days(year: u32, month: u32) -> u32 {
    if lunar_info(year) & (0x10000 >> month) != 0 { 30 } else { 29 }
}

fn lunar_leap_month(year: u32) -> u32 {
    lunar_info(year) & 0x0f
}

fn lunar_leap_days(year: u32) -> u32 {
    if lunar_leap_month(year) == 0 {
        0
    } else if lunar_info(year) & 0x10000 != 0 {
        30
    } else {
        29
    }
}

fn lunar_year_days(year: u32) -> u32 {
    let mut days = 348 + lunar_leap_days(year);
    let mut mask = 0x8000;
    while mask > 0x8 {
        if lunar_info(year) & mask != 0 {
            days += 1;
        }
        mask >>= 1;
    }
    days
}

fn is_leap_year(year: u32) -> bool {
    year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
}

fn days_in_month(year: u32, month: u32) -> u32 {
    const DAYS: [u32; 13] = [0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if month == 0 || month > 12 {
        return 0;
    }
    if month == 2 && is_leap_year(year) { 29 } else { DAYS[month as usize] }
}

fn civil_day_number(year: i64, month: u32, day: u32) -> i64 {
    let year = if month <= 2 { year - 1 } else { year };
    let era = (if year >= 0 { year } else { year - 399 }) / 400;
    let year_of_era = year - era * 400;
    let shifted_month = i64::from(if month > 2 { month - 3 } else { month + 9 });
    let day_of_year = (153 * shifted_month + 2) / 5 + i64::from(day) - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    era * 146097 + day_of_era - 719468
}

fn days_since_lunar_epoch(date: &DateTime) -> Option<u32> {
    if date.year == 0 || date.month == 0 || date.month > 12 || date.day == 0 || date.day > days_in_month(date.year, date.month) {
        return None;
    }
    let epoch = civil_day_number(1900, 1, 31);
    let current = civil_day_number(i64::from(date.year), date.month, date.day);
    (current >= epoch).then(|| (current - epoch) as u32)
}

fn lunar_date(now: &DateTime) -> String {
    let Some(mut remaining) = days_since_lunar_epoch(now) else { return String::new() };
    let mut year = 1900;
    while year <= 2100 {
        let year_days = lunar_year_days(year);
        if remaining < year_days {
            break;
        }
        remaining -= year_days;
        year += 1;
    }
    if year > 2100 {
        return String::new();
    }
    let mut month = 1;
    let mut is_leap = false;
    while month <= 12 {
        let month_days = lunar_month_days(year, month);
        if remaining < month_days {
            break;
        }
        remaining -= month_days;
        if lunar_leap_month(year) == month {
            let leap_days = lunar_leap_days(year);
            if remaining < leap_days {
                is_leap = true;
                break;
            }
            remaining -= leap_days;
        }
        month += 1;
    }
    const STEMS: [&str; 10] = ["甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"];
    const BRANCHES: [&str; 12] = ["子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"];
    format!(
        "{}{}年{}{}月{}日",
        STEMS[((year - 4) % 10) as usize],
        BRANCHES[((year - 4) % 12) as usize],
        if is_leap { "闰" } else { "" },
        chinese_number(month),
        chinese_number(remaining + 1)
    )
}

fn weekday_of(year: u32, month: u32, day: u32) -> u32 {
    // 1970-01-01 是星期四。
    let days = civil_day_number(i64::from(year), month, day);
    (((days + 4) % 7 + 7) % 7) as u32
}

// ---------- 格式表 ----------

/// 一种格式：稳定的 ID 加上按此刻算出的文本。文本为空表示这种格式此刻给不出，候选里跳过它，
/// ID 仍然留在格式表里，学到的顺序不会因此错位。
type Format = (&'static str, String);

/// 这组格式里哪些给得出：唤醒词取此刻，全都给；指定的日期时间只给写到了的部分，不拿此刻去补。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Parts {
    Now,
    Date,
    YearMonth,
    Time,
    TimeWithSeconds,
    DateTime,
    DateTimeWithSeconds,
}

fn text_if(available: bool, text: String) -> String {
    if available { text } else { String::new() }
}

fn date_candidates(now: &DateTime, parts: Parts) -> Vec<Format> {
    let weekday = weekday_index(now);
    let (y, m, d) = (now.year, now.month, now.day);
    let date = matches!(parts, Parts::Now | Parts::Date);
    let date_hm = parts == Parts::Now;
    let year_month = parts == Parts::YearMonth;
    // 年月几种排在最后：它们后加，学到的顺序里没记过的格式插回出厂下标，排在后面不会挤动旧顺序。
    vec![
        ("date:ymd_cn", text_if(date, format!("{y}年{m}月{d}日"))),
        ("date:ymd_dash", text_if(date, format!("{y:04}-{m:02}-{d:02}"))),
        ("date:ymd_slash", text_if(date, format!("{y:04}/{m:02}/{d:02}"))),
        ("date:ymd_dot", text_if(date, format!("{y:04}.{m:02}.{d:02}"))),
        ("date:ymd_compact", text_if(date, format!("{y:04}{m:02}{d:02}"))),
        ("date:yy_md_cn", text_if(date, format!("{:02}年{m}月{d}日", y % 100))),
        ("date:md_cn", text_if(date, format!("{m}月{d}日"))),
        ("date:md_dash", text_if(date, format!("{m:02}-{d:02}"))),
        ("date:md_compact", text_if(date, format!("{m:02}{d:02}"))),
        ("date:ymd_cn_week", text_if(date, format!("{y}年{m}月{d}日 {}", WEEKDAYS[weekday]))),
        ("date:md_cn_week", text_if(date, format!("{m}月{d}日 {}", SHORT_WEEKDAYS[weekday]))),
        ("date:ymd_dash_week", text_if(date, format!("{y:04}-{m:02}-{d:02} {}", ENGLISH_WEEKDAYS[weekday]))),
        ("date:ymd_dash_hm", text_if(date_hm, format!("{y:04}-{m:02}-{d:02} {:02}:{:02}", now.hour, now.minute))),
        ("date:md_cn_hm", text_if(date_hm, format!("{m}月{d}日 {:02}:{:02}", now.hour, now.minute))),
        ("date:ymd_chinese", text_if(date, format!("{}年{}月{}日", chinese_digits(y), chinese_number(m), chinese_number(d)))),
        ("date:ymd_financial", text_if(date, format!("{}年{}月{}日", financial_digits(y, 1), financial_digits(m, 1), financial_digits(d, 2)))),
        ("date:lunar", text_if(date, lunar_date(now))),
        ("date:ym_cn", text_if(year_month, format!("{y}年{m}月"))),
        ("date:ym_dash", text_if(year_month, format!("{y:04}-{m:02}"))),
        ("date:ym_slash", text_if(year_month, format!("{y:04}/{m:02}"))),
        ("date:ym_dot", text_if(year_month, format!("{y:04}.{m:02}"))),
        ("date:ym_chinese", text_if(year_month, format!("{}年{}月", chinese_digits(y), chinese_number(m)))),
    ]
}

fn time_candidates(now: &DateTime, parts: Parts) -> Vec<Format> {
    let (y, m, d) = (now.year, now.month, now.day);
    let (h, mi, s) = (now.hour, now.minute, now.second);
    let hour12 = if h % 12 == 0 { 12 } else { h % 12 };
    let period = if h < 12 { "上午" } else { "下午" };
    let meridiem_upper = if h < 12 { "AM" } else { "PM" };
    let meridiem_lower = if h < 12 { "am" } else { "pm" };
    let colloquial_hour = if hour12 == 2 { "两".to_string() } else { chinese_number(hour12) };
    let colloquial_minutes = match mi {
        0 => String::new(),
        30 => "半".to_string(),
        _ => format!("{}分", chinese_number(mi)),
    };
    let now_parts = parts == Parts::Now;
    // 写了秒就不给丢掉秒的格式，没写秒就不给补出 :00 的格式；年月日加时间只给带日期的那几种。
    let hm = now_parts || parts == Parts::Time;
    let hms = now_parts || parts == Parts::TimeWithSeconds;
    let date_hms = now_parts || matches!(parts, Parts::DateTime | Parts::DateTimeWithSeconds);
    let date_hm = now_parts || parts == Parts::DateTime;
    vec![
        ("time:hm", text_if(hm, format!("{h:02}:{mi:02}"))),
        ("time:hms", text_if(hms, format!("{h:02}:{mi:02}:{s:02}"))),
        ("time:hm_compact", text_if(hm, format!("{h:02}{mi:02}"))),
        ("time:hms_compact", text_if(hms, format!("{h:02}{mi:02}{s:02}"))),
        ("time:period_hm", text_if(hm, format!("{period}{hour12}:{mi:02}"))),
        ("time:period_hm_cn", text_if(hm, format!("{period}{hour12}点{mi:02}分"))),
        ("time:period_colloquial", text_if(hm, format!("{period}{colloquial_hour}点{colloquial_minutes}"))),
        ("time:hm_meridiem", text_if(hm, format!("{hour12}:{mi:02} {meridiem_upper}"))),
        ("time:hm_meridiem_lower", text_if(hm, format!("{hour12}:{mi:02}{meridiem_lower}"))),
        ("time:hms_meridiem", text_if(hms, format!("{hour12:02}:{mi:02}:{s:02} {meridiem_upper}"))),
        ("time:ymd_hms", text_if(date_hms, format!("{y:04}-{m:02}-{d:02} {h:02}:{mi:02}:{s:02}"))),
        ("time:ymd_cn_hm", text_if(date_hm, format!("{y}年{m}月{d}日 {h:02}:{mi:02}"))),
        ("time:md_cn_period_hm", text_if(date_hm, format!("{m}月{d}日 {period}{hour12}:{mi:02}"))),
    ]
}

fn week_candidates(now: &DateTime) -> Vec<Format> {
    let weekday = weekday_index(now);
    vec![
        ("week:cn", WEEKDAYS[weekday].to_string()),
        ("week:cn_sunday", if weekday == 0 { "星期天".to_string() } else { String::new() }),
        ("week:en_full", ENGLISH_FULL_WEEKDAYS[weekday].to_string()),
        ("week:en", ENGLISH_WEEKDAYS[weekday].to_string()),
    ]
}

fn formats_for(keyword: &str, now: &DateTime) -> Vec<Format> {
    if is_date_keyword(keyword) {
        date_candidates(now, Parts::Now)
    } else if is_time_keyword(keyword) {
        time_candidates(now, Parts::Now)
    } else if is_week_keyword(keyword) {
        week_candidates(now)
    } else {
        Vec::new()
    }
}

// ---------- 指定的日期时间 ----------

struct Specific {
    value: DateTime,
    parts: Parts,
}

fn digits_value(text: &[u8], start: usize, length: usize) -> u32 {
    text[start..start + length].iter().fold(0, |value, b| value * 10 + u32::from(b - b'0'))
}

/// 只写月日时年份取 `today` 的；只写时间时日期也取 `today` 的，但那几种带日期的格式不会给出来。取值不对
/// （13 月、2 月 30 日、25 点）时不认。
fn parse_specific(body: &str, today: &DateTime) -> Option<Specific> {
    let bytes = body.as_bytes();
    let matched = match_complete(bytes);
    if matched.shape == Shape::None {
        return None;
    }
    let group = |index: usize, offset: usize, length: usize| {
        let range = matched.groups[index];
        digits_value(bytes, range.start + offset, if length == 0 { range.length } else { length })
    };
    let optional_group = |index: usize| if index < matched.group_count { group(index, 0, 0) } else { 0 };

    let mut value = DateTime { hour: 0, minute: 0, second: 0, ..*today };
    let mut has_date = true;
    let parts;
    match matched.shape {
        Shape::Date | Shape::DateTime => {
            value.year = group(0, 0, 4);
            value.month = group(0, 4, 2);
            value.day = group(0, 6, 2);
            if matched.shape == Shape::DateTime {
                value.hour = group(0, 8, 2);
                value.minute = optional_group(1);
                value.second = optional_group(2);
                parts = if matched.group_count == 3 { Parts::DateTimeWithSeconds } else { Parts::DateTime };
            } else {
                parts = Parts::Date;
            }
        }
        Shape::YearMonthDay | Shape::YearMonth => {
            value.year = group(0, 0, 0);
            value.month = group(1, 0, 0);
            value.day = if matched.shape == Shape::YearMonthDay { group(2, 0, 0) } else { 1 };
            parts = if matched.shape == Shape::YearMonthDay { Parts::Date } else { Parts::YearMonth };
        }
        Shape::MonthDay => {
            value.month = group(0, 0, 0);
            value.day = group(1, 0, 0);
            parts = Parts::Date;
        }
        Shape::Time => {
            has_date = false;
            value.hour = group(0, 0, 0);
            value.minute = group(1, 0, 0);
            value.second = optional_group(2);
            parts = if matched.group_count == 3 { Parts::TimeWithSeconds } else { Parts::Time };
        }
        Shape::None => return None,
    }
    if has_date {
        if value.year == 0 || value.month == 0 || value.month > 12 || value.day == 0 || value.day > days_in_month(value.year, value.month) {
            return None;
        }
        value.weekday = weekday_of(value.year, value.month, value.day);
    }
    if value.hour > 23 || value.minute > 59 || value.second > 59 {
        return None;
    }
    Some(Specific { value, parts })
}

fn specific_formats(specific: &Specific) -> Vec<Format> {
    match specific.parts {
        Parts::Date | Parts::YearMonth => date_candidates(&specific.value, specific.parts),
        Parts::Time | Parts::TimeWithSeconds | Parts::DateTime | Parts::DateTimeWithSeconds => time_candidates(&specific.value, specific.parts),
        Parts::Now => Vec::new(),
    }
}

// ---------- 对外 ----------

pub fn is_date_time_keyword(keyword: &str) -> bool {
    is_date_keyword(keyword) || is_time_keyword(keyword) || is_week_keyword(keyword)
}

/// Shift+T 后面那一串（不含 T）出得了候选：是唤醒词，或者是写完了、取值也对的指定日期时间。
pub fn is_date_time_query(input: &str, now: &DateTime) -> bool {
    is_date_time_keyword(input) || (is_specific_body(input) && parse_specific(input, now).is_some())
}

/// `input` 是唤醒词时按 `now` 出整组格式，是指定的日期时间时只出写到了的部分。
/// 每条候选的 `pinyin` 是它的格式 ID（如 `date:ymd_dash`）。
pub fn query_date_time(input: &str, now: &DateTime, limit: i32) -> Vec<Item> {
    if limit <= 0 {
        return Vec::new();
    }
    let mut candidates = if is_date_time_keyword(input) {
        formats_for(input, now)
    } else if let Some(specific) = is_specific_body(input).then(|| parse_specific(input, now)).flatten() {
        specific_formats(&specific)
    } else {
        return Vec::new();
    };
    candidates.retain(|(_, text)| !text.is_empty());
    let count = candidates.len().min(limit as usize);
    candidates.into_iter().take(count).enumerate().map(|(index, (id, text))| Item::new(id, &text, (count - index) as i64, Source::Generated)).collect()
}

/// 所属的一组格式："date" / "time" / "week"，都不是时为空。
pub fn date_time_category(input: &str) -> &'static str {
    if is_date_keyword(input) {
        return "date";
    }
    if is_time_keyword(input) {
        return "time";
    }
    if is_week_keyword(input) {
        return "week";
    }
    match match_complete(input.as_bytes()).shape {
        Shape::Date | Shape::YearMonthDay | Shape::YearMonth | Shape::MonthDay => "date",
        Shape::DateTime | Shape::Time => "time",
        Shape::None => "",
    }
}

/// 这组格式的全部 ID，按出厂顺序。包括此刻给不出文本、`query_date_time` 跳过的那些。
pub fn date_time_format_ids(keyword: &str) -> Vec<&'static str> {
    formats_for(keyword, &DateTime { year: 2000, month: 1, day: 1, weekday: 6, hour: 0, minute: 0, second: 0 }).into_iter().map(|(id, _)| id).collect()
}

pub const MENU_PREFIX: &str = "menu:";

/// 混输里展开全部格式的那个入口候选。它不上屏：选中时候选框换成 `query_date_time` 的整组格式。
pub fn date_time_menu_item(keyword: &str) -> Item {
    let label = match date_time_category(keyword) {
        "time" => "\u{1F552}时间",
        "week" => "\u{1F4C5}星期",
        _ => "\u{1F4C5}日期",
    };
    Item::new(&format!("{MENU_PREFIX}{keyword}"), label, 0, Source::DateTime)
}

pub fn is_date_time_menu_item(item: &Item) -> bool {
    item.source == Source::DateTime && item.pinyin.starts_with(MENU_PREFIX)
}

pub fn date_time_menu_keyword(item: &Item) -> String {
    if is_date_time_menu_item(item) { item.pinyin[MENU_PREFIX.len()..].to_string() } else { String::new() }
}
