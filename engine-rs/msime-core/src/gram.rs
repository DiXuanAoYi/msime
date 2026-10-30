//! RIME octagram（八股文）`.gram` 语法模型的读取端，对应 `engine/ngram/octagram/octagram_gram.cpp`。
//!
//! 文件是「Metadata + darts-clone 双数组镜像」，键是「上下文尾部字符 + 词首字符」经 `encode` 压缩的字节串，
//! 值是 `int(ln(共现频次) * 10000)`。它不是概率语言模型：`query` 给出的是搭配强度信号。
//! 模型整个读进内存（真模型约 390MB，C++ 用内存映射）；双数组的遍历只用到 darts-clone 的 traverse 和 commonPrefixSearch。

use std::path::Path;

pub const MAX_ENCODED_UNICODE: usize = 8;
const MAX_RESULTS: usize = 8;
const VALUE_SCALE: f64 = 10000.0;

/// 打分参数，默认值 = 万象现行的 6/2/-14/-6/-100/-18。
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GrammarConfig {
    pub collocation_max_length: i32,
    pub collocation_min_length: i32,
    pub collocation_penalty: f64,
    pub non_collocation_penalty: f64,
    pub weak_collocation_penalty: f64,
    pub rear_penalty: f64,
}

impl Default for GrammarConfig {
    fn default() -> Self {
        Self {
            collocation_max_length: 6,
            collocation_min_length: 2,
            collocation_penalty: -14.0,
            non_collocation_penalty: -6.0,
            weak_collocation_penalty: -100.0,
            rear_penalty: -18.0,
        }
    }
}

/// 一次 `query` 的可观测计数。
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct QueryStats {
    pub context_windows: i32,
    pub matched_windows: i32,
    pub matches: i32,
    pub best_value: f64,
}

pub struct GramDb {
    data: Vec<u8>,
    array_offset: usize,
    array_size: usize,
}

// ---------- 编码和码点辅助 ----------

/// 读一个原始 UTF-8 码点并推进 `i`。续字节缺位或非法序列按 Latin-1 单字节兜底。
fn next_codepoint(text: &[u8], i: &mut usize) -> u32 {
    let lead = text[*i];
    *i += 1;
    let take = |i: &mut usize| -> u32 {
        if *i < text.len() && text[*i] & 0xC0 == 0x80 {
            let value = u32::from(text[*i] & 0x3F);
            *i += 1;
            value
        } else {
            0
        }
    };
    if lead < 0x80 {
        u32::from(lead)
    } else if lead & 0xE0 == 0xC0 {
        (u32::from(lead & 0x1F) << 6) | take(i)
    } else if lead & 0xF0 == 0xE0 {
        let high = take(i);
        let low = take(i);
        (u32::from(lead & 0x0F) << 12) | (high << 6) | low
    } else if lead & 0xF8 == 0xF0 {
        let (b1, b2, b3) = (take(i), take(i), take(i));
        (u32::from(lead & 0x07) << 18) | (b1 << 12) | (b2 << 6) | b3
    } else {
        u32::from(lead)
    }
}

/// UTF-8 压缩编码：ASCII 1 字节、CJK 常用区（0x4000–0xA000）2 字节、其余按 7bit 组变长。
pub fn encode(utf8: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(utf8.len());
    let mut i = 0;
    while i < utf8.len() {
        let mut u = next_codepoint(utf8, &mut i);
        if u < 0x80 {
            // 编码串里不允许出现 NUL（要当 C 字符串用），0 被转成 0xE0 占位。
            out.push(if u == 0 { 0xE0 } else { u as u8 });
        } else if (0x4000..0xA000).contains(&u) {
            if u & 0xFF == 0 {
                out.push(0xE1);
                out.push(((u >> 8) + 0x40) as u8);
            } else {
                out.push(((u >> 8) + 0x40) as u8);
                out.push((u & 0xFF) as u8);
            }
        } else {
            let mut bits: i32 = 32;
            while bits > 0 && u & 0xFE00_0000 == 0 {
                bits -= 7;
                u <<= 7;
            }
            let mut bytes_to_encode = (bits + 6) / 7;
            out.push((0xE0 | bytes_to_encode) as u8);
            while bytes_to_encode > 0 {
                bytes_to_encode -= 1;
                out.push((((u >> 25) & 0x7F) | 0x80) as u8);
                u <<= 7;
            }
        }
    }
    out
}

/// 编码串上的游标推进：一个编码码点占 1 字节（ASCII）、2 字节（CJK 常用区）或 1+首字节低 4 位（扩展区变长）。
fn next_unicode(encoded: &[u8], p: usize) -> usize {
    let first = encoded[p];
    p + if first & 0x80 == 0 {
        1
    } else if first & 0xF0 == 0xE0 {
        usize::from(first & 0x0F) + 1
    } else {
        2
    }
}

/// 编码串前 `byte_length` 字节里的码点数。
pub fn unicode_length(encoded: &[u8], byte_length: usize) -> usize {
    let (mut len, mut i) = (0, 0);
    while i < byte_length && i < encoded.len() {
        i = next_unicode(encoded, i);
        len += 1;
    }
    len
}

pub fn codepoint_count(utf8: &[u8]) -> usize {
    let (mut n, mut i) = (0, 0);
    while i < utf8.len() {
        next_codepoint(utf8, &mut i);
        n += 1;
    }
    n
}

pub fn first_codepoints(utf8: &[u8], n: usize) -> &[u8] {
    let (mut seen, mut i) = (0, 0);
    while i < utf8.len() && seen < n {
        next_codepoint(utf8, &mut i);
        seen += 1;
    }
    &utf8[..i]
}

pub fn last_codepoints(utf8: &[u8], n: usize) -> &[u8] {
    let total = codepoint_count(utf8);
    if total <= n {
        return utf8;
    }
    let (mut skip, mut i) = (total - n, 0);
    while i < utf8.len() && skip > 0 {
        next_codepoint(utf8, &mut i);
        skip -= 1;
    }
    &utf8[i..]
}

// ---------- 双数组 ----------

#[derive(Clone, Copy)]
struct Match {
    value: i32,
    length: usize,
}

fn scale_value(value: i32) -> f64 {
    if value >= 0 { f64::from(value) / VALUE_SCALE } else { -1.0 }
}

impl GramDb {
    pub fn open(path: &Path) -> Result<Self, String> {
        let data = std::fs::read(path).map_err(|e| format!("读不了 {}：{e}", path.display()))?;
        Self::from_bytes(data)
    }

    /// 校验 "Rime::Grammar/" 魔数与双数组镜像边界。头部布局：format char[32]、checksum u32、
    /// double_array_size u32（单位数）、相对自身的 int32 偏移。
    pub fn from_bytes(data: Vec<u8>) -> Result<Self, String> {
        const OFFSET_FIELD: usize = 40;
        if data.len() < 44 {
            return Err("file too small for Metadata".to_string());
        }
        if &data[..14] != b"Rime::Grammar/" {
            return Err(format!("format magic mismatch: {}", String::from_utf8_lossy(&data[..14])));
        }
        let read_u32 = |at: usize| u32::from_le_bytes([data[at], data[at + 1], data[at + 2], data[at + 3]]);
        let array_size = read_u32(36) as usize;
        let offset = read_u32(OFFSET_FIELD) as i32;
        let start = OFFSET_FIELD as i64 + i64::from(offset);
        if offset < 0 || start < 0 || start as usize > data.len() || array_size > (data.len() - start as usize) / 4 {
            return Err("double-array image out of bounds".to_string());
        }
        Ok(Self { data, array_offset: start as usize, array_size })
    }

    fn unit(&self, index: usize) -> u32 {
        // 单元下标由双数组自己的偏移算出；越界的下标只可能来自损坏的镜像，按空单元处理。
        if index >= self.array_size {
            return 0;
        }
        let at = self.array_offset + index * 4;
        u32::from_le_bytes([self.data[at], self.data[at + 1], self.data[at + 2], self.data[at + 3]])
    }

    fn has_leaf(unit: u32) -> bool {
        (unit >> 8) & 1 == 1
    }
    fn value(unit: u32) -> i32 {
        (unit & ((1 << 31) - 1)) as i32
    }
    fn label(unit: u32) -> u32 {
        unit & ((1 << 31) | 0xFF)
    }
    fn offset(unit: u32) -> usize {
        ((unit >> 10) << ((unit & (1 << 9)) >> 6)) as usize
    }

    /// darts-clone 的 `traverse`：从根沿着以 NUL 结尾的 `key` 走，返回停下的结点和已走的字节数。
    fn traverse(&self, key: &[u8]) -> (usize, usize) {
        let (mut node_pos, mut id) = (0usize, 0usize);
        let mut unit = self.unit(0);
        let mut key_pos = 0;
        while key_pos < key.len() && key[key_pos] != 0 {
            id ^= Self::offset(unit) ^ usize::from(key[key_pos]);
            unit = self.unit(id);
            if Self::label(unit) != u32::from(key[key_pos]) {
                return (node_pos, key_pos);
            }
            node_pos = id;
            key_pos += 1;
        }
        (node_pos, key_pos)
    }

    /// darts-clone 的 `commonPrefixSearch`：从 `node_pos` 起，沿着 `key` 收集每个到达叶子的前缀。
    fn common_prefix_search(&self, key: &[u8], node_pos: usize) -> Vec<Match> {
        let mut results = Vec::new();
        let mut unit = self.unit(node_pos);
        let mut node_pos = node_pos ^ Self::offset(unit);
        let mut length = 0;
        while length < key.len() && key[length] != 0 {
            node_pos ^= usize::from(key[length]);
            unit = self.unit(node_pos);
            if Self::label(unit) != u32::from(key[length]) {
                return results;
            }
            node_pos ^= Self::offset(unit);
            if Self::has_leaf(unit) && results.len() < MAX_RESULTS {
                results.push(Match { value: Self::value(self.unit(node_pos)), length: length + 1 });
            }
            length += 1;
        }
        results
    }

    fn lookup(&self, context_encoded: &[u8], word_encoded: &[u8]) -> Vec<Match> {
        let (node_pos, key_pos) = self.traverse(context_encoded);
        let context_length = context_encoded.iter().position(|&b| b == 0).unwrap_or(context_encoded.len());
        if key_pos == context_length {
            self.common_prefix_search(word_encoded, node_pos)
        } else {
            Vec::new()
        }
    }

    /// 复刻 `Octagram::Query`。`context`/`word` 是原始 UTF-8；`is_rear` 对应句尾词的「词+$」项。
    pub fn query(&self, context: &str, word: &str, is_rear: bool, config: &GrammarConfig, stats: Option<&mut QueryStats>) -> f64 {
        self.query_bytes(context.as_bytes(), word.as_bytes(), is_rear, config, stats)
    }

    pub fn query_bytes(&self, context: &[u8], word: &[u8], is_rear: bool, config: &GrammarConfig, stats: Option<&mut QueryStats>) -> f64 {
        let mut result = config.non_collocation_penalty;
        let mut local = QueryStats::default();
        let s = stats.unwrap_or(&mut local);
        if context.is_empty() {
            return result;
        }
        // 上下文取尾部 (collocation_max_length - 1) 个码点，词取头部同样数量，再各自 encode。
        let n = (MAX_ENCODED_UNICODE as i32).min(config.collocation_max_length - 1);
        if n < 1 {
            return result;
        }
        let context_tail = last_codepoints(context, n as usize);
        let mut remaining_windows = codepoint_count(context_tail) as i32;
        let context_query = encode(context_tail);
        let word_head = first_codepoints(word, n as usize);
        let word_head_len = codepoint_count(word_head);
        let word_query = encode(word_head);

        let mut context_ptr = 0;
        while remaining_windows > 0 {
            s.context_windows += 1;
            let matches = self.lookup(&context_query[context_ptr..], &word_query);
            if !matches.is_empty() {
                s.matched_windows += 1;
                let at_window_start = context_ptr == 0;
                for m in &matches {
                    s.matches += 1;
                    let scaled = scale_value(m.value);
                    if scaled > s.best_value {
                        s.best_value = scaled;
                    }
                    let match_len = unicode_length(&word_query, m.length) as i32;
                    let collocation_len = remaining_windows + match_len;
                    let whole_query = at_window_start && m.length == word_query.len();
                    let penalty = if collocation_len >= config.collocation_min_length || whole_query {
                        config.collocation_penalty
                    } else {
                        config.weak_collocation_penalty
                    };
                    let candidate = scaled + penalty;
                    if candidate > result {
                        result = candidate;
                    }
                }
            }
            remaining_windows -= 1;
            if context_ptr < context_query.len() {
                context_ptr = next_unicode(&context_query, context_ptr);
            }
        }

        // 句尾标记只在「词未被头部截断」时查：截过的词查 $ 无意义。
        if is_rear && word_head_len == codepoint_count(word) {
            let matches = self.lookup(&word_query, b"$");
            if let Some(first) = matches.first() {
                let candidate = scale_value(first.value) + config.rear_penalty;
                if candidate > result {
                    result = candidate;
                }
            }
        }
        result
    }
}
