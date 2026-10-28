//! 日语：罗马字方案的按键处理（`schemes/japanese_romaji_scheme.cpp`）、Mozc 词典模型的读取与整句解码
//! （`japanese/japanese_sentence_decoder.cpp`）、矩阵搜索（`japanese/japanese_matrix_search.cpp`）和候选提供者
//! （`providers/japanese_candidate_provider.cpp`）。
//!
//! 模型文件 `dict_japanese.dat`（`MSJPDT1`）整个读进内存；候选里的 `japanese_lexicon` 表来自 `msime.db`，只读打开。
//! 云候选缓存（`dynamic_candidates_`）在 C++ 的提供者里，这里只接受调用方传进来的那一份。

use crate::lattice::Source;
use crate::romaji::{self, Conversion};
use crate::wubi::{KeyStroke, VK_BACKSPACE, VK_ESCAPE, VK_RETURN};
use rusqlite::{Connection, OpenFlags};
use std::collections::HashSet;
use std::path::Path;

const VK_MINUS: u32 = 0xBD;
const VK_APOSTROPHE: u32 = 0xDE;
const UNKNOWN_KANA_COST: i64 = 12000;
const HEADER_SIZE: usize = 56;
const TOKEN_SIZE: usize = 20;
/// 矩阵每一行保留的节点数、一个词条最长占几个假名。
pub const MAX_NODE_A_ROW: usize = 8;
pub const MAX_LEMMA_MORA: usize = 16;

// ---------- 按键方案 ----------

/// 日语方案交给查询层的请求。
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Request {
    pub raw_input: String,
    pub raw_input_with_cases: String,
    pub normalized_input: String,
    pub raw_segmentation: String,
    pub normalized_segmentation: String,
    pub segmentation: String,
    pub key_strokes: Vec<KeyStroke>,
    pub valid: bool,
}

#[derive(Debug, Clone, Default)]
pub struct Scheme {
    raw: String,
    key_strokes: Vec<KeyStroke>,
}

impl Scheme {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn reset(&mut self) {
        self.raw.clear();
        self.key_strokes.clear();
    }

    pub fn handle_key(&mut self, vk: u32, modifiers: u32, wch: u16) {
        if vk == VK_BACKSPACE {
            self.raw.pop();
            self.key_strokes.pop();
            return;
        }
        if vk == VK_ESCAPE || vk == VK_RETURN {
            self.reset();
            return;
        }
        if vk == VK_APOSTROPHE && wch == u16::from(b'\'') {
            self.raw.push('\'');
            self.key_strokes.push(KeyStroke { vk, modifiers, wch });
            return;
        }
        // '-' 是长音符（ー）的罗马字写法，不是翻页键。
        if vk == VK_MINUS && wch == u16::from(b'-') {
            self.raw.push('-');
            self.key_strokes.push(KeyStroke { vk, modifiers, wch });
            return;
        }
        if !(u32::from(b'A')..=u32::from(b'Z')).contains(&vk) {
            return;
        }
        self.key_strokes.push(KeyStroke { vk, modifiers, wch });
        match u8::try_from(wch) {
            Ok(b) if b.is_ascii_alphabetic() => self.raw.push(b as char),
            _ => self.raw.push(((vk as u8) + (b'a' - b'A')) as char),
        }
    }

    pub fn set_raw_input(&mut self, raw: &str, raw_with_cases: &str) {
        let source = if raw_with_cases.is_empty() { raw } else { raw_with_cases };
        self.raw = source.bytes().filter(|b| b.is_ascii_alphabetic() || *b == b'\'' || *b == b'-').map(char::from).collect();
        self.key_strokes.clear();
    }

    pub fn preedit(&self) -> &str {
        &self.raw
    }

    pub fn build_request(&self) -> Request {
        let lower = self.raw.to_ascii_lowercase();
        let converted = romaji::convert(&lower);
        let segmentation = format!("{}{}", converted.hiragana, converted.pending);
        Request {
            raw_input: lower.clone(),
            raw_input_with_cases: self.raw.clone(),
            normalized_input: lower.clone(),
            raw_segmentation: self.raw.clone(),
            normalized_segmentation: segmentation.clone(),
            segmentation,
            key_strokes: self.key_strokes.clone(),
            valid: !lower.is_empty(),
        }
    }
}

// ---------- 模型 ----------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Lemma {
    pub reading: String,
    pub surface: String,
    pub left_id: u16,
    pub right_id: u16,
    pub word_cost: i32,
    pub token_id: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SentenceCandidate {
    pub text: String,
    pub cost: i64,
}

struct Token {
    reading_offset: usize,
    reading_length: usize,
    surface_offset: usize,
    surface_length: usize,
    left_id: u16,
    right_id: u16,
    word_cost: i32,
}

pub struct Decoder {
    data: Vec<u8>,
    token_offset: usize,
    connection_offset: usize,
    string_offset: usize,
    token_count: usize,
    connection_size: usize,
}

fn le_u16(bytes: &[u8]) -> u16 {
    u16::from_le_bytes([bytes[0], bytes[1]])
}
fn le_u32(bytes: &[u8]) -> u32 {
    u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}
fn le_u64(bytes: &[u8]) -> u64 {
    u64::from_le_bytes(bytes[..8].try_into().expect("8 字节"))
}

fn utf8_boundaries(text: &str) -> Vec<usize> {
    let bytes = text.as_bytes();
    let mut boundaries = vec![0];
    let mut index = 0;
    while index < bytes.len() {
        let lead = bytes[index];
        let length = if lead < 0x80 {
            1
        } else if lead >> 5 == 0x6 {
            2
        } else if lead >> 4 == 0xE {
            3
        } else {
            4
        };
        index = bytes.len().min(index + length);
        boundaries.push(index);
    }
    boundaries
}

fn lossy(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

impl Decoder {
    pub fn open(path: &Path) -> Result<Self, String> {
        let data = std::fs::read(path).map_err(|e| format!("读不了 {}：{e}", path.display()))?;
        Self::from_bytes(data)
    }

    pub fn from_bytes(data: Vec<u8>) -> Result<Self, String> {
        let bad = |why: &str| Err(format!("日语模型格式不对：{why}"));
        if data.len() < HEADER_SIZE {
            return bad("文件太短");
        }
        if &data[..8] != b"MSJPDT1\0" {
            return bad("魔数");
        }
        let version = le_u32(&data[8..]);
        let token_count = le_u32(&data[12..]);
        let connection_size = le_u32(&data[16..]);
        let token_offset = le_u64(&data[24..]);
        let connection_offset = le_u64(&data[32..]);
        let string_offset = le_u64(&data[40..]);
        let string_size = le_u64(&data[48..]);
        if version != 1 || connection_size == 0 || token_count > 2_000_000 || string_size > (1 << 32) {
            return bad("头部");
        }
        let size = data.len() as u64;
        let connection_count = u64::from(connection_size) * u64::from(connection_size);
        let contains = |offset: u64, length: u64| offset >= HEADER_SIZE as u64 && offset <= size && length <= size - offset;
        if connection_count > 20_000_000
            || !contains(token_offset, u64::from(token_count) * TOKEN_SIZE as u64)
            || !contains(connection_offset, connection_count * 2)
            || !contains(string_offset, string_size)
        {
            return bad("区段越界");
        }
        let decoder = Decoder {
            data,
            token_offset: token_offset as usize,
            connection_offset: connection_offset as usize,
            string_offset: string_offset as usize,
            token_count: token_count as usize,
            connection_size: connection_size as usize,
        };
        for index in 0..decoder.token_count {
            let token = decoder.token(index);
            if (token.reading_offset + token.reading_length) as u64 > string_size
                || (token.surface_offset + token.surface_length) as u64 > string_size
                || usize::from(token.left_id) >= decoder.connection_size
                || usize::from(token.right_id) >= decoder.connection_size
            {
                return bad("词条越界");
            }
            // 文件里的读音顺序就是检索用的索引；乱序的文件拒绝。
            if index > 0 && decoder.reading(&token) < decoder.reading(&decoder.token(index - 1)) {
                return bad("读音没有排序");
            }
        }
        if decoder.token_count == 0 {
            return bad("没有词条");
        }
        Ok(decoder)
    }

    pub fn token_count(&self) -> usize {
        self.token_count
    }

    fn token(&self, index: usize) -> Token {
        let b = &self.data[self.token_offset + index * TOKEN_SIZE..][..TOKEN_SIZE];
        Token {
            reading_offset: le_u32(&b[0..]) as usize,
            reading_length: usize::from(le_u16(&b[4..])),
            surface_offset: le_u32(&b[6..]) as usize,
            surface_length: usize::from(le_u16(&b[10..])),
            left_id: le_u16(&b[12..]),
            right_id: le_u16(&b[14..]),
            word_cost: le_u32(&b[16..]) as i32,
        }
    }

    fn reading(&self, token: &Token) -> &[u8] {
        &self.data[self.string_offset + token.reading_offset..][..token.reading_length]
    }

    fn surface(&self, token: &Token) -> &[u8] {
        &self.data[self.string_offset + token.surface_offset..][..token.surface_length]
    }

    fn lemma(&self, token_id: u32) -> Lemma {
        let token = self.token(token_id as usize);
        Lemma {
            reading: lossy(self.reading(&token)),
            surface: lossy(self.surface(&token)),
            left_id: token.left_id,
            right_id: token.right_id,
            word_cost: token.word_cost,
            token_id,
        }
    }

    /// 连接成本；任何一侧越界都按 10000 算。
    pub fn connection_cost(&self, right_id: u16, left_id: u16) -> i32 {
        let (right, left) = (usize::from(right_id), usize::from(left_id));
        if right >= self.connection_size || left >= self.connection_size {
            return 10000;
        }
        let at = self.connection_offset + (right * self.connection_size + left) * 2;
        i32::from(i16::from_le_bytes([self.data[at], self.data[at + 1]]))
    }

    fn lower_bound(&self, reading: &[u8]) -> usize {
        let (mut first, mut last) = (0, self.token_count);
        while first < last {
            let middle = first + (last - first) / 2;
            if self.reading(&self.token(middle)) < reading {
                first = middle + 1;
            } else {
                last = middle;
            }
        }
        first
    }

    /// 词条按成本从低到高、成本相同按下标从小到大，取前 `limit` 个。
    fn best_lemmas(&self, mut ids: Vec<u32>, limit: usize) -> Vec<Lemma> {
        let key = |id: &u32| (self.token(*id as usize).word_cost, *id);
        if ids.len() > limit {
            ids.select_nth_unstable_by_key(limit - 1, key);
            ids.truncate(limit);
        }
        ids.sort_by_key(key);
        ids.into_iter().map(|id| self.lemma(id)).collect()
    }

    pub fn exact_lemmas(&self, reading: &str, limit: usize) -> Vec<Lemma> {
        if reading.is_empty() || limit == 0 {
            return Vec::new();
        }
        let mut matches = Vec::new();
        for index in self.lower_bound(reading.as_bytes())..self.token_count {
            if self.reading(&self.token(index)) != reading.as_bytes() {
                break;
            }
            matches.push(index as u32);
        }
        self.best_lemmas(matches, limit)
    }

    pub fn prefix_lemmas(&self, prefix: &str, limit: usize) -> Vec<Lemma> {
        self.prefixed(prefix, limit, false)
    }

    /// 同 `prefix_lemmas`，但读音必须比前缀更长。
    pub fn longer_prefix_lemmas(&self, prefix: &str, limit: usize) -> Vec<Lemma> {
        self.prefixed(prefix, limit, true)
    }

    fn prefixed(&self, prefix: &str, limit: usize, strictly_longer: bool) -> Vec<Lemma> {
        if prefix.is_empty() || limit == 0 {
            return Vec::new();
        }
        let prefix = prefix.as_bytes();
        let mut matches = Vec::new();
        for index in self.lower_bound(prefix)..self.token_count {
            let token = self.token(index);
            let reading = self.reading(&token);
            if !reading.starts_with(prefix) {
                break;
            }
            if !strictly_longer || reading.len() > prefix.len() {
                matches.push(index as u32);
            }
        }
        self.best_lemmas(matches, limit)
    }

    /// 读音以 `prefix` 开头、后面紧跟 `next_kana` 之一的词条。
    ///
    /// 和 C++ 一样，遇到第一条读音不比前缀长的词条就停：只要词库里有读音恰好等于 `prefix` 的词条，
    /// 它们排在最前，这个函数就什么也不返回（疑似缺陷，见 README）。
    pub fn prefix_lemmas_continuing(&self, prefix: &str, next_kana: &[String], limit: usize) -> Vec<Lemma> {
        if prefix.is_empty() || next_kana.is_empty() || limit == 0 {
            return Vec::new();
        }
        let prefix = prefix.as_bytes();
        let mut matches = Vec::new();
        for index in self.lower_bound(prefix)..self.token_count {
            let token = self.token(index);
            let reading = self.reading(&token);
            if reading.len() <= prefix.len() || !reading.starts_with(prefix) {
                break;
            }
            let remaining = &reading[prefix.len()..];
            if next_kana.iter().any(|kana| remaining.starts_with(kana.as_bytes())) {
                matches.push(index as u32);
            }
        }
        self.best_lemmas(matches, limit)
    }

    /// 整句解码：按字节位置的网格，束宽取 max(16, 4×limit)。
    pub fn decode(&self, reading: &str, limit: usize) -> Vec<SentenceCandidate> {
        if reading.is_empty() || limit == 0 {
            return Vec::new();
        }
        struct Path {
            text: String,
            cost: i64,
            right_id: u16,
        }
        let boundaries = utf8_boundaries(reading);
        let mut paths: Vec<Vec<Path>> = (0..=reading.len()).map(|_| Vec::new()).collect();
        paths[0].push(Path { text: String::new(), cost: 0, right_id: 0 });
        let beam = 16.max(limit * 4);

        for boundary_index in 0..boundaries.len() - 1 {
            let start = boundaries[boundary_index];
            if paths[start].is_empty() {
                continue;
            }
            for end_index in boundary_index + 1..boundaries.len() {
                let end = boundaries[end_index];
                let key = &reading[start..end];
                for lemma in self.exact_lemmas(key, 24) {
                    let additions: Vec<Path> = paths[start]
                        .iter()
                        .map(|previous| Path {
                            text: format!("{}{}", previous.text, lemma.surface),
                            cost: previous.cost + i64::from(lemma.word_cost) + i64::from(self.connection_cost(previous.right_id, lemma.left_id)),
                            right_id: lemma.right_id,
                        })
                        .collect();
                    paths[end].extend(additions);
                }
            }

            let next = boundaries[boundary_index + 1];
            let kana = &reading[start..next];
            let additions: Vec<Path> = paths[start]
                .iter()
                .map(|previous| Path { text: format!("{}{}", previous.text, kana), cost: previous.cost + UNKNOWN_KANA_COST, right_id: 0 })
                .collect();
            paths[next].extend(additions);

            for end_index in boundary_index + 1..boundaries.len() {
                let bucket = &mut paths[boundaries[end_index]];
                if bucket.len() > beam {
                    partial_sort_by_cost(bucket, beam, |p| p.cost);
                    bucket.truncate(beam);
                }
            }
        }

        let mut finals = std::mem::take(&mut paths[reading.len()]);
        for path in &mut finals {
            path.cost += i64::from(self.connection_cost(path.right_id, 0));
        }
        finals.sort_by_key(|p| p.cost);
        let mut result = Vec::new();
        let mut seen = HashSet::new();
        for path in finals {
            if seen.insert(path.text.clone()) {
                result.push(SentenceCandidate { text: path.text, cost: path.cost });
            }
            if result.len() == limit {
                break;
            }
        }
        result
    }
}

/// 按代价从小到大的 `partial_sort`，见 [`crate::msvc_sort`]。
fn partial_sort_by_cost<T>(items: &mut Vec<T>, middle: usize, cost: impl Fn(&T) -> i64) {
    crate::msvc_sort::partial_sort_by(items, middle, |a, b| cost(a) < cost(b));
}

// ---------- 矩阵搜索 ----------

struct MatrixNode {
    text: String,
    cost: i64,
    right_id: u16,
}

fn append_unique(out: &mut Vec<SentenceCandidate>, seen: &mut HashSet<String>, text: String, cost: i64, limit: usize) {
    if text.is_empty() || out.len() >= limit {
        return;
    }
    if seen.insert(text.clone()) {
        out.push(SentenceCandidate { text, cost });
    }
}

pub fn search_converted(decoder: &Decoder, conversion: &Conversion, limit: usize) -> Vec<SentenceCandidate> {
    search_reading(decoder, &conversion.hiragana, &conversion.pending, limit)
}

pub fn search(decoder: &Decoder, romaji_input: &str, limit: usize) -> Vec<SentenceCandidate> {
    search_converted(decoder, &romaji::convert(romaji_input), limit)
}

pub fn search_reading(decoder: &Decoder, reading: &str, pending: &str, limit: usize) -> Vec<SentenceCandidate> {
    if limit == 0 {
        return Vec::new();
    }
    let pending_kana = romaji::kana_for_prefix(pending);
    if reading.is_empty() {
        let mut result = Vec::new();
        let mut seen = HashSet::new();
        for kana in &pending_kana {
            for lemma in decoder.prefix_lemmas(kana, 24) {
                append_unique(&mut result, &mut seen, lemma.surface, i64::from(lemma.word_cost), limit);
                if result.len() == limit {
                    return result;
                }
            }
        }
        return result;
    }

    let boundaries = utf8_boundaries(reading);
    let mora_count = boundaries.len() - 1;
    let mut rows: Vec<Vec<MatrixNode>> = (0..=mora_count).map(|_| Vec::new()).collect();
    rows[0].push(MatrixNode { text: String::new(), cost: 0, right_id: 0 });

    for start_mora in 0..mora_count {
        if rows[start_mora].is_empty() {
            continue;
        }
        let start_byte = boundaries[start_mora];
        let max_end = mora_count.min(start_mora + MAX_LEMMA_MORA);
        for end_mora in start_mora + 1..=max_end {
            let key = &reading[start_byte..boundaries[end_mora]];
            for lemma in decoder.exact_lemmas(key, 24) {
                let additions: Vec<MatrixNode> = rows[start_mora]
                    .iter()
                    .map(|previous| MatrixNode {
                        text: format!("{}{}", previous.text, lemma.surface),
                        cost: previous.cost + i64::from(lemma.word_cost) + i64::from(decoder.connection_cost(previous.right_id, lemma.left_id)),
                        right_id: lemma.right_id,
                    })
                    .collect();
                rows[end_mora].extend(additions);
            }
        }

        let next_mora = start_mora + 1;
        let kana = &reading[start_byte..boundaries[next_mora]];
        let additions: Vec<MatrixNode> = rows[start_mora]
            .iter()
            .map(|previous| MatrixNode { text: format!("{}{}", previous.text, kana), cost: previous.cost + UNKNOWN_KANA_COST, right_id: 0 })
            .collect();
        rows[next_mora].extend(additions);

        for end_mora in start_mora + 1..=max_end {
            let row = &mut rows[end_mora];
            if row.len() > MAX_NODE_A_ROW {
                partial_sort_by_cost(row, MAX_NODE_A_ROW, |n| n.cost);
                row.truncate(MAX_NODE_A_ROW);
            }
        }
    }

    let mut result = Vec::new();
    let mut seen = HashSet::new();
    let mut finals = std::mem::take(&mut rows[mora_count]);
    for node in &mut finals {
        node.cost += i64::from(decoder.connection_cost(node.right_id, 0));
    }
    finals.sort_by_key(|n| n.cost);
    let mut finals = finals.into_iter();
    if let Some(first) = finals.next() {
        append_unique(&mut result, &mut seen, first.text, first.cost, limit);
    }
    let rest: Vec<MatrixNode> = finals.collect();

    if !pending.is_empty() {
        for kana in &pending_kana {
            for lemma in decoder.exact_lemmas(&format!("{reading}{kana}"), 16) {
                append_unique(&mut result, &mut seen, lemma.surface, i64::from(lemma.word_cost), limit);
            }
        }
        for lemma in decoder.prefix_lemmas_continuing(reading, &pending_kana, 48) {
            append_unique(&mut result, &mut seen, lemma.surface, i64::from(lemma.word_cost), limit);
        }
    }

    for node in rest {
        append_unique(&mut result, &mut seen, node.text, node.cost, limit);
    }

    let mut lemma_mora = mora_count;
    while lemma_mora > 0 && result.len() < limit {
        for lemma in decoder.exact_lemmas(&reading[..boundaries[lemma_mora]], 16) {
            append_unique(&mut result, &mut seen, lemma.surface, i64::from(lemma.word_cost), limit);
            if result.len() == limit {
                break;
            }
        }
        lemma_mora -= 1;
    }
    result
}

// ---------- 候选提供者 ----------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Candidate {
    pub pinyin: String,
    pub canonical_pinyin: String,
    pub word: String,
    pub weight: i64,
    pub source: Source,
}

pub struct Provider {
    connection: Connection,
    decoder: Decoder,
}

fn escape_like_prefix(code: &str) -> String {
    let mut escaped = String::with_capacity(code.len() + 1);
    for ch in code.chars() {
        if matches!(ch, '%' | '_' | '#') {
            escaped.push('#');
        }
        escaped.push(ch);
    }
    escaped.push('%');
    escaped
}

fn append_candidate(items: &mut Vec<Candidate>, seen: &mut HashSet<String>, code: &str, value: &str, weight: i64, source: Source) {
    if !value.is_empty() && seen.insert(value.to_string()) {
        items.push(Candidate { pinyin: code.to_string(), canonical_pinyin: code.to_string(), word: value.to_string(), weight, source });
    }
}

impl Provider {
    pub fn open(db_path: &Path, model_path: &Path) -> Result<Self, String> {
        let connection = Connection::open_with_flags(db_path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_URI)
            .map_err(|e| format!("打不开词库 {}：{e}", db_path.display()))?;
        Ok(Self { connection, decoder: Decoder::open(model_path)? })
    }

    pub fn decoder(&self) -> &Decoder {
        &self.decoder
    }

    fn lexicon_rows(&self, with_cases: &str, raw: &str) -> Vec<(String, String, i64)> {
        let sql = "SELECT code, value, weight FROM japanese_lexicon \
                   WHERE code=?1 OR code=?2 OR code LIKE ?3 ESCAPE '#' OR code LIKE ?4 ESCAPE '#' \
                   ORDER BY weight DESC, rowid ASC LIMIT 64";
        let Ok(mut statement) = self.connection.prepare(sql) else { return Vec::new() };
        let like_raw = escape_like_prefix(raw);
        let like_q = escape_like_prefix(&format!("q{raw}"));
        let Ok(rows) = statement.query_map([with_cases, raw, like_raw.as_str(), like_q.as_str()], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, i64>(2)?))) else {
            return Vec::new();
        };
        rows.filter_map(Result::ok).collect()
    }

    /// `dynamic` 是 C++ 提供者按 `raw_input` 取到的云候选缓存（没有就传空）。
    pub fn query(&self, request: &Request, dynamic: &[Candidate]) -> Vec<Candidate> {
        if !request.valid {
            return Vec::new();
        }
        let mut candidates = Vec::new();
        let mut seen = HashSet::new();
        let cases = request.raw_input_with_cases.as_str();

        // 单独按 '-' 时给出两个候选：长音符 ー 在前，普通连字符在后。
        if request.raw_input == "-" {
            append_candidate(&mut candidates, &mut seen, cases, "ー", 1_000_000, Source::Generated);
            append_candidate(&mut candidates, &mut seen, cases, "-", 999_999, Source::Generated);
            return candidates;
        }

        let conversion = romaji::convert(&request.raw_input);
        let kana_first = romaji::is_single_kana(&conversion);
        if kana_first {
            append_candidate(&mut candidates, &mut seen, cases, &conversion.hiragana, 1_000_000, Source::Generated);
            append_candidate(&mut candidates, &mut seen, cases, &romaji::hiragana_to_katakana(&conversion.hiragana), 999_999, Source::Generated);
        }

        let pending_kana = romaji::kana_for_prefix(&conversion.pending);
        if !conversion.hiragana.is_empty() && !conversion.pending.is_empty() {
            for kana in &pending_kana {
                // 假名前缀已经和打下的读音对上，不用再转回某一种罗马字写法（那会拒掉 nn、n' 之类的别名）。
                for lemma in self.decoder.prefix_lemmas(&format!("{}{}", conversion.hiragana, kana), 24) {
                    append_candidate(&mut candidates, &mut seen, cases, &lemma.surface, 980_000 - i64::from(lemma.word_cost), Source::Database);
                }
            }
        }
        let sentences = search_converted(&self.decoder, &conversion, 12);
        let mut next_sentence = 0;
        // 读音打完之后先转换已经打下的，再联想更长的词：整句最佳路径和精确词条在前，然后是联想，
        // 最后是句子的其余部分。没打完的罗马字走上面“联想在前”的分支。
        if conversion.pending.is_empty() && conversion.hiragana.len() >= 6 {
            let mut weight_floor: i64 = 980_000;
            let mut append_exact = |candidates: &mut Vec<Candidate>, seen: &mut HashSet<String>, text: &str, weight: i64| {
                let before = candidates.len();
                append_candidate(candidates, seen, cases, text, weight, Source::Database);
                if candidates.len() != before {
                    weight_floor = weight_floor.min(weight);
                }
            };
            if let Some(first) = sentences.first() {
                append_exact(&mut candidates, &mut seen, &first.text, 900_000 - first.cost);
                next_sentence = 1;
            }
            for lemma in self.decoder.exact_lemmas(&conversion.hiragana, 16) {
                append_exact(&mut candidates, &mut seen, &lemma.surface, 900_000 - i64::from(lemma.word_cost));
            }
            // 联想的权重不能压过上面的转换结果，否则按权重重排会把次序翻回去。
            for lemma in self.decoder.longer_prefix_lemmas(&conversion.hiragana, 16) {
                append_candidate(&mut candidates, &mut seen, cases, &lemma.surface, weight_floor.min(980_000 - i64::from(lemma.word_cost)), Source::Database);
            }
        }
        for sentence in &sentences[next_sentence..] {
            append_candidate(&mut candidates, &mut seen, cases, &sentence.text, 900_000 - sentence.cost, Source::Database);
        }

        for (code, value, weight) in self.lexicon_rows(cases, &request.raw_input) {
            append_candidate(&mut candidates, &mut seen, &code, &value, weight, Source::Database);
        }

        if !conversion.hiragana.is_empty() && !kana_first {
            append_candidate(&mut candidates, &mut seen, cases, &conversion.hiragana, 1_000_000, Source::Generated);
            append_candidate(&mut candidates, &mut seen, cases, &romaji::hiragana_to_katakana(&conversion.hiragana), 999_999, Source::Generated);
        }

        let mut insertion = (if kana_first { 2 } else { 1 }).min(candidates.len());
        for item in dynamic {
            if seen.insert(item.word.clone()) {
                candidates.insert(insertion, item.clone());
                insertion += 1;
            }
        }
        candidates
    }
}
