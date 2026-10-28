//! Google 拼音解码器（`engine/googlepinyinime-rev`）的整句解码，对应 C++ 的 `PinyinDecoder::sentence`。
//!
//! 搬的是 `dict_pinyin.dat` 的载入（音节树、单字表和词表、词树、一元分数码本）和 `MatrixSearch` 里
//! 一次完整搜索的那一段：逐字符建词图矩阵、取最后一行得分最低的路径拼成句子。句中辅助码的字符约束也在
//! 矩阵里生效（`filter_lpis_by_constraints`）。
//!
//! 和 C++ 的区别：
//! - 每次调用都从头解，不做上一次输入的公共前缀复用（C++ 的复用只是少解几个字母，结果用探针逐条对过）。
//! - 用户词库当成空的：适配层从不调 `choose` / `put_lemma`，用户词库里不会有词。
//! - 候选列表（`prepare_candidates`）和预测不搬：适配层只取第 0 个候选，也就是矩阵里的最佳句子。
//! - 定长池（矩阵节点 640、词典匹配 2560、里程碑 1024、解析标记 8192）的上限和满了以后的行为都照搬。
//!
//! 排序用 MSVC 的 `qsort`（`msvc_sort::qsort_by`），同分候选的次序跟 C++ 基线一致。

use crate::msvc_sort::qsort_by;
use std::path::Path;
use std::sync::Mutex;

const HALF_ID_COUNT: usize = 29;
const FULL_ID_START: u16 = (HALF_ID_COUNT + 1) as u16;
const MAX_LEMMA_SIZE: usize = 8;
const MAX_PINYIN_SIZE: usize = 6;
const MAX_ROW_NUM: usize = 128;
const MAX_NODE_A_ROW: usize = 5;
const MTRX_ND_POOL_SIZE: usize = MAX_ROW_NUM * MAX_NODE_A_ROW;
const DMI_POOL_SIZE: usize = MAX_ROW_NUM * 20;
const MAX_LMA_PSB_ITEMS: usize = 1450;
const MAX_LPI_CACHE_PER_ID: usize = 15;
const MAX_MILESTONE: usize = MAX_ROW_NUM * 8;
const MAX_PARSING_MARK: usize = MAX_ROW_NUM * 64;
const SYS_DICT_ID_END: usize = 500_000;
const MAX_HZS_LEN: usize = 64;
const PRUNING_SCORE: f64 = 8000.0;
const NO_POS: u16 = u16::MAX;
const HALF_ID_CHARS: &[u8; 30] = b"0ABCcDEFGHIJKLMNOPQRSsTUVWXYZz";
/// `A`..`Z` 各自是声母(1)、韵母(2)；`szm_enable_*` 默认全开，所以声母韵母都算“可作首字母”。
const CHAR_FLAGS: [u8; 26] = [2, 1, 1, 1, 2, 1, 1, 1, 0, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1];

struct Reader<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        let end = self.pos.checked_add(n)?;
        let slice = self.data.get(self.pos..end)?;
        self.pos = end;
        Some(slice)
    }
    fn u8(&mut self) -> Option<u8> {
        self.bytes(1).map(|b| b[0])
    }
    fn u32(&mut self) -> Option<u32> {
        self.bytes(4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
}

fn is_valid_spl_char(ch: u8) -> bool {
    ch.is_ascii_alphabetic()
}

fn is_same_spl_char(a: u8, b: u8) -> bool {
    a == b || a.wrapping_sub(b) == 32 || b.wrapping_sub(a) == 32
}

// ---------------------------------------------------------------- 音节树

#[derive(Clone, Copy)]
struct SpellingNode {
    first_son: usize,
    num_of_son: u16,
    spelling_idx: u16,
    ch: u8,
}

struct SpellingTrie {
    spelling_num: usize,
    nodes: Vec<SpellingNode>,
    level1: [Option<usize>; 26],
    h2f_start: [u16; 30],
    h2f_num: [u16; 30],
}

struct Builder<'a> {
    nodes: Vec<SpellingNode>,
    level1: [Option<usize>; 26],
    h2f_start: [u16; 30],
    h2f_num: [u16; 30],
    buf: &'a [u8],
    size: usize,
}

impl Builder<'_> {
    fn at(&self, item: usize, level: usize) -> u8 {
        self.buf.get(item * self.size + level).copied().unwrap_or(0)
    }

    /// `SpellingTrie::construct_spellings_subset`：[item_start, item_end) 这些音节在第 level 位上分叉。
    fn subset(&mut self, item_start: usize, item_end: usize, level: usize, parent: usize) -> Option<usize> {
        if level >= self.size || item_end <= item_start {
            return None;
        }
        let mut num_of_son = 0usize;
        let mut ch = self.at(item_start, level);
        for i in item_start + 1..item_end {
            let c = self.at(i, level);
            if c != ch {
                num_of_son += 1;
                ch = c;
            }
        }
        num_of_son += 1;
        let first_son = self.nodes.len();
        self.nodes.resize(first_son + num_of_son, SpellingNode { first_son: usize::MAX, num_of_son: 0, spelling_idx: 0, ch: 0 });
        let mut son_pos = 0usize;
        let mut item_start_next = item_start;
        let mut char_for_node = self.at(item_start, level);
        for i in item_start + 1..item_end {
            let current = self.at(i, level);
            if current != char_for_node {
                self.finish(first_son + son_pos, level, char_for_node, item_start_next, i);
                char_for_node = current;
                item_start_next = i;
                son_pos += 1;
            }
        }
        self.finish(first_son + son_pos, level, char_for_node, item_start_next, item_end);
        self.nodes[parent].num_of_son = num_of_son as u16;
        Some(first_son)
    }

    fn finish(&mut self, node: usize, level: usize, ch: u8, item_start_next: usize, end: usize) {
        self.nodes[node].ch = ch;
        if level == 0 && ch.is_ascii_uppercase() {
            self.level1[(ch - b'A') as usize] = Some(node);
        }
        if self.at(item_start_next, level + 1) == 0 {
            self.nodes[node].spelling_idx = FULL_ID_START.wrapping_add(item_start_next as u16);
        }
        if self.at(item_start_next, level + 1) != 0 || end - item_start_next > 1 {
            let mut real_start = item_start_next;
            if self.at(item_start_next, level + 1) == 0 {
                real_start += 1;
            }
            if let Some(son) = self.subset(real_start, end, level + 1, node) {
                self.nodes[node].first_son = son;
            }
        }
        let mut half = 0usize;
        if level == 0 && ch.is_ascii_uppercase() && CHAR_FLAGS[(ch - b'A') as usize] != 0 {
            let mut id = (ch - b'A') as usize + 1;
            if ch > b'C' {
                id += 1;
            }
            if ch > b'S' {
                id += 1;
            }
            half = id;
        } else if level == 1 && ch == b'h' {
            half = match self.at(item_start_next, 0) {
                b'C' => (b'C' - b'A') as usize + 1 + 1,
                b'S' => (b'S' - b'A') as usize + 1 + 2,
                b'Z' => (b'Z' - b'A') as usize + 1 + 3,
                _ => 0,
            };
        }
        if half != 0 && half < 30 {
            self.nodes[node].spelling_idx = half as u16;
            self.h2f_num[half] = (end - item_start_next) as u16;
            self.h2f_start[half] = if self.h2f_num[half] > 0 { (item_start_next as u16).wrapping_add(FULL_ID_START) } else { 0 };
        }
    }
}

impl SpellingTrie {
    fn load(reader: &mut Reader) -> Option<Self> {
        let size = reader.u32()? as usize;
        let num = reader.u32()? as usize;
        reader.bytes(4)?; // score_amplifier
        reader.u8()?; // average_score
        if size < 2 || size > 64 || num == 0 || num > 4096 {
            return None;
        }
        let raw = reader.bytes(size.checked_mul(num)?)?;
        let mut rows: Vec<&[u8]> = raw.chunks(size).collect();
        let cstr = |row: &[u8]| -> Vec<u8> { row.iter().copied().take_while(|&b| b != 0).collect() };
        rows.sort_by(|a, b| cstr(a).cmp(&cstr(b)));
        let sorted: Vec<u8> = rows.concat();
        let mut builder = Builder {
            nodes: vec![SpellingNode { first_son: usize::MAX, num_of_son: 0, spelling_idx: 0, ch: 0 }],
            level1: [None; 26],
            h2f_start: [0; 30],
            h2f_num: [0; 30],
            buf: &sorted,
            size,
        };
        let first = builder.subset(0, num, 0, 0)?;
        builder.nodes[0].first_son = first;
        Some(SpellingTrie { spelling_num: num, nodes: builder.nodes, level1: builder.level1, h2f_start: builder.h2f_start, h2f_num: builder.h2f_num })
    }

    fn is_half_id(&self, id: u16) -> bool {
        id != 0 && id < FULL_ID_START
    }

    fn is_half_id_yunmu(&self, id: u16) -> bool {
        if id == 0 || id >= FULL_ID_START {
            return false;
        }
        let ch = HALF_ID_CHARS[id as usize];
        ch < b'a' && CHAR_FLAGS[(ch - b'A') as usize] & 2 != 0
    }

    /// 返回这个半音节对应多少个全音节，并把第一个全音节的编号写进 `start`。
    fn half_to_full(&self, half: u16, start: &mut u16) -> u16 {
        if half >= FULL_ID_START {
            return 0;
        }
        *start = self.h2f_start[half as usize];
        self.h2f_num[half as usize]
    }

    fn if_valid_id_update(&self, id: &mut u16) -> bool {
        if *id == 0 {
            return false;
        }
        if *id >= FULL_ID_START {
            return true;
        }
        let ch = HALF_ID_CHARS[*id as usize];
        if ch > b'Z' {
            return true;
        }
        let flags = CHAR_FLAGS[(ch - b'A') as usize];
        if flags != 0 {
            return true;
        }
        if flags & 2 != 0 {
            *id = self.h2f_start[*id as usize];
            return true;
        }
        false
    }

    /// `SpellingParser::splstr_to_idxs`：返回 (音节编号, 每个音节的起点, 最后一个是不是还没打完)。
    fn splstr_to_idxs(&self, s: &[u8], max_size: usize) -> (Vec<u16>, Vec<usize>, bool) {
        let mut idx: Vec<u16> = Vec::new();
        let mut start = vec![0usize];
        if s.is_empty() || max_size == 0 || !is_valid_spl_char(s[0]) {
            return (idx, start, false);
        }
        let mut node = 0usize;
        let mut pos = 0usize;
        let mut last_is_splitter = false;
        while pos < s.len() {
            let ch = s[pos];
            if !is_valid_spl_char(ch) {
                let mut id = self.nodes[node].spelling_idx;
                if self.if_valid_id_update(&mut id) {
                    idx.push(id);
                    pos += 1;
                    start.push(pos);
                    if idx.len() >= max_size {
                        return (idx, start, false);
                    }
                    node = 0;
                    last_is_splitter = true;
                    continue;
                } else if last_is_splitter {
                    pos += 1;
                    *start.last_mut().expect("起点表不会空") = pos;
                    continue;
                } else {
                    return (idx, start, false);
                }
            }
            last_is_splitter = false;
            let found = if pos == 0 {
                self.level1[(ch.to_ascii_uppercase() - b'A') as usize]
            } else {
                let n = self.nodes[node];
                (0..n.num_of_son as usize).map(|i| n.first_son + i).find(|&son| is_same_spl_char(self.nodes[son].ch, ch))
            };
            if let Some(son) = found {
                node = son;
            } else {
                let mut id = self.nodes[node].spelling_idx;
                if self.if_valid_id_update(&mut id) {
                    idx.push(id);
                    start.push(pos);
                    if idx.len() >= max_size {
                        return (idx, start, false);
                    }
                    node = 0;
                    continue;
                }
                return (idx, start, false);
            }
            pos += 1;
        }
        let mut id = self.nodes[node].spelling_idx;
        if self.if_valid_id_update(&mut id) {
            idx.push(id);
            start.push(pos);
        }
        (idx, start, !last_is_splitter)
    }

    /// `SpellingParser::get_splid_by_str`：整段正好是一个音节才返回它的编号（可能是半音节）；
    /// 第二项是“像一个还没打完的音节”，不管有没有返回编号都照 C++ 给出。
    fn get_splid_by_str(&self, s: &[u8]) -> (u16, bool) {
        let (idx, start, pre) = self.splstr_to_idxs(s, 2);
        if idx.len() != 1 || start[1] != s.len() {
            return (0, pre);
        }
        (idx[0], pre)
    }
}

// ---------------------------------------------------------------- 词表与词树

struct DictList {
    start_pos: [u32; MAX_LEMMA_SIZE + 1],
    start_id: [u32; MAX_LEMMA_SIZE + 1],
    buf: Vec<u16>,
}

impl DictList {
    fn load(reader: &mut Reader) -> Option<Self> {
        let scis_num = reader.u32()? as usize;
        let mut start_pos = [0u32; MAX_LEMMA_SIZE + 1];
        let mut start_id = [0u32; MAX_LEMMA_SIZE + 1];
        for v in start_pos.iter_mut() {
            *v = reader.u32()?;
        }
        for v in start_id.iter_mut() {
            *v = reader.u32()?;
        }
        reader.bytes((scis_num).checked_mul(4)?)?; // 单字表（汉字 + 音节编号）：整句解码用不到
        let buf_len = start_pos[MAX_LEMMA_SIZE] as usize;
        let raw = reader.bytes(buf_len.checked_mul(2)?)?;
        let buf = raw.chunks_exact(2).map(|b| u16::from_le_bytes([b[0], b[1]])).collect();
        Some(DictList { start_pos, start_id, buf })
    }

    /// 词条编号对应的汉字串；编号越界或没有对应词条返回空。
    fn lemma_str(&self, id: usize) -> &[u16] {
        if id >= self.start_id[MAX_LEMMA_SIZE] as usize {
            return &[];
        }
        for i in 0..MAX_LEMMA_SIZE {
            if self.start_id[i] as usize <= id && self.start_id[i + 1] as usize > id {
                let begin = self.start_pos[i] as usize + (id - self.start_id[i] as usize) * (i + 1);
                return self.buf.get(begin..begin + i + 1).unwrap_or(&[]);
            }
        }
        &[]
    }
}

struct NGram {
    freq_codes: Vec<u16>,
    lma_freq_idx: Vec<u8>,
}

impl NGram {
    fn load(reader: &mut Reader) -> Option<Self> {
        let idx_num = reader.u32()? as usize;
        let raw = reader.bytes(256 * 2)?;
        let freq_codes = raw.chunks_exact(2).map(|b| u16::from_le_bytes([b[0], b[1]])).collect();
        let lma_freq_idx = reader.bytes(idx_num)?.to_vec();
        Some(NGram { freq_codes, lma_freq_idx })
    }

    /// 一元分数：码本里的数，越小越可能。
    fn uni_psb(&self, id: usize) -> u16 {
        let code = self.lma_freq_idx.get(id).copied().unwrap_or(0);
        self.freq_codes[code as usize]
    }
}

#[derive(Clone, Copy)]
struct NodeLe0 {
    son_1st_off: u32,
    homo_idx_buf_off: u32,
    spl_idx: u16,
    num_of_son: u16,
    num_of_homo: u16,
}

#[derive(Clone, Copy)]
struct NodeGe1 {
    son_1st_off: u32,
    homo_idx_buf_off: u32,
    spl_idx: u16,
    num_of_son: u8,
    num_of_homo: u8,
}

struct DictTrie {
    root: Vec<NodeLe0>,
    ge1: Vec<NodeGe1>,
    idx_buf: Vec<u8>,
    le0_index: Vec<u16>,
}

impl DictTrie {
    fn load(reader: &mut Reader, spelling_num: usize) -> Option<Self> {
        let le0_num = reader.u32()? as usize;
        let ge1_num = reader.u32()? as usize;
        let idx_len = reader.u32()? as usize;
        let top_lmas = reader.u32()? as usize;
        if top_lmas >= idx_len || idx_len / 3 > SYS_DICT_ID_END {
            return None;
        }
        let mut root = Vec::with_capacity(le0_num.min(1 << 16));
        for _ in 0..le0_num {
            let b = reader.bytes(16)?;
            root.push(NodeLe0 {
                son_1st_off: u32::from_le_bytes([b[0], b[1], b[2], b[3]]),
                homo_idx_buf_off: u32::from_le_bytes([b[4], b[5], b[6], b[7]]),
                spl_idx: u16::from_le_bytes([b[8], b[9]]),
                num_of_son: u16::from_le_bytes([b[10], b[11]]),
                num_of_homo: u16::from_le_bytes([b[12], b[13]]),
            });
        }
        let mut ge1 = Vec::with_capacity(ge1_num.min(1 << 20));
        for _ in 0..ge1_num {
            let b = reader.bytes(10)?;
            ge1.push(NodeGe1 {
                son_1st_off: u16::from_le_bytes([b[0], b[1]]) as u32 + ((b[8] as u32) << 16),
                homo_idx_buf_off: u16::from_le_bytes([b[2], b[3]]) as u32 + ((b[9] as u32) << 16),
                spl_idx: u16::from_le_bytes([b[4], b[5]]),
                num_of_son: b[6],
                num_of_homo: b[7],
            });
        }
        let idx_buf = reader.bytes(idx_len)?.to_vec();
        let buf_size = spelling_num + 1;
        if le0_num > buf_size {
            return None;
        }
        let mut le0_index = vec![0u16; buf_size];
        let (mut last_splid, mut last_pos) = (FULL_ID_START as usize, 0usize);
        for i in 1..le0_num {
            let spl = root[i].spl_idx as usize;
            if spl < last_splid || spl - FULL_ID_START as usize >= buf_size {
                return None;
            }
            for splid in last_splid..spl {
                le0_index[splid - FULL_ID_START as usize] = last_pos as u16;
            }
            le0_index[spl - FULL_ID_START as usize] = i as u16;
            last_splid = spl;
            last_pos = i;
        }
        for splid in last_splid + 1..buf_size + FULL_ID_START as usize {
            le0_index[splid - FULL_ID_START as usize] = (last_pos + 1) as u16;
        }
        Some(DictTrie { root, ge1, idx_buf, le0_index })
    }

    fn lemma_id(&self, offset: usize) -> usize {
        let b = offset * 3;
        match self.idx_buf.get(b..b + 3) {
            Some(s) => s[0] as usize | (s[1] as usize) << 8 | (s[2] as usize) << 16,
            None => 0,
        }
    }
}

#[derive(Clone, Copy, Default)]
struct Lpi {
    id: u32,
    psb: u16,
}

struct Model {
    spl: SpellingTrie,
    list: DictList,
    dict: DictTrie,
    ngram: NGram,
}

#[derive(Clone, Copy, Default)]
struct Mark {
    offset: u32,
    num: u8,
}

#[derive(Clone, Copy, Default)]
struct Stone {
    start: u16,
    num: u16,
}

struct Dep {
    splids: [u16; MAX_ROW_NUM],
    splids_extended: u16,
    ext_len: u16,
    id_start: u16,
    id_num: u16,
}

struct Milestones {
    marks: Vec<Mark>,
    marks_pos: usize,
    stones: Vec<Stone>,
    stones_pos: usize,
}

impl Milestones {
    fn reset(&mut self, from_step: usize, from_handle: u16) {
        if from_step == 0 {
            self.marks_pos = 0;
            self.stones_pos = 1;
        } else if from_handle > 0 && (from_handle as usize) < self.stones_pos {
            self.stones_pos = from_handle as usize;
            self.marks_pos = self.stones[from_handle as usize].start as usize;
        }
    }

    fn can_mark(&self) -> bool {
        self.stones_pos < MAX_MILESTONE && self.marks_pos < MAX_PARSING_MARK
    }
}

impl Model {
    fn fill_lpi(&self, out: &mut [Lpi], max: usize, homo_off: usize, count: usize) -> usize {
        let mut n = 0;
        for homo in 0..count {
            let id = self.dict.lemma_id(homo_off + homo);
            out[n] = Lpi { id: id as u32, psb: self.ngram.uni_psb(id) };
            n += 1;
            if n >= max {
                break;
            }
        }
        n
    }

    fn extend_dict(&self, ms: &mut Milestones, cache: &LpiCache, from: u16, dep: &Dep, lpi: &mut [Lpi], lpi_max: usize, lpi_num: &mut usize) -> u16 {
        if from == 0 {
            return self.extend_dict0(ms, cache, dep, lpi, lpi_max, lpi_num);
        }
        let Some(stone) = ms.stones.get(from as usize).copied() else { return 0 };
        if from as usize >= ms.stones_pos {
            return 0;
        }
        let first_level = dep.splids_extended == 1;
        let (id_start, id_num) = (dep.id_start as usize, dep.id_num as usize);
        let mut ret_val = 0usize;
        for h_pos in 0..stone.num as usize {
            let mark = ms.marks[stone.start as usize + h_pos];
            for ext_pos in 0..mark.num as usize {
                let (son_first, son_num) = if first_level {
                    let Some(node) = self.dict.root.get(mark.offset as usize + ext_pos) else { continue };
                    (node.son_1st_off as usize, node.num_of_son as usize)
                } else {
                    let Some(node) = self.dict.ge1.get(mark.offset as usize + ext_pos) else { continue };
                    (node.son_1st_off as usize, node.num_of_son as usize)
                };
                let (mut found_start, mut found_num) = (0usize, 0usize);
                for son_pos in 0..son_num {
                    let Some(son) = self.dict.ge1.get(son_first + son_pos) else { break };
                    let spl = son.spl_idx as usize;
                    if spl >= id_start && spl < id_start + id_num {
                        if *lpi_num < lpi_max {
                            *lpi_num += self.fill_lpi(&mut lpi[*lpi_num..], lpi_max - *lpi_num, son.homo_idx_buf_off as usize, son.num_of_homo as usize);
                        }
                        if found_num == 0 {
                            found_start = son_pos;
                        }
                        found_num += 1;
                    }
                    if spl + 1 >= id_start + id_num || son_pos + 1 == son_num {
                        if found_num > 0 {
                            if ms.can_mark() {
                                ms.marks[ms.marks_pos] = Mark { offset: ((son_first + found_start) & 0xFF_FFFF) as u32, num: found_num as u8 };
                                if ret_val == 0 {
                                    ms.stones[ms.stones_pos].start = ms.marks_pos as u16;
                                }
                                ms.marks_pos += 1;
                            }
                            ret_val += 1;
                        }
                        break;
                    }
                }
            }
        }
        if ret_val > 0 {
            // C++ 在里程碑池满了以后仍然往后写、越过数组末尾；这里让池跟着长。
            if ms.stones_pos + 1 >= ms.stones.len() {
                ms.stones.resize(ms.stones_pos + 16, Stone::default());
            }
            ms.stones[ms.stones_pos].num = ret_val as u16;
            let handle = ms.stones_pos as u16;
            ms.stones_pos += 1;
            return handle;
        }
        0
    }

    fn extend_dict0(&self, ms: &mut Milestones, cache: &LpiCache, dep: &Dep, lpi: &mut [Lpi], lpi_max: usize, lpi_num: &mut usize) -> u16 {
        *lpi_num = 0;
        let mut ret = 0u16;
        let splid = dep.splids[dep.splids_extended as usize];
        let (id_start, id_num) = (dep.id_start as usize, dep.id_num as usize);
        let cached = cache.is_cached(splid);
        let base = FULL_ID_START as usize;
        let (Some(&start), Some(&end)) = (self.dict.le0_index.get(id_start.wrapping_sub(base)), self.dict.le0_index.get((id_start + id_num).wrapping_sub(base))) else { return 0 };
        let (son_start, son_end) = (start as usize, end as usize);
        let mut son_pos = son_start;
        while son_pos < son_end {
            let Some(son) = self.dict.root.get(son_pos).copied() else { break };
            if !cached && *lpi_num < lpi_max {
                let need = !(self.spl.is_half_id_yunmu(splid) && son_pos != son_start);
                if need {
                    *lpi_num += self.fill_lpi(&mut lpi[*lpi_num..], lpi_max - *lpi_num, son.homo_idx_buf_off as usize, son.num_of_homo as usize);
                }
            }
            if son.spl_idx as usize == id_start && ms.can_mark() {
                ms.marks[ms.marks_pos] = Mark { offset: (son_pos & 0xFF_FFFF) as u32, num: id_num as u8 };
                ms.stones[ms.stones_pos] = Stone { start: ms.marks_pos as u16, num: 1 };
                ret = ms.stones_pos as u16;
                ms.marks_pos += 1;
                ms.stones_pos += 1;
            }
            if son.spl_idx as usize + 1 >= id_start + id_num {
                break;
            }
            son_pos += 1;
        }
        ret
    }
}

struct LpiCache {
    items: Vec<Vec<Lpi>>,
}

impl LpiCache {
    fn is_cached(&self, splid: u16) -> bool {
        splid < FULL_ID_START && !self.items[splid as usize].is_empty()
    }

    fn put(&mut self, splid: u16, items: &[Lpi]) -> usize {
        let n = items.len().min(MAX_LPI_CACHE_PER_ID);
        self.items[splid as usize] = items[..n].to_vec();
        n
    }

    fn get(&self, splid: u16, out: &mut [Lpi], max: usize) -> usize {
        let cached = &self.items[splid as usize];
        let n = max.min(cached.len());
        out[..n].copy_from_slice(&cached[..n]);
        n
    }
}

// ---------------------------------------------------------------- 矩阵搜索

#[derive(Clone, Copy, Default)]
struct Dmi {
    handles: [u16; 2],
    dmi_fr: u16,
    spl_id: u16,
    dict_level: u8,
    splstr_len: u8,
    all_full_id: bool,
}

#[derive(Clone, Copy)]
struct Node {
    id: u32,
    score: f32,
    from: Option<usize>,
    dmi_fr: u16,
    step: u16,
}

#[derive(Clone, Copy, Default)]
struct Row {
    nd_pos: u16,
    nd_num: u16,
    dmi_pos: u16,
    dmi_num: u16,
    has_full_id: bool,
}

/// 句中辅助码：拼音串里 `pinyin_offset` 处起的那个音节，只能解成 `accept` 认可的汉字（UTF-8 单字）。
pub struct DecoderConstraint<'a> {
    pub pinyin_offset: usize,
    pub accept: &'a dyn Fn(&str) -> bool,
}

struct State {
    ms: Milestones,
    cache: LpiCache,
    rows: Vec<Row>,
    nodes: Vec<Node>,
    nodes_used: usize,
    dmis: Vec<Dmi>,
    dmi_used: usize,
    pys: [u8; MAX_ROW_NUM],
    pys_len: usize,
    lpi: Vec<Lpi>,
    lpi_total: usize,
    dep: Dep,
    positions: Vec<usize>,
}

impl State {
    fn new() -> Self {
        State {
            ms: Milestones { marks: vec![Mark::default(); MAX_PARSING_MARK + 1], marks_pos: 0, stones: vec![Stone::default(); MAX_MILESTONE + 2], stones_pos: 1 },
            cache: LpiCache { items: vec![Vec::new(); FULL_ID_START as usize] },
            rows: vec![Row::default(); MAX_ROW_NUM],
            nodes: vec![Node { id: 0, score: 0.0, from: None, dmi_fr: NO_POS, step: 0 }; MTRX_ND_POOL_SIZE],
            nodes_used: 0,
            dmis: vec![Dmi::default(); DMI_POOL_SIZE],
            dmi_used: 0,
            pys: [0; MAX_ROW_NUM],
            pys_len: 0,
            lpi: vec![Lpi::default(); MAX_LMA_PSB_ITEMS],
            lpi_total: 0,
            dep: Dep { splids: [0; MAX_ROW_NUM], splids_extended: 0, ext_len: 0, id_start: 0, id_num: 0 },
            positions: Vec::new(),
        }
    }

    fn reset_search0(&mut self) {
        self.pys_len = 0;
        self.nodes_used = 1;
        self.dmi_used = 0;
        self.rows[0] = Row { nd_pos: 0, nd_num: 1, dmi_pos: 0, dmi_num: 0, has_full_id: true };
        self.nodes[0] = Node { id: 0, score: 0.0, from: None, dmi_fr: NO_POS, step: 0 };
        self.ms.reset(0, 0);
    }

    /// 退回到第 `ch_pos` 个字符之后（`MatrixSearch::reset_search(ch_pos, false, false, false)`）。
    fn reset_search(&mut self, ch_pos: usize) -> bool {
        if ch_pos > self.pys_len || ch_pos >= MAX_ROW_NUM {
            return false;
        }
        if ch_pos == 0 {
            self.reset_search0();
            return true;
        }
        if self.pys_len > ch_pos && self.rows[ch_pos + 1].dmi_num > 0 {
            let handle = self.dmis[self.rows[ch_pos + 1].dmi_pos as usize].handles[0];
            self.ms.reset(ch_pos, handle);
        }
        self.pys_len = ch_pos;
        self.dmi_used = self.rows[ch_pos].dmi_pos as usize + self.rows[ch_pos].dmi_num as usize;
        self.nodes_used = self.rows[ch_pos].nd_pos as usize + self.rows[ch_pos].nd_num as usize;
        true
    }

    fn add_char(&mut self, m: &Model, ch: u8, accept: &dyn Fn(usize, u16) -> bool) -> bool {
        if self.pys_len >= MAX_ROW_NUM - 1 || (!is_valid_spl_char(ch) && ch != b'\'') {
            return false;
        }
        if self.dmi_used >= DMI_POOL_SIZE {
            return false;
        }
        self.pys[self.pys_len] = ch;
        self.pys_len += 1;
        self.rows[self.pys_len] = Row { nd_pos: self.nodes_used as u16, nd_num: 0, dmi_pos: self.dmi_used as u16, dmi_num: 0, has_full_id: false };
        self.add_char_qwerty(m, accept)
    }

    fn add_char_qwerty(&mut self, m: &Model, accept: &dyn Fn(usize, u16) -> bool) -> bool {
        let cur = self.pys_len;
        self.rows[cur].nd_num = 0;
        let mut spl_matched = false;
        let mut longest_ext = 0usize;
        for ext_len in (1..=MAX_PINYIN_SIZE + 1).rev() {
            if ext_len > cur {
                continue;
            }
            if ext_len > 1 && longest_ext != 0 && !self.rows[cur - ext_len].has_full_id {
                break;
            }
            let oldrow = cur - ext_len;
            if self.rows[oldrow].nd_num == 0 {
                continue;
            }
            let (spl_idx, is_pre) = m.spl.get_splid_by_str(&self.pys[oldrow..oldrow + ext_len]);
            if is_pre {
                spl_matched = true;
            }
            if spl_idx == 0 {
                continue;
            }
            let dmi_first = self.rows[oldrow].dmi_pos as usize;
            let dmi_count = self.rows[oldrow].dmi_num as usize;
            for dmi_pos in dmi_first..=dmi_first + dmi_count {
                let dmi = if dmi_pos == dmi_first + dmi_count { None } else { Some(dmi_pos) };
                if longest_ext > ext_len {
                    match dmi {
                        None if !self.rows[oldrow].has_full_id => continue,
                        Some(d) if m.spl.is_half_id(self.dmis[d].spl_id) => continue,
                        _ => {}
                    }
                }
                self.dep.splids_extended = 0;
                if let Some(d) = dmi {
                    let mut prev = self.dmis[d].dict_level as usize;
                    if prev >= MAX_LEMMA_SIZE {
                        continue;
                    }
                    let mut at = d;
                    loop {
                        prev -= 1;
                        self.dep.splids[prev] = self.dmis[at].spl_id;
                        if self.dmis[at].dmi_fr == NO_POS {
                            break;
                        }
                        at = self.dmis[at].dmi_fr as usize;
                    }
                    self.dep.splids_extended = self.dmis[d].dict_level as u16;
                }
                self.dep.splids[self.dep.splids_extended as usize] = spl_idx;
                self.dep.ext_len = ext_len as u16;
                self.dep.id_num = 1;
                self.dep.id_start = spl_idx;
                if m.spl.is_half_id(spl_idx) {
                    let mut start = 0;
                    self.dep.id_num = m.spl.half_to_full(spl_idx, &mut start);
                    self.dep.id_start = start;
                }
                let new_dmi_num = self.extend_dmi(m, dmi);
                if new_dmi_num > 0 {
                    self.rows[cur].dmi_num += new_dmi_num as u16;
                    self.dmi_used += new_dmi_num;
                    if !m.spl.is_half_id(spl_idx) {
                        self.rows[cur].has_full_id = true;
                    }
                }
                if self.lpi_total > 0 {
                    let fr_row = match dmi {
                        None => oldrow,
                        Some(d) => oldrow - self.dmis[d].splstr_len as usize,
                    };
                    self.filter_by_constraints(m, dmi, fr_row, accept);
                    let first = self.rows[fr_row].nd_pos as usize;
                    for node in first..first + self.rows[fr_row].nd_num as usize {
                        self.extend_mtrx_nd(node, (self.dmi_used - new_dmi_num) as u16, cur);
                        if longest_ext == 0 {
                            longest_ext = ext_len;
                        }
                    }
                }
            }
        }
        self.nodes_used += self.rows[cur].nd_num as usize;
        self.rows[cur].nd_num != 0 || spl_matched
    }

    fn extend_dmi(&mut self, m: &Model, dmi_s: Option<usize>) -> usize {
        if self.dmi_used >= DMI_POOL_SIZE {
            return 0;
        }
        let splid = self.dep.splids[self.dep.splids_extended as usize];
        let cached = self.dep.splids_extended == 0 && self.cache.is_cached(splid);
        let mut ret_val = 0;
        self.lpi_total = 0;
        let mut from = 0u16;
        if self.dep.splids_extended != 0 {
            from = dmi_s.map_or(0, |d| self.dmis[d].handles[0]);
        }
        let mut lpi_num = 0usize;
        let mut handle = 0u16;
        if from > 0 || dmi_s.is_none() {
            handle = m.extend_dict(&mut self.ms, &self.cache, from, &self.dep, &mut self.lpi, MAX_LMA_PSB_ITEMS, &mut lpi_num);
        }
        if handle > 0 {
            self.lpi_total = lpi_num;
        }
        if handle != 0 {
            let add = self.dmi_used;
            let new = match dmi_s {
                None => Dmi { handles: [handle, 0], dmi_fr: NO_POS, spl_id: splid, dict_level: 1, splstr_len: self.dep.ext_len as u8 & 0x7F, all_full_id: !m.spl.is_half_id(splid) },
                Some(d) => {
                    let prev = self.dmis[d];
                    Dmi {
                        handles: [handle, 0],
                        dmi_fr: d as u16,
                        spl_id: splid,
                        dict_level: prev.dict_level + 1,
                        splstr_len: (prev.splstr_len as u16 + self.dep.ext_len) as u8 & 0x7F,
                        all_full_id: !m.spl.is_half_id(splid) && prev.all_full_id,
                    }
                }
            };
            self.dmis[add] = new;
            ret_val = 1;
        }
        if !cached {
            if self.lpi_total == 0 {
                return ret_val;
            }
            qsort_by(&mut self.lpi[..self.lpi_total], |a, b| a.psb.cmp(&b.psb) as i32);
            if dmi_s.is_none() && m.spl.is_half_id(splid) {
                self.lpi_total = self.cache.put(splid, &self.lpi[..self.lpi_total]);
            }
        } else {
            self.lpi_total = self.cache.get(splid, &mut self.lpi, MAX_LMA_PSB_ITEMS);
        }
        ret_val
    }

    fn extend_mtrx_nd(&mut self, from_node: usize, dmi_fr: u16, res_row: usize) -> usize {
        if self.nodes_used >= MTRX_ND_POOL_SIZE - MAX_NODE_A_ROW {
            return 0;
        }
        let mut lpi_num = self.lpi_total;
        if self.nodes[from_node].step == 0 && lpi_num > MAX_NODE_A_ROW {
            lpi_num = MAX_NODE_A_ROW;
        }
        let res_min = self.rows[res_row].nd_pos as usize;
        for pos in 0..lpi_num {
            let score = self.nodes[from_node].score + f32::from(self.lpi[pos].psb);
            if pos > 0 && f64::from(score) - PRUNING_SCORE > f64::from(self.nodes[res_min].score) {
                break;
            }
            let nd_num = self.rows[res_row].nd_num as usize;
            let mut res = res_min + nd_num;
            let mut replace = false;
            while res > res_min && score < self.nodes[res - 1].score {
                if res - res_min < MAX_NODE_A_ROW {
                    self.nodes[res] = self.nodes[res - 1];
                }
                res -= 1;
                replace = true;
            }
            if replace || (nd_num < MAX_NODE_A_ROW && res_min + nd_num < MTRX_ND_POOL_SIZE) {
                self.nodes[res] = Node { id: self.lpi[pos].id, score, from: Some(from_node), dmi_fr, step: res_row as u16 };
                if (self.rows[res_row].nd_num as usize) < MAX_NODE_A_ROW {
                    self.rows[res_row].nd_num += 1;
                }
            }
        }
        self.rows[res_row].nd_num as usize
    }

    /// 本地补丁：被约束的音节只留下 `accept` 认可的汉字；字数和音节数对不上的候选一并丢掉。
    fn filter_by_constraints(&mut self, m: &Model, dmi: Option<usize>, fr_row: usize, accept: &dyn Fn(usize, u16) -> bool) {
        if self.positions.is_empty() || self.lpi_total == 0 {
            return;
        }
        let to_row = self.pys_len;
        let spl_num = dmi.map_or(1, |d| self.dmis[d].dict_level as usize + 1);
        if spl_num > MAX_LEMMA_SIZE {
            return;
        }
        let mut spl_starts = [0usize; MAX_LEMMA_SIZE];
        spl_starts[0] = fr_row;
        let mut level = spl_num - 1;
        let mut at = dmi;
        while let Some(d) = at {
            if level == 0 {
                break;
            }
            spl_starts[level] = fr_row + self.dmis[d].splstr_len as usize;
            at = if self.dmis[d].dmi_fr == NO_POS { None } else { Some(self.dmis[d].dmi_fr as usize) };
            level -= 1;
        }
        let mut pinned: Vec<(usize, usize)> = Vec::new();
        for (k, &pos) in self.positions.iter().enumerate() {
            if pos < fr_row || pos >= to_row {
                continue;
            }
            match spl_starts[..spl_num].iter().position(|&s| s == pos) {
                Some(offset) => pinned.push((k, offset)),
                None => {
                    self.lpi_total = 0;
                    return;
                }
            }
        }
        if pinned.is_empty() {
            return;
        }
        let mut kept = 0;
        for pos in 0..self.lpi_total {
            let text = m.list.lemma_str(self.lpi[pos].id as usize);
            let accepted = text.len() == spl_num && pinned.iter().all(|&(k, offset)| accept(k, text[offset]));
            if accepted {
                self.lpi[kept] = self.lpi[pos];
                kept += 1;
            }
        }
        self.lpi_total = kept;
    }

    /// 最佳路径上的音节总数（`get_spl_start_id` 里算出的 `spl_id_num_`）。
    fn spl_id_num(&self) -> usize {
        if self.pys_len == 0 || self.rows[self.pys_len].nd_num == 0 {
            return 0;
        }
        let mut count = 0;
        let mut node = self.rows[self.pys_len].nd_pos as usize;
        while node != 0 {
            let mut dmi_fr = self.nodes[node].dmi_fr;
            while dmi_fr != NO_POS {
                count += 1;
                dmi_fr = self.dmis[dmi_fr as usize].dmi_fr;
            }
            match self.nodes[node].from {
                Some(from) => node = from,
                None => break,
            }
        }
        count
    }

    fn search(&mut self, m: &Model, py: &[u8], accept: &dyn Fn(usize, u16) -> bool) {
        let mut py_len = py.len().min(MAX_ROW_NUM - 1);
        self.reset_search0();
        self.pys[..py_len].copy_from_slice(&py[..py_len]);
        self.pys[py_len] = 0;
        let mut ch_pos = 0;
        while self.pys[ch_pos] != 0 {
            if !self.add_char(m, py[ch_pos], accept) {
                self.pys_len = ch_pos;
                break;
            }
            ch_pos += 1;
        }
        while self.spl_id_num() > MAX_HZS_LEN {
            py_len -= 1;
            self.reset_search(py_len);
            self.pys[py_len] = 0;
        }
    }

    /// 矩阵最后一行得分最低的路径拼出的句子（`get_candidate0`）。
    fn best_sentence(&self, m: &Model) -> Vec<u16> {
        if self.pys_len == 0 || self.rows[self.pys_len].nd_num == 0 {
            return Vec::new();
        }
        let mut ids: Vec<u32> = Vec::new();
        let mut node = Some(self.rows[self.pys_len].nd_pos as usize);
        while let Some(n) = node {
            ids.push(self.nodes[n].id);
            node = self.nodes[n].from;
        }
        let mut out: Vec<u16> = Vec::new();
        for &id in ids.iter().rev() {
            if id == 0 {
                continue;
            }
            let text = m.list.lemma_str(id as usize);
            if text.is_empty() || 255 - out.len() <= text.len() {
                return Vec::new();
            }
            out.extend_from_slice(text);
        }
        out
    }
}

/// 载入后的 Google 拼音解码器；跨线程共用时内部加锁（和 C++ 适配层一样一次只跑一个搜索）。
pub struct PinyinDecoder {
    model: Model,
    state: Mutex<State>,
}

impl PinyinDecoder {
    /// 载入 `dict_pinyin.dat` 的内容；格式不对或被截断返回 `None`。
    pub fn load(bytes: &[u8]) -> Option<Self> {
        let mut reader = Reader { data: bytes, pos: 0 };
        let spl = SpellingTrie::load(&mut reader)?;
        let list = DictList::load(&mut reader)?;
        let dict = DictTrie::load(&mut reader, spl.spelling_num)?;
        let ngram = NGram::load(&mut reader)?;
        Some(PinyinDecoder { model: Model { spl, list, dict, ngram }, state: Mutex::new(State::new()) })
    }

    pub fn load_file(path: &Path) -> Option<Self> {
        Self::load(&std::fs::read(path).ok()?)
    }

    /// 整句解码；`pinyin` 为空、超过 128 字节、约束位置越界，或者解不出句子都返回空串。
    pub fn sentence(&self, pinyin: &str, constraints: &[DecoderConstraint]) -> String {
        let bytes = pinyin.as_bytes();
        if bytes.is_empty() || bytes.len() > 128 {
            return String::new();
        }
        if constraints.iter().any(|c| c.pinyin_offset >= bytes.len()) {
            return String::new();
        }
        let accept = |index: usize, hanzi: u16| -> bool {
            match constraints.get(index) {
                Some(c) => (c.accept)(&String::from_utf16_lossy(&[hanzi])),
                None => true,
            }
        };
        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        state.positions = constraints.iter().map(|c| c.pinyin_offset & 0xFFFF).collect();
        state.search(&self.model, bytes, &accept);
        let sentence = state.best_sentence(&self.model);
        state.positions.clear();
        String::from_utf16_lossy(&sentence)
    }

    /// 音节编号对应的整段字符串能否解析（测试和探针对账用）：返回 (编号, 像不像没打完的音节)。
    pub fn spelling_id(&self, text: &str) -> (u16, bool) {
        self.model.spl.get_splid_by_str(text.as_bytes())
    }
}
