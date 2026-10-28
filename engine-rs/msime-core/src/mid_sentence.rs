//! 双拼句中辅助码的输入形状，移植自 `engine/contracts/mid_sentence_helpcode.h`。
//!
//! 一段是反引号加至多两个字母（第一码大小写都算，第二码要大写才参与筛选）；大写触发开着时，完整音节后面的大写字母
//! 自己开一段。TSF、Server 和引擎三处按同一条规则解析，所以这里只看形状，不判断前面的键是不是合法音节。
//! 按字节处理（输入是 ASCII 键序列）。

pub const MARKER: u8 = b'`';
pub const SEMICOLON_TRIGGER: u8 = b';';

pub fn is_ascii_letter(ch: u8) -> bool {
    ch.is_ascii_alphabetic()
}

/// `text[marker]` 是反引号，返回这一段结束的位置。
pub fn block_end(text: &[u8], marker: usize) -> usize {
    let mut end = marker + 1;
    if end < text.len() && is_ascii_letter(text[end]) {
        end += 1;
        if end < text.len() && text[end].is_ascii_uppercase() {
            end += 1;
        }
    }
    end
}

/// 当前这一节已有的键数 `chunk_length` 是偶数且至少两个时，这个位置上的大写字母开一段。
pub fn starts_uppercase_block(ch: u8, chunk_length: usize) -> bool {
    ch.is_ascii_uppercase() && chunk_length >= 2 && chunk_length % 2 == 0
}

/// `text[first]` 是大写段的第一码，返回这一段结束的位置。
pub fn uppercase_block_end(text: &[u8], first: usize) -> usize {
    let mut end = first + 1;
    if end < text.len() && text[end].is_ascii_uppercase() {
        end += 1;
    }
    end
}

/// 一段：`begin..end` 整段，`codes_begin` 是第一码的位置。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Block {
    pub begin: usize,
    pub end: usize,
    pub codes_begin: usize,
}

/// 扫描 `text`，返回所有段和最后一节（最后一个 `'` 或段之后）的起点。
pub fn scan_blocks(text: &[u8], uppercase_trigger: bool) -> (Vec<Block>, usize) {
    let mut blocks = Vec::new();
    let (mut chunk_start, mut index) = (0usize, 0usize);
    while index < text.len() {
        if text[index] == b'\'' {
            index += 1;
            chunk_start = index;
        } else if text[index] == MARKER {
            let end = block_end(text, index);
            blocks.push(Block { begin: index, end, codes_begin: index + 1 });
            index = end;
            chunk_start = end;
        } else if uppercase_trigger && starts_uppercase_block(text[index], index - chunk_start) {
            let end = uppercase_block_end(text, index);
            blocks.push(Block { begin: index, end, codes_begin: index });
            index = end;
            chunk_start = end;
        } else {
            index += 1;
        }
    }
    (blocks, chunk_start)
}

pub fn chunk_start(text: &[u8], uppercase_trigger: bool) -> usize {
    scan_blocks(text, uppercase_trigger).1
}

/// `text` 末尾能否接一个反引号：当前这一节是偶数个、至少两个键，也就是光标前恰好是一个个完整的两键音节。
pub fn accepts_marker(text: &[u8], uppercase_trigger: bool) -> bool {
    let chunk_length = text.len() - chunk_start(text, uppercase_trigger);
    chunk_length >= 2 && chunk_length % 2 == 0
}

/// 光标停在 `text[caret]` 时能否插入一个反引号：光标前按 `accepts_marker` 判断，后面不看；光标后紧跟的已经是
/// 这个音节的段时不收，大写触发开着时光标后的大写字母正好开这个音节的段。
pub fn accepts_marker_at(text: &[u8], caret: usize, uppercase_trigger: bool) -> bool {
    caret <= text.len()
        && accepts_marker(&text[..caret], uppercase_trigger)
        && (caret == text.len() || (text[caret] != MARKER && !(uppercase_trigger && text[caret].is_ascii_uppercase())))
}

/// 光标停在 `text[caret]` 时大写字母能否作为第二码：光标前恰好是一段只有第一码的段。
pub fn accepts_second_code_at(text: &[u8], caret: usize, uppercase_trigger: bool) -> bool {
    if caret > text.len() {
        return false;
    }
    let mut open = false;
    for block in scan_blocks(&text[..caret], uppercase_trigger).0 {
        open = block.end == caret && block.end - block.codes_begin == 1;
    }
    open && caret > 0 && is_ascii_letter(text[caret - 1])
}

/// 微软/搜狗/紫光双拼的 `;` 是 ing 韵母，光标前这一节是奇数键时它是编码键。
pub fn accepts_semicolon_final_at(text: &[u8], caret: usize, uppercase_trigger: bool) -> bool {
    if caret > text.len() {
        return false;
    }
    let start = if uppercase_trigger {
        chunk_start(&text[..caret], true)
    } else {
        (1..=caret).rev().find(|&i| text[i - 1] == b'\'').unwrap_or(0)
    };
    (caret - start) % 2 == 1
}
