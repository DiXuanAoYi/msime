//! kenlm 二进制语言模型的只读查询，只支持出货的 `sc.lm` 这一种格式：
//! `build_binary -s -a 22 -q 4 trie`，也就是量化加数组压缩指针的 trie（`QUANT_ARRAY_TRIE`，模型类型 5，
//! 搜索版本 1），词表是排好序的 64 位 MurmurHash64A，分数 log10。
//!
//! 对照 `engine/ngram/kenlm/lm/` 里的 `binary_format`、`model`、`search_trie`、`trie`、`bhiksha`、`quantize`、
//! `vocab` 和 `util/bit_packing` 写成，位压缩一律小端。打分按 `GenericModel::FullScore` 的顺序逐步累加 float，
//! 所以和 C++ 的结果逐位相同（`tests/kenlm_golden.rs` 对着真实的 `sc.lm` 核对）。
//!
//! 文件布局（偏移都是文件内的字节偏移，C++ 里是 mmap 的地址，起点页对齐，所以对齐规则等价）：
//! ```text
//! 头 Sanity(88) + 固定参数(20) + counts[order]*8，对齐到 8
//! 词表   u64 个数 + 个数*u64 哈希（升序，<unk> 不在里面，下标 = 位置 + 1）
//! 量化   8 字节头(版本 2, prob 位数, backoff 位数) + 每个中间阶 [prob 表, backoff 表] + 最高阶 [prob 表]
//! 一元   (counts[0] + 2) * { prob f32, backoff f32, next u64 }
//! 中间阶 每阶 [Bhiksha 数组(头 8 字节 + 偏移表) + 位压缩的 (词, backoff|prob, next) 数组]
//! 最高阶 位压缩的 (词, prob) 数组
//! ```

use std::path::Path;

const MAGIC: &[u8] = b"mmap lm http://kheafield.com/code format version 5\n\0";
const SANITY_SIZE: usize = 88;
const FIXED_SIZE: usize = 20;
const MODEL_TYPE_QUANT_ARRAY_TRIE: u32 = 5;
const SEARCH_VERSION: u32 = 1;
const QUANT_VERSION: u8 = 2;
const BHIKSHA_VERSION: u8 = 0;

/// 与 `lm/state.hh` 的 `State` 对应，最多 `MAX_STATE` 个词（`KENLM_MAX_ORDER - 1`，出货构建里是 2）。
pub const MAX_STATE: usize = 2;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct State {
    pub words: [u32; MAX_STATE],
    pub backoff: [f32; MAX_STATE],
    pub length: u8,
}

impl Default for State {
    fn default() -> Self {
        Self { words: [0; MAX_STATE], backoff: [0.0; MAX_STATE], length: 0 }
    }
}

/// `-0.0`：这个 n-gram 的 backoff 是零，且没有以它为前缀的更长 n-gram。符号位不同的 `0.0` 才表示有延伸。
fn has_extension(backoff: f32) -> bool {
    backoff.to_bits() != (-0.0f32).to_bits()
}

fn required_bits(max_value: u64) -> u8 {
    if max_value == 0 {
        0
    } else {
        (64 - max_value.leading_zeros()) as u8
    }
}

fn murmur_hash64a(key: &[u8], seed: u64) -> u64 {
    const M: u64 = 0xc6a4a7935bd1e995;
    const R: u32 = 47;
    let mut h = seed ^ (key.len() as u64).wrapping_mul(M);
    let mut chunks = key.chunks_exact(8);
    for chunk in &mut chunks {
        let mut k = u64::from_le_bytes(chunk.try_into().unwrap());
        k = k.wrapping_mul(M);
        k ^= k >> R;
        k = k.wrapping_mul(M);
        h ^= k;
        h = h.wrapping_mul(M);
    }
    let rest = chunks.remainder();
    if !rest.is_empty() {
        for (i, byte) in rest.iter().enumerate() {
            h ^= u64::from(*byte) << (8 * i);
        }
        h = h.wrapping_mul(M);
    }
    h ^= h >> R;
    h = h.wrapping_mul(M);
    h ^= h >> R;
    h
}

fn align8(offset: usize) -> usize {
    (offset + 7) & !7
}

/// 一个位压缩数组（中间阶或最高阶）。
#[derive(Debug, Clone)]
struct Packed {
    /// 数据区的起点（文件偏移）。
    base: usize,
    word_bits: u8,
    total_bits: u8,
    word_mask: u64,
}

impl Packed {
    fn new(base: usize, max_vocab: u64, remaining_bits: u8) -> Self {
        let word_bits = required_bits(max_vocab);
        Self { base, word_bits, total_bits: word_bits + remaining_bits, word_mask: (1u64 << word_bits) - 1 }
    }

    fn size(entries: u64, max_vocab: u64, remaining_bits: u8) -> usize {
        let total_bits = u64::from(required_bits(max_vocab) + remaining_bits);
        (((1 + entries) * total_bits + 7) / 8 + 8) as usize
    }
}

/// 中间阶：词、量化的 backoff|prob、以及指向下一阶的 `next`（数组压缩）。
#[derive(Debug, Clone)]
struct Middle {
    packed: Packed,
    quant_bits: u8,
    /// Bhiksha 内联位数（`next` 里没被偏移表吃掉的低位）。
    inline_bits: u8,
    inline_mask: u64,
    /// Bhiksha 偏移表：文件偏移与个数。
    offsets_at: usize,
    offsets_len: usize,
}

/// 一阶的 n-gram 范围 [begin, end)，下标是这一阶位压缩数组里的项号。
#[derive(Debug, Clone, Copy, Default)]
struct Range {
    begin: u64,
    end: u64,
}

pub struct KenLm {
    data: Vec<u8>,
    order: usize,
    vocab_len: usize,
    vocab_at: usize,
    prob_bits: u8,
    backoff_bits: u8,
    /// 每个中间阶 [prob 表, backoff 表] 的文件偏移，再加最高阶的 prob 表。
    tables: Vec<(usize, usize)>,
    longest_table_at: usize,
    unigram_at: usize,
    middles: Vec<Middle>,
    longest: Packed,
    begin_sentence: State,
    null_context: State,
    unknown_penalty: f32,
}

/// `lm::ngram::detail::GenericModel` 里 `FullScoreReturn` 的有用部分。
#[derive(Default, Clone, Copy)]
struct Score {
    prob: f32,
    ngram_length: u8,
    independent_left: bool,
}

impl KenLm {
    /// 与 C++ `LanguageModel` 的默认罚分取同一个值：`log10(1/6e7)`，按 float 算。
    pub fn default_unknown_penalty() -> f32 {
        (1.0f32 / 60_000_000.0f32).log10()
    }

    pub fn open(path: &Path) -> Result<Self, String> {
        let data = std::fs::read(path).map_err(|e| format!("读不了 {}：{e}", path.display()))?;
        Self::from_bytes(data)
    }

    pub fn from_bytes(data: Vec<u8>) -> Result<Self, String> {
        if data.len() <= SANITY_SIZE + FIXED_SIZE || &data[..MAGIC.len()] != MAGIC {
            return Err("不是 kenlm 二进制模型（魔数不对）".into());
        }
        let f32_at = |at: usize| f32::from_le_bytes(data[at..at + 4].try_into().unwrap());
        let u32_at = |at: usize| u32::from_le_bytes(data[at..at + 4].try_into().unwrap());
        let u64_at = |at: usize| u64::from_le_bytes(data[at..at + 8].try_into().unwrap());
        // 魔数之后的自检值：0.0、1.0、-0.5、一、最大词下标、一（u64）。
        if (f32_at(56), f32_at(60), f32_at(64)) != (0.0, 1.0, -0.5) || u32_at(68) != 1 || u32_at(72) != u32::MAX || u64_at(80) != 1 {
            return Err("自检值不对：这份文件不是用同样的字节序/对齐建的".into());
        }
        let order = data[SANITY_SIZE] as usize;
        let model_type = u32_at(SANITY_SIZE + 8);
        let search_version = u32_at(SANITY_SIZE + 16);
        if model_type != MODEL_TYPE_QUANT_ARRAY_TRIE || search_version != SEARCH_VERSION {
            return Err(format!("只支持量化数组 trie（类型 5，版本 1），这份是类型 {model_type} 版本 {search_version}"));
        }
        if !(2..=3).contains(&order) {
            return Err(format!("阶数 {order} 不支持（出货构建的 KENLM_MAX_ORDER 是 3）"));
        }
        let counts: Vec<u64> = (0..order).map(|i| u64_at(SANITY_SIZE + FIXED_SIZE + 8 * i)).collect();
        let header = align8(SANITY_SIZE + FIXED_SIZE + 8 * order);

        // 词表
        let vocab_at = header;
        let vocab_len = u64_at(vocab_at) as usize;
        let vocab_size = 8 + 8 * counts[0] as usize;
        let search = header + vocab_size;

        // 量化表
        if data[search] != QUANT_VERSION {
            return Err(format!("量化版本 {} 不认识（期望 {QUANT_VERSION}）", data[search]));
        }
        let (prob_bits, backoff_bits) = (data[search + 1], data[search + 2]);
        if prob_bits == 0 || backoff_bits == 0 || prob_bits > 25 || backoff_bits > 25 {
            return Err("量化位数不合法".into());
        }
        let (prob_table, backoff_table) = (4usize << prob_bits, 4usize << backoff_bits);
        let mut tables = Vec::new();
        let mut cursor = search + 8;
        for _ in 0..order - 2 {
            tables.push((cursor, cursor + prob_table));
            cursor += prob_table + backoff_table;
        }
        let longest_table_at = cursor;
        let quant_size = (order - 2) * (backoff_table + prob_table) + prob_table + 8;

        // 一元
        let unigram_at = search + quant_size;
        let unigram_size = (counts[0] as usize + 2) * 16;

        // 中间阶与最高阶
        let first_middle = unigram_at + unigram_size;
        let bhiksha_bits = if order > 2 {
            if data[first_middle] != BHIKSHA_VERSION {
                return Err(format!("数组压缩版本 {} 不认识", data[first_middle]));
            }
            data[first_middle + 1]
        } else {
            0
        };
        let mut middles = Vec::new();
        let mut start = first_middle;
        for i in 2..order {
            let entries = counts[i - 1];
            let max_next = counts[i];
            let max_offset = entries + 1;
            // ChopBits / ArrayCount / InlineBits
            let required = required_bits(max_next);
            let mut best_chop = 0u8;
            let mut lowest = i64::MAX;
            for chop in 0..=required.min(bhiksha_bits) {
                let change = ((max_next >> (required - chop)) as i64) * 64 - (max_offset as i64) * i64::from(chop);
                if change < lowest {
                    lowest = change;
                    best_chop = chop;
                }
            }
            let array_count = ((max_next >> (required - best_chop)) + 1) as usize;
            let bhiksha_size = 8 * (1 + array_count) + 7;
            let inline_bits = required - best_chop;
            let quant_bits = prob_bits + backoff_bits;
            let remaining = quant_bits + inline_bits;
            let aligned = align8(start);
            let packed = Packed::new(start + bhiksha_size, counts[0], remaining);
            middles.push(Middle {
                packed,
                quant_bits,
                inline_bits,
                inline_mask: (1u64 << inline_bits) - 1,
                offsets_at: aligned + 8,
                offsets_len: array_count,
            });
            start += bhiksha_size + Packed::size(entries, counts[0], remaining);
        }
        let longest = Packed::new(start, counts[0], prob_bits);
        let end = start + Packed::size(counts[order - 1], counts[0], prob_bits);
        if data.len() < end {
            return Err(format!("文件只有 {} 字节，头里说至少要 {end}", data.len()));
        }

        let mut model = Self {
            data,
            order,
            vocab_len,
            vocab_at,
            prob_bits,
            backoff_bits,
            tables,
            longest_table_at,
            unigram_at,
            middles,
            longest,
            begin_sentence: State::default(),
            null_context: State::default(),
            unknown_penalty: Self::default_unknown_penalty(),
        };
        // 句首状态：一个词 <s>，backoff 取它的一元 backoff。
        let begin_word = model.index("<s>");
        let mut begin = State { length: 1, ..State::default() };
        begin.words[0] = begin_word;
        begin.backoff[0] = model.unigram(begin_word).1;
        model.begin_sentence = begin;
        Ok(model)
    }

    pub fn order(&self) -> usize {
        self.order
    }

    pub fn unknown_penalty(&self) -> f32 {
        self.unknown_penalty
    }

    pub fn set_unknown_penalty(&mut self, penalty: f32) {
        self.unknown_penalty = penalty;
    }

    pub fn begin_state(&self) -> State {
        self.begin_sentence
    }

    pub fn null_state(&self) -> State {
        self.null_context
    }

    pub fn is_unknown(&self, index: u32) -> bool {
        index == 0
    }

    /// 词的下标，不认识是 0（`<unk>`）。
    pub fn index(&self, word: &str) -> u32 {
        let hash = murmur_hash64a(word.as_bytes(), 0);
        let (mut low, mut high) = (0usize, self.vocab_len);
        while low < high {
            let mid = (low + high) / 2;
            let value = self.read_u64(self.vocab_at + 8 + 8 * mid);
            match value.cmp(&hash) {
                std::cmp::Ordering::Equal => return mid as u32 + 1,
                std::cmp::Ordering::Less => low = mid + 1,
                std::cmp::Ordering::Greater => high = mid,
            }
        }
        0
    }

    /// `ngram::LanguageModel::score`：log10 概率，未登录词再加罚分。
    pub fn score(&self, state: &State, index: u32) -> (f32, State) {
        let (prob, out) = self.full_score(state, index);
        (prob + if self.is_unknown(index) { self.unknown_penalty } else { 0.0 }, out)
    }

    pub fn score_word(&self, state: &State, word: &str) -> (f32, State) {
        self.score(state, self.index(word))
    }

    // ---------------------------------------------------------------------------------------
    // 打分
    // ---------------------------------------------------------------------------------------

    fn full_score(&self, in_state: &State, new_word: u32) -> (f32, State) {
        let mut out = State::default();
        let context = &in_state.words[..in_state.length as usize];
        let mut ret = self.score_except_backoff(context, new_word, &mut out);
        for i in (ret.ngram_length as usize - 1)..in_state.length as usize {
            ret.prob += in_state.backoff[i];
        }
        (ret.prob, out)
    }

    fn score_except_backoff(&self, context: &[u32], new_word: u32, out: &mut State) -> Score {
        let mut ret = Score { ngram_length: 1, ..Score::default() };
        let (prob, backoff, mut node) = self.unigram_with_range(new_word);
        out.backoff[0] = backoff;
        ret.prob = prob;
        ret.independent_left = node.begin == node.end;
        out.length = u8::from(has_extension(backoff));
        out.words[0] = new_word;
        if context.is_empty() {
            return ret;
        }
        self.resume_score(context, &mut node, &mut out.backoff[1..], &mut out.length, &mut ret);
        // CopyRemainingHistory：新词已经写好，其余历史从 context 抄 length - 1 个。
        for i in 0..(out.length as usize).saturating_sub(1) {
            out.words[1 + i] = context[i];
        }
        ret
    }

    fn resume_score(&self, context: &[u32], node: &mut Range, backoff_out: &mut [f32], next_use: &mut u8, ret: &mut Score) {
        let mut order_minus_2 = 0usize;
        let mut position = 0usize;
        loop {
            if position == context.len() || ret.independent_left {
                return;
            }
            if order_minus_2 == self.order - 2 {
                break;
            }
            let Some((prob, backoff, independent_left)) = self.lookup_middle(order_minus_2, context[position], node) else {
                return;
            };
            ret.independent_left = independent_left;
            backoff_out[position] = backoff;
            ret.prob = prob;
            ret.ngram_length = order_minus_2 as u8 + 2;
            if has_extension(backoff) {
                *next_use = ret.ngram_length;
            }
            order_minus_2 += 1;
            position += 1;
        }
        ret.independent_left = true;
        if let Some(prob) = self.lookup_longest(context[position], node) {
            ret.prob = prob;
            ret.ngram_length = self.order as u8;
        }
    }

    // ---------------------------------------------------------------------------------------
    // 数据结构查询
    // ---------------------------------------------------------------------------------------

    fn read_u64(&self, at: usize) -> u64 {
        u64::from_le_bytes(self.data[at..at + 8].try_into().unwrap())
    }

    fn read_f32(&self, at: usize) -> f32 {
        f32::from_le_bytes(self.data[at..at + 4].try_into().unwrap())
    }

    /// `util::ReadInt57`：从比特偏移处读至多 57 位。数据区尾部有 8 字节垫底，越界的字节按 0 补。
    fn read_int57(&self, bit_offset: u64, mask: u64) -> u64 {
        let at = (bit_offset >> 3) as usize;
        let mut buffer = [0u8; 8];
        let available = self.data.len().saturating_sub(at).min(8);
        buffer[..available].copy_from_slice(&self.data[at..at + available]);
        (u64::from_le_bytes(buffer) >> (bit_offset & 7)) & mask
    }

    fn read_int25(&self, bit_offset: u64, mask: u32) -> u32 {
        let at = (bit_offset >> 3) as usize;
        let mut buffer = [0u8; 4];
        let available = self.data.len().saturating_sub(at).min(4);
        buffer[..available].copy_from_slice(&self.data[at..at + available]);
        (u32::from_le_bytes(buffer) >> (bit_offset & 7)) & mask
    }

    /// 一元的 prob、backoff。
    fn unigram(&self, word: u32) -> (f32, f32) {
        let at = self.unigram_at + 16 * word as usize;
        (self.read_f32(at), self.read_f32(at + 4))
    }

    fn unigram_with_range(&self, word: u32) -> (f32, f32, Range) {
        let at = self.unigram_at + 16 * word as usize;
        let range = Range { begin: self.read_u64(at + 8), end: self.read_u64(at + 24) };
        (self.read_f32(at), self.read_f32(at + 4), range)
    }

    /// 在 `packed` 的 [range.begin, range.end) 里找 `key`（项按词升序、无重复），返回项号。
    fn find(&self, packed: &Packed, range: Range, key: u32) -> Option<u64> {
        let key = u64::from(key);
        let (mut low, mut high) = (range.begin, range.end);
        while low < high {
            let mid = low + (high - low) / 2;
            let value = self.read_int57(packed.base as u64 * 8 + mid * u64::from(packed.total_bits), packed.word_mask);
            match value.cmp(&key) {
                std::cmp::Ordering::Equal => return Some(mid),
                std::cmp::Ordering::Less => low = mid + 1,
                std::cmp::Ordering::Greater => high = mid,
            }
        }
        None
    }

    /// `ArrayBhiksha::ReadNext`：第 `index` 项的 next 范围。`bit_offset` 是这一项的 next 内联位的起点。
    fn read_next(&self, middle: &Middle, bit_offset: u64, index: u64) -> Range {
        let offsets = |i: usize| self.read_u64(middle.offsets_at + 8 * i);
        // upper_bound(index) - 1：最后一个 <= index 的位置。
        let (mut low, mut high) = (0usize, middle.offsets_len);
        while low < high {
            let mid = (low + high) / 2;
            if offsets(mid) <= index {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        let begin_index = low - 1;
        let mut end_index = begin_index + 1;
        while end_index < middle.offsets_len && offsets(end_index) <= index + 1 {
            end_index += 1;
        }
        end_index -= 1;
        let total = u64::from(middle.packed.total_bits);
        Range {
            begin: ((begin_index as u64) << middle.inline_bits) | self.read_int57(bit_offset, middle.inline_mask),
            end: ((end_index as u64) << middle.inline_bits) | self.read_int57(bit_offset + total, middle.inline_mask),
        }
    }

    /// 中间阶查询：找到时返回 (prob, backoff, independent_left)，并把 `node` 更新成这个 n-gram 的延伸范围。
    fn lookup_middle(&self, order_minus_2: usize, word: u32, node: &mut Range) -> Option<(f32, f32, bool)> {
        let middle = &self.middles[order_minus_2];
        let index = self.find(&middle.packed, *node, word)?;
        let total = u64::from(middle.packed.total_bits);
        let word_bits = u64::from(middle.packed.word_bits);
        let entry = middle.packed.base as u64 * 8 + index * total + word_bits;
        *node = self.read_next(middle, entry + u64::from(middle.quant_bits), index);
        let (prob_table, backoff_table) = self.tables[order_minus_2];
        let backoff_mask = (1u32 << self.backoff_bits) - 1;
        let prob_mask = (1u32 << self.prob_bits) - 1;
        let backoff = self.read_f32(backoff_table + 4 * self.read_int25(entry, backoff_mask) as usize);
        let prob = self.read_f32(prob_table + 4 * self.read_int25(entry + u64::from(self.backoff_bits), prob_mask) as usize);
        Some((prob, backoff, node.begin == node.end))
    }

    fn lookup_longest(&self, word: u32, node: &Range) -> Option<f32> {
        let index = self.find(&self.longest, *node, word)?;
        let entry = self.longest.base as u64 * 8 + index * u64::from(self.longest.total_bits) + u64::from(self.longest.word_bits);
        let prob_mask = (1u32 << self.prob_bits) - 1;
        Some(self.read_f32(self.longest_table_at + 4 * self.read_int25(entry, prob_mask) as usize))
    }
}

/// 把 [`KenLm`] 接给词格：[`crate::lattice::LanguageModel`] 的状态是不透明的 `Vec<u32>`，
/// 这里编成 `[length, words.., backoff 的位模式..]`。
impl crate::lattice::LanguageModel for KenLm {
    fn is_valid(&self) -> bool {
        true
    }

    fn null_state(&self) -> crate::lattice::ModelState {
        encode_state(&KenLm::null_state(self))
    }

    fn index(&self, word: &str) -> u32 {
        KenLm::index(self, word)
    }

    fn score(&self, state: &crate::lattice::ModelState, index: u32) -> (f32, crate::lattice::ModelState) {
        let (score, next) = KenLm::score(self, &decode_state(state), index);
        (score, encode_state(&next))
    }
}

fn encode_state(state: &State) -> Vec<u32> {
    let mut encoded = vec![u32::from(state.length)];
    encoded.extend(state.words);
    encoded.extend(state.backoff.iter().map(|b| b.to_bits()));
    encoded
}

fn decode_state(encoded: &[u32]) -> State {
    let mut state = State::default();
    if encoded.len() == 1 + 2 * MAX_STATE {
        state.length = encoded[0] as u8;
        state.words.copy_from_slice(&encoded[1..1 + MAX_STATE]);
        for (slot, bits) in state.backoff.iter_mut().zip(&encoded[1 + MAX_STATE..]) {
            *slot = f32::from_bits(*bits);
        }
    }
    state
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn required_bits_matches_the_cpp_helper() {
        assert_eq!([0, 1, 2, 3, 4, 7, 8, 255, 256].map(required_bits), [0, 1, 2, 2, 3, 3, 4, 8, 9]);
    }

    #[test]
    fn murmur_hash_is_the_64a_variant_with_seed_zero() {
        // 空串：seed ^ (0 * m) = 0，三步混合之后仍是 0。
        assert_eq!(murmur_hash64a(b"", 0), 0);
        // 同一个输入稳定，不同输入不同。
        assert_eq!(murmur_hash64a("你好".as_bytes(), 0), murmur_hash64a("你好".as_bytes(), 0));
        assert_ne!(murmur_hash64a("你好".as_bytes(), 0), murmur_hash64a("你们".as_bytes(), 0));
        // 8 字节整块与余数两条路径都要走到。
        assert_ne!(murmur_hash64a(b"abcdefgh", 0), murmur_hash64a(b"abcdefgi", 0));
        assert_ne!(murmur_hash64a(b"abcdefghi", 0), murmur_hash64a(b"abcdefgh", 0));
    }

    #[test]
    fn the_no_extension_backoff_is_negative_zero() {
        assert!(!has_extension(-0.0));
        assert!(has_extension(0.0));
        assert!(has_extension(-0.5));
    }

    #[test]
    fn garbage_is_rejected_instead_of_panicking() {
        assert!(KenLm::from_bytes(vec![0; 10]).is_err());
        assert!(KenLm::from_bytes(vec![0; 4096]).is_err());
        let mut almost = MAGIC.to_vec();
        almost.resize(4096, 0);
        assert!(KenLm::from_bytes(almost).is_err(), "魔数对但自检值不对");
    }

    #[test]
    fn the_lattice_state_encoding_round_trips() {
        let state = State { words: [7, 9], backoff: [-0.0, -1.25], length: 2 };
        let back = decode_state(&encode_state(&state));
        assert_eq!(back.length, 2);
        assert_eq!(back.words, [7, 9]);
        assert_eq!(back.backoff[0].to_bits(), (-0.0f32).to_bits(), "-0.0 的符号位表示“没有延伸”，不能丢");
        assert_eq!(back.backoff[1], -1.25);
        assert_eq!(decode_state(&[]).length, 0, "空状态就是无上下文");
    }

    #[test]
    fn the_default_unknown_penalty_is_log10_of_one_over_sixty_million() {
        let penalty = KenLm::default_unknown_penalty();
        assert!((penalty + 7.77815).abs() < 1e-4, "{penalty}");
    }
}
