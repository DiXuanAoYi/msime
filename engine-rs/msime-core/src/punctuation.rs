//! 中文标点翻译，对应 `engine/core/punctuation_policy.cpp` 和生成的 `contracts/punctuation/policy.h`。
//!
//! 引号成对切换（双、单各一套状态）；书名号可嵌套：最外层是《》，里面用〈〉，计数器决定用哪个，
//! 多出来的 `>` 不会把计数器压成负数。

const SIMPLE: [(char, &str); 15] = [
    (',', "，"),
    ('.', "。"),
    ('?', "？"),
    ('!', "！"),
    (';', "；"),
    (':', "："),
    ('(', "（"),
    (')', "）"),
    ('[', "【"),
    (']', "】"),
    ('\\', "、"),
    ('`', "·"),
    ('$', "￥"),
    ('^', "……"),
    ('_', "——"),
];

/// 引号：(输入, 开, 合)。
const ALTERNATING: [(char, &str, &str); 2] = [('"', "“", "”"), ('\'', "‘", "’")];

#[derive(Debug, Clone)]
pub struct PunctuationPolicy {
    next_double_quote_is_opening: bool,
    next_single_quote_is_opening: bool,
    book_title_nesting: u32,
}

impl Default for PunctuationPolicy {
    fn default() -> Self {
        Self { next_double_quote_is_opening: true, next_single_quote_is_opening: true, book_title_nesting: 0 }
    }
}

impl PunctuationPolicy {
    pub fn new() -> Self {
        Self::default()
    }

    /// 该字符对应的中文标点；不支持的字符返回 `None`。
    pub fn translate(&mut self, character: char) -> Option<&'static str> {
        if let Some((_, output)) = SIMPLE.iter().find(|(input, _)| *input == character) {
            return Some(output);
        }
        if let Some((_, opening, closing)) = ALTERNATING.iter().find(|(input, _, _)| *input == character) {
            let flag = if character == '"' { &mut self.next_double_quote_is_opening } else { &mut self.next_single_quote_is_opening };
            let is_opening = *flag;
            *flag = !*flag;
            return Some(if is_opening { opening } else { closing });
        }
        match character {
            '<' => {
                let outermost = self.book_title_nesting == 0;
                self.book_title_nesting += 1;
                Some(if outermost { "《" } else { "〈" })
            }
            '>' => {
                if self.book_title_nesting > 0 {
                    self.book_title_nesting -= 1;
                    return Some(if self.book_title_nesting == 0 { "》" } else { "〉" });
                }
                Some("》")
            }
            _ => None,
        }
    }
}
