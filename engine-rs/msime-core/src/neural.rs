//! 神经重排：字符级 Transformer 句子模型（`sentence-model.safetensors`），移植自 `engine/neural/{sentence_model,neural_decoder}.cpp`。
//!
//! 模型是 decoder-only 的 Pre-LN Transformer，输入输出共用词表嵌入。输入法用的是 [`SentenceModel::score_sentences`]：
//! 在已上屏的前文之后给一批成型句子打 `log P(句子 | 前文)`；[`rerank_order`] 把词格 n-best 路径的静态分和神经分
//! 按 `静态 + λ·(神经 − 静态)` 插值后重排。
//!
//! 数值上和 C++ 逐位对齐的地方：erf 用 Abramowitz–Stegun 7.1.26 的 f32 多项式（模型不是用 tanh 近似训练的，也不是
//! 精确 erf）；LayerNorm eps 1e-5；注意力缩放 `1/sqrt(头宽)`；int8 每输出行对称量化，缩放在点积之后乘一次；位置 i 预测
//! 位置 i+1。int8 矩阵在支持 AVX2+FMA 的 x86 上走和 C++ 一样的 8 路 FMA 累加（`linear_int8_avx2`），否则走 4 路标量
//! 累加；两条路径的末位会不同，C++ 也一样。`exp`、`ln` 来自各自的数学库，末位可能差 1 ulp。
//!
//! 没搬的：`RescoreWorker`（后台线程池，异步重排的调度，不是算法）、`shared_sentence_model` 的进程级缓存
//! （调用方自己持有 `SentenceModel`，需要共享就包一层 `Arc`）。

use std::collections::HashMap;
use std::fs;
use std::path::Path;
use std::sync::Mutex;

const UNK: u32 = 1;
const BOS: u32 = 2;
const NO_TARGET: u32 = 0xFFFF_FFFF;
/// 词格报的是 log10（kenlm 的单位），模型报的是自然对数。
const LOG10_TO_NATS: f64 = 2.302585092994046;

// ---------------------------------------------------------------------------------------------
// 数值原语
// ---------------------------------------------------------------------------------------------

fn erf_approx(x: f32) -> f32 {
    // Abramowitz and Stegun 7.1.26；误差小于 1.5e-7。
    let sign = if x < 0.0 { -1.0f32 } else { 1.0f32 };
    let x = x.abs();
    let t = 1.0f32 / (1.0f32 + 0.327_591_1f32 * x);
    let poly = t * (0.254_829_6f32 + t * (-0.284_496_74f32 + t * (1.421_413_7f32 + t * (-1.453_152f32 + t * 1.061_405_4f32))));
    sign * (1.0f32 - poly * (-x * x).exp())
}

fn gelu(x: f32) -> f32 {
    const INV_SQRT2: f32 = 0.707_106_77f32;
    0.5f32 * x * (1.0f32 + erf_approx(x * INV_SQRT2))
}

/// 四路累加，不是一路：一路串行加要等上一次加完，四路在最后才合并（和 C++ 一样，末位与严格从左到右的和不同）。
fn dot(a: &[f32], b: &[f32]) -> f32 {
    let n = a.len().min(b.len());
    let (mut s0, mut s1, mut s2, mut s3) = (0.0f32, 0.0f32, 0.0f32, 0.0f32);
    let mut i = 0;
    while i + 4 <= n {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
        i += 4;
    }
    let mut total = (s0 + s1) + (s2 + s3);
    while i < n {
        total += a[i] * b[i];
        i += 1;
    }
    total
}

fn dot_i8(a: &[f32], b: &[i8]) -> f32 {
    let mut total = 0.0f32;
    for i in 0..a.len().min(b.len()) {
        total += a[i] * f32::from(b[i]);
    }
    total
}

#[cfg(target_arch = "x86_64")]
fn avx2_fma_available() -> bool {
    use std::sync::OnceLock;
    static AVAILABLE: OnceLock<bool> = OnceLock::new();
    *AVAILABLE.get_or_init(|| is_x86_feature_detected!("avx2") && is_x86_feature_detected!("fma"))
}

#[cfg(not(target_arch = "x86_64"))]
fn avx2_fma_available() -> bool {
    false
}

/// `out[row][o] = dot(x[row], w[o]) * scale[o] + bias[o]`，int8 权重；8 路 FMA 累加，加完 8 路再加尾巴（`inputs` 不是 8 的倍数时）。
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,fma")]
unsafe fn linear_int8_avx2(x: &[f32], weights: &[i8], scales: &[f32], bias: Option<&[f32]>, rows: usize, inputs: usize, outputs: usize, out: &mut [f32]) {
    use std::arch::x86_64::*;
    for output in 0..outputs {
        let weight_row = &weights[output * inputs..(output + 1) * inputs];
        let scale = scales[output];
        let offset = bias.map_or(0.0f32, |b| b[output]);
        for row in 0..rows {
            let activations = &x[row * inputs..(row + 1) * inputs];
            let mut accumulator = _mm256_setzero_ps();
            let mut input = 0;
            while input + 8 <= inputs {
                let packed = _mm_loadl_epi64(weight_row.as_ptr().add(input) as *const __m128i);
                let widened = _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(packed));
                let activation = _mm256_loadu_ps(activations.as_ptr().add(input));
                accumulator = _mm256_fmadd_ps(activation, widened, accumulator);
                input += 8;
            }
            let mut lanes = [0.0f32; 8];
            _mm256_storeu_ps(lanes.as_mut_ptr(), accumulator);
            let mut total = 0.0f32;
            for lane in lanes {
                total += lane;
            }
            for tail in input..inputs {
                total += activations[tail] * f32::from(weight_row[tail]);
            }
            out[row * outputs + output] = total * scale + offset;
        }
    }
}

/// IEEE 754 半精度转单精度（次正规数照常处理，不清零）。
fn half_to_f32(bits: u16) -> f32 {
    let sign = u32::from(bits & 0x8000) << 16;
    let exponent = u32::from((bits >> 10) & 0x1f);
    let mantissa = u32::from(bits & 0x03ff);
    let out = if exponent == 0 {
        if mantissa == 0 {
            sign
        } else {
            let leading = mantissa.leading_zeros();
            let exp = 134 - leading;
            let mant = (mantissa << (leading - 8)) & 0x007f_ffff;
            sign | (exp << 23) | mant
        }
    } else if exponent == 0x1f {
        sign | 0x7f80_0000 | (mantissa << 13)
    } else {
        sign | ((exponent + 127 - 15) << 23) | (mantissa << 13)
    };
    f32::from_bits(out)
}

/// UTF-8 解成码点；坏字节变 U+FFFD。边界判断照 C++ 原样（`i + extra >= n` 时整段当一个 U+FFFD 收尾）。
fn decode_utf8(text: &[u8]) -> Vec<u32> {
    let mut out = Vec::new();
    let mut i = 0;
    let n = text.len();
    while i < n {
        let c = text[i];
        let (mut code, extra) = if c < 0x80 {
            (u32::from(c), 0)
        } else if c & 0xE0 == 0xC0 {
            (u32::from(c & 0x1F), 1)
        } else if c & 0xF0 == 0xE0 {
            (u32::from(c & 0x0F), 2)
        } else if c & 0xF8 == 0xF0 {
            (u32::from(c & 0x07), 3)
        } else {
            out.push(0xFFFD);
            i += 1;
            continue;
        };
        if i + extra >= n {
            out.push(0xFFFD);
            break;
        }
        let mut ok = true;
        for j in 1..=extra {
            let cc = text[i + j];
            if cc & 0xC0 != 0x80 {
                ok = false;
                break;
            }
            code = (code << 6) | u32::from(cc & 0x3F);
        }
        if !ok {
            out.push(0xFFFD);
            i += 1;
            continue;
        }
        out.push(code);
        i += extra + 1;
    }
    out
}

fn layer_norm(x: &[f32], rows: usize, weight: &[f32], bias: &[f32], width: usize) -> Vec<f32> {
    let mut out = vec![0.0f32; rows * width];
    for row in 0..rows {
        let source = &x[row * width..(row + 1) * width];
        let target = &mut out[row * width..(row + 1) * width];
        let mut mean = 0.0f32;
        for &v in source {
            mean += v;
        }
        mean /= width as f32;
        let mut variance = 0.0f32;
        for &v in source {
            let d = v - mean;
            variance += d * d;
        }
        variance /= width as f32;
        let inverse = 1.0f32 / (variance + 1e-5f32).sqrt();
        for i in 0..width {
            target[i] = (source[i] - mean) * inverse * weight[i] + bias[i];
        }
    }
    out
}

// ---------------------------------------------------------------------------------------------
// 模型
// ---------------------------------------------------------------------------------------------

/// 权重矩阵，保持文件里的精度。int8 矩阵不展开：每输出行的缩放是常数，提到点积外乘一次。
#[derive(Default)]
struct Matrix {
    quantized: bool,
    floats: Vec<f32>,
    ints: Vec<i8>,
    scales: Vec<f32>,
}

impl Matrix {
    fn len(&self) -> usize {
        if self.quantized {
            self.ints.len()
        } else {
            self.floats.len()
        }
    }

    fn dot_row(&self, x: &[f32], row: usize, width: usize) -> f32 {
        if self.quantized {
            dot_i8(x, &self.ints[row * width..(row + 1) * width]) * self.scales[row]
        } else {
            dot(x, &self.floats[row * width..(row + 1) * width])
        }
    }

    fn row_into(&self, index: usize, width: usize, out: &mut [f32]) {
        if self.quantized {
            let scale = self.scales[index];
            for (i, &p) in self.ints[index * width..(index + 1) * width].iter().enumerate() {
                out[i] = f32::from(p) * scale;
            }
        } else {
            out[..width].copy_from_slice(&self.floats[index * width..(index + 1) * width]);
        }
    }

    /// `out[row][o] = dot(x[row], w[o]) * scale + bias[o]`，输出主序：权重比缓存大得多，每行权重只读一次、跨整批复用。
    fn linear(&self, x: &[f32], bias: &[f32], inputs: usize, outputs: usize) -> Vec<f32> {
        let rows = if inputs == 0 { 0 } else { x.len() / inputs };
        let mut out = vec![0.0f32; rows * outputs];
        #[cfg(target_arch = "x86_64")]
        if self.quantized && avx2_fma_available() && rows != 0 && inputs != 0 {
            // SAFETY: 已检测到 AVX2 与 FMA；各切片长度由调用方的几何校验保证。
            unsafe { linear_int8_avx2(x, &self.ints, &self.scales, if bias.is_empty() { None } else { Some(bias) }, rows, inputs, outputs, &mut out) };
            return out;
        }
        let mut scratch = vec![0.0f32; if self.quantized { inputs } else { 0 }];
        for o in 0..outputs {
            let (weights, scale): (&[f32], f32) = if self.quantized {
                for (i, &q) in self.ints[o * inputs..(o + 1) * inputs].iter().enumerate() {
                    scratch[i] = f32::from(q);
                }
                (&scratch, self.scales[o])
            } else {
                (&self.floats[o * inputs..(o + 1) * inputs], 1.0f32)
            };
            let b = if bias.is_empty() { 0.0f32 } else { bias[o] };
            for row in 0..rows {
                out[row * outputs + o] = dot(&x[row * inputs..(row + 1) * inputs], weights) * scale + b;
            }
        }
        out
    }
}

#[derive(Debug, Clone, Copy, Default)]
struct Config {
    vocab: usize,
    n_layer: usize,
    n_head: usize,
    n_embd: usize,
    context: usize,
}

#[derive(Default)]
struct Block {
    ln1_weight: Vec<f32>,
    ln1_bias: Vec<f32>,
    qkv_weight: Matrix,
    qkv_bias: Vec<f32>,
    proj_weight: Matrix,
    proj_bias: Vec<f32>,
    ln2_weight: Vec<f32>,
    ln2_bias: Vec<f32>,
    fc_weight: Matrix,
    fc_bias: Vec<f32>,
    out_weight: Matrix,
    out_bias: Vec<f32>,
}

/// 一段固定前导 token 的逐层 K/V，每层 `[长度, n_embd]` 行主序。后面每个批行都 attend 到同一份缓存。
#[derive(Default, Clone)]
struct PrefixCache {
    keys: Vec<Vec<f32>>,
    values: Vec<Vec<f32>>,
    length: usize,
}

pub struct SentenceModel {
    config: Config,
    index: HashMap<u32, u32>,
    token: Matrix,
    position: Vec<f32>,
    blocks: Vec<Block>,
    final_weight: Vec<f32>,
    final_bias: Vec<f32>,
    attribution: String,
    license: String,
    /// 前文（去掉末个 token）的 K/V 缓存，前文不变时复用。
    cache: Mutex<(Vec<u32>, PrefixCache)>,
}

struct Entry {
    dtype: String,
    shape: Vec<usize>,
    off0: usize,
    off1: usize,
}

fn read_f32(data: &[u8], entry: &Entry) -> Result<Vec<f32>, String> {
    if entry.off1 > data.len() || entry.off0 > entry.off1 {
        return Err("tensor offsets out of range".into());
    }
    Ok(data[entry.off0..entry.off1].chunks_exact(4).map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect())
}

fn read_f16(data: &[u8], entry: &Entry) -> Result<Vec<f32>, String> {
    if entry.off1 > data.len() || entry.off0 > entry.off1 {
        return Err("tensor offsets out of range".into());
    }
    Ok(data[entry.off0..entry.off1].chunks_exact(2).map(|c| half_to_f32(u16::from_le_bytes([c[0], c[1]]))).collect())
}

impl SentenceModel {
    /// 从内存里的 safetensors 载入。任何格式问题都返回 `None`（模型缺失或损坏时输入法照常工作，只是没有神经重排）。
    pub fn load(bytes: &[u8]) -> Option<Self> {
        Self::try_load(bytes).ok().flatten()
    }

    fn try_load(bytes: &[u8]) -> Result<Option<Self>, String> {
        if bytes.len() < 8 {
            return Ok(None);
        }
        let header_len = u64::from_le_bytes(bytes[..8].try_into().unwrap());
        let header_end = 8usize.saturating_add(usize::try_from(header_len).unwrap_or(usize::MAX));
        if header_len == 0 || header_end > bytes.len() {
            return Ok(None);
        }
        let header: serde_json::Value = serde_json::from_slice(&bytes[8..header_end]).map_err(|e| e.to_string())?;
        let Some(header) = header.as_object() else { return Ok(None) };
        let data = &bytes[header_end..];

        let Some(metadata) = header.get("__metadata__").and_then(|m| m.as_object()) else { return Ok(None) };
        let metadata_string = |key: &str| metadata.get(key).and_then(|v| v.as_str());
        let (Some(config_text), Some(vocab_text)) = (metadata_string("config"), metadata_string("vocab")) else { return Ok(None) };

        let config_json: serde_json::Value = serde_json::from_str(config_text).map_err(|e| e.to_string())?;
        let config_uint = |key: &str| -> Result<usize, String> {
            config_json.get(key).and_then(|v| v.as_f64()).map(|v| v as usize).ok_or_else(|| format!("config missing {key}"))
        };
        let config = Config {
            vocab: config_uint("vocab")?,
            n_layer: config_uint("n_layer")?,
            n_head: config_uint("n_head")?,
            n_embd: config_uint("n_embd")?,
            context: config_uint("context")?,
        };
        if config.n_head == 0 || config.n_embd == 0 || config.n_embd % config.n_head != 0 || config.vocab == 0 || config.n_layer == 0 || config.context == 0 {
            return Ok(None);
        }

        let vocab_json: serde_json::Value = serde_json::from_str(vocab_text).map_err(|e| e.to_string())?;
        let Some(vocab) = vocab_json.as_array() else { return Ok(None) };
        if vocab.len() != config.vocab {
            return Ok(None);
        }
        let mut index = HashMap::new();
        for (id, item) in vocab.iter().enumerate() {
            let Some(text) = item.as_str() else { continue };
            let chars = decode_utf8(text.as_bytes());
            // 保留符号（<pad> 等）靠常量寻址；单码点才是字符词表。
            if chars.len() == 1 {
                index.insert(chars[0], id as u32);
            }
        }

        let mut entries: HashMap<&str, Entry> = HashMap::new();
        for (name, desc) in header {
            if name == "__metadata__" {
                continue;
            }
            let Some(desc) = desc.as_object() else { continue };
            let (Some(dtype), Some(offsets)) = (desc.get("dtype").and_then(|v| v.as_str()), desc.get("data_offsets").and_then(|v| v.as_array())) else { return Ok(None) };
            if offsets.len() != 2 {
                return Ok(None);
            }
            let shape = desc.get("shape").and_then(|v| v.as_array()).map(|dims| dims.iter().map(|d| d.as_f64().unwrap_or(0.0) as usize).collect()).unwrap_or_default();
            entries.insert(
                name.as_str(),
                Entry { dtype: dtype.to_string(), shape, off0: offsets[0].as_f64().unwrap_or(0.0) as usize, off1: offsets[1].as_f64().unwrap_or(0.0) as usize },
            );
        }

        let materialize = |name: &str| -> Result<Matrix, String> {
            let entry = entries.get(name).ok_or_else(|| format!("missing tensor {name}"))?;
            let expected: usize = entry.shape.iter().product();
            let mut matrix = Matrix::default();
            match entry.dtype.as_str() {
                "F32" | "F16" => {
                    matrix.floats = if entry.dtype == "F32" { read_f32(data, entry)? } else { read_f16(data, entry)? };
                    if matrix.floats.len() != expected {
                        return Err(format!("{name}: shape does not match data"));
                    }
                }
                "I8" => {
                    let scale_entry = entries.get(format!("{name}.scale").as_str()).ok_or_else(|| format!("{name}: missing scale"))?;
                    matrix.scales = read_f32(data, scale_entry)?;
                    let rows = entry.shape.first().copied().unwrap_or(1);
                    if rows == 0 || matrix.scales.len() != rows {
                        return Err(format!("{name}: scale length does not match rows"));
                    }
                    if entry.off1 > data.len() || entry.off0 > entry.off1 {
                        return Err(format!("{name}: offsets out of range"));
                    }
                    matrix.ints = data[entry.off0..entry.off1].iter().map(|&b| b as i8).collect();
                    matrix.quantized = true;
                    if matrix.ints.len() != expected {
                        return Err(format!("{name}: shape does not match data"));
                    }
                }
                other => return Err(format!("{name}: unsupported dtype {other}")),
            }
            Ok(matrix)
        };
        let materialize_floats = |name: &str| -> Result<Vec<f32>, String> {
            let matrix = materialize(name)?;
            if matrix.quantized {
                return Err(format!("{name} is quantized but must be float"));
            }
            Ok(matrix.floats)
        };

        let token = materialize("tok.weight")?;
        let position = materialize_floats("pos.weight")?;
        let mut blocks = Vec::with_capacity(config.n_layer);
        for layer in 0..config.n_layer {
            let prefix = format!("blocks.{layer}.");
            blocks.push(Block {
                ln1_weight: materialize_floats(&format!("{prefix}ln1.weight"))?,
                ln1_bias: materialize_floats(&format!("{prefix}ln1.bias"))?,
                qkv_weight: materialize(&format!("{prefix}qkv.weight"))?,
                qkv_bias: materialize_floats(&format!("{prefix}qkv.bias"))?,
                proj_weight: materialize(&format!("{prefix}proj.weight"))?,
                proj_bias: materialize_floats(&format!("{prefix}proj.bias"))?,
                ln2_weight: materialize_floats(&format!("{prefix}ln2.weight"))?,
                ln2_bias: materialize_floats(&format!("{prefix}ln2.bias"))?,
                fc_weight: materialize(&format!("{prefix}fc.weight"))?,
                fc_bias: materialize_floats(&format!("{prefix}fc.bias"))?,
                out_weight: materialize(&format!("{prefix}out.weight"))?,
                out_bias: materialize_floats(&format!("{prefix}out.bias"))?,
            });
        }
        let final_weight = materialize_floats("ln_f.weight")?;
        let final_bias = materialize_floats("ln_f.bias")?;

        // 几何校验：任何不符都是坏文件，拒收。
        let n_embd = config.n_embd;
        if token.len() != config.vocab * n_embd || position.len() != config.context * n_embd || final_weight.len() != n_embd || final_bias.len() != n_embd {
            return Ok(None);
        }
        for block in &blocks {
            if block.qkv_weight.len() != 3 * n_embd * n_embd || block.proj_weight.len() != n_embd * n_embd || block.fc_weight.len() != 4 * n_embd * n_embd || block.out_weight.len() != 4 * n_embd * n_embd {
                return Ok(None);
            }
        }
        Ok(Some(Self {
            config,
            index,
            token,
            position,
            blocks,
            final_weight,
            final_bias,
            attribution: metadata_string("attribution").unwrap_or_default().to_string(),
            license: metadata_string("license").unwrap_or_default().to_string(),
            cache: Mutex::new((Vec::new(), PrefixCache::default())),
        }))
    }

    /// 从磁盘载入；文件不存在、读不了或格式不对返回 `None`。
    pub fn load_file(path: &Path) -> Option<Self> {
        let bytes = fs::read(path).ok()?;
        if bytes.is_empty() {
            return None;
        }
        Self::load(&bytes)
    }

    pub fn context_length(&self) -> usize {
        self.config.context
    }

    pub fn attribution(&self) -> &str {
        &self.attribution
    }

    pub fn license(&self) -> &str {
        &self.license
    }

    /// 模型给这个字符（Unicode 码点）的 token id。
    pub fn token(&self, character: char) -> Option<u32> {
        self.index.get(&(character as u32)).copied()
    }

    fn encode(&self, text: &str) -> Vec<u32> {
        decode_utf8(text.as_bytes()).into_iter().map(|ch| self.index.get(&ch).copied().unwrap_or(UNK)).collect()
    }

    fn embed(&self, tokens: &[u32], width: usize, offset: usize) -> Vec<f32> {
        let n_embd = self.config.n_embd;
        let mut x = vec![0.0f32; tokens.len() * n_embd];
        let mut token_row = vec![0.0f32; n_embd];
        for (step, &id) in tokens.iter().enumerate() {
            let token = (id as usize).min(self.config.vocab - 1);
            // 每个批行的位置都从 `offset` 重新开始：它们各自是同一份缓存前缀的续写。
            let within = if width == 0 { step } else { step % width };
            let position = (offset + within).min(self.config.context - 1);
            self.token.row_into(token, n_embd, &mut token_row);
            let position_row = &self.position[position * n_embd..(position + 1) * n_embd];
            let row = &mut x[step * n_embd..(step + 1) * n_embd];
            for i in 0..n_embd {
                row[i] = token_row[i] + position_row[i];
            }
        }
        x
    }

    #[allow(clippy::too_many_arguments)]
    fn attention(&self, q: &[f32], k: &[f32], v: &[f32], batch: usize, width: usize, past_k: Option<&[f32]>, past_v: Option<&[f32]>, past_len: usize) -> Vec<f32> {
        let n_embd = self.config.n_embd;
        let heads = self.config.n_head;
        let head_width = n_embd / heads;
        let scale = 1.0f32 / (head_width as f32).sqrt();
        let past_len = if past_k.is_none() || past_v.is_none() { 0 } else { past_len };
        let mut out = vec![0.0f32; batch * width * n_embd];
        let mut scores = vec![0.0f32; past_len + width];
        for row in 0..batch {
            let base_row = row * width;
            for step in 0..width {
                // 因果遮罩分两段：整个共享前缀，再是本行到 `step` 为止的自己的 token。行与行互不可见。
                let visible_self = step + 1;
                let visible = past_len + visible_self;
                for head in 0..heads {
                    let base = head * head_width;
                    let query = &q[(base_row + step) * n_embd + base..(base_row + step) * n_embd + base + head_width];
                    let mut highest = f32::NEG_INFINITY;
                    for j in 0..past_len {
                        let key = &past_k.unwrap()[j * n_embd + base..j * n_embd + base + head_width];
                        let s = dot(query, key) * scale;
                        scores[j] = s;
                        highest = highest.max(s);
                    }
                    for j in 0..visible_self {
                        let key = &k[(base_row + j) * n_embd + base..(base_row + j) * n_embd + base + head_width];
                        let s = dot(query, key) * scale;
                        scores[past_len + j] = s;
                        highest = highest.max(s);
                    }
                    let mut sum = 0.0f32;
                    for score in scores.iter_mut().take(visible) {
                        *score = (*score - highest).exp();
                        sum += *score;
                    }
                    let target = &mut out[(base_row + step) * n_embd + base..(base_row + step) * n_embd + base + head_width];
                    for j in 0..past_len {
                        let weight = scores[j] / sum;
                        let value = &past_v.unwrap()[j * n_embd + base..j * n_embd + base + head_width];
                        for i in 0..head_width {
                            target[i] += weight * value[i];
                        }
                    }
                    for j in 0..visible_self {
                        let weight = scores[past_len + j] / sum;
                        let value = &v[(base_row + j) * n_embd + base..(base_row + j) * n_embd + base + head_width];
                        for i in 0..head_width {
                            target[i] += weight * value[i];
                        }
                    }
                }
            }
        }
        out
    }

    fn feed_forward(&self, x: &mut [f32], block: &Block) {
        let n_embd = self.config.n_embd;
        let rows = x.len() / n_embd;
        let normed = layer_norm(x, rows, &block.ln2_weight, &block.ln2_bias, n_embd);
        let mut hidden = block.fc_weight.linear(&normed, &block.fc_bias, n_embd, 4 * n_embd);
        for value in &mut hidden {
            *value = gelu(*value);
        }
        let out = block.out_weight.linear(&hidden, &block.out_bias, 4 * n_embd, n_embd);
        for (xi, oi) in x.iter_mut().zip(&out) {
            *xi += *oi;
        }
    }

    /// 在位置 `[past_len, past_len+width)` 上跑 `batch` 行、每行 `width` 个 token（行主序，调用方补齐等长），每行还 attend 到 `past`。
    /// 返回最后一层 LayerNorm 之后的隐状态 `[batch*width, n_embd]`。`record` 非空时把这次产生的 K/V 记进去（只能 batch == 1）。
    fn forward(&self, tokens: &[u32], batch: usize, width: usize, past: Option<&PrefixCache>, mut record: Option<&mut PrefixCache>) -> Vec<f32> {
        let n_embd = self.config.n_embd;
        let rows = batch * width;
        let past_len = past.map_or(0, |p| p.length);
        let mut x = self.embed(tokens, width, past_len);
        if let Some(record) = record.as_deref_mut() {
            record.keys = vec![Vec::new(); self.blocks.len()];
            record.values = vec![Vec::new(); self.blocks.len()];
            record.length = past_len + width;
        }
        for (layer, block) in self.blocks.iter().enumerate() {
            let normed = layer_norm(&x, rows, &block.ln1_weight, &block.ln1_bias, n_embd);
            let qkv = block.qkv_weight.linear(&normed, &block.qkv_bias, n_embd, 3 * n_embd);
            let (mut q, mut k, mut v) = (vec![0.0f32; rows * n_embd], vec![0.0f32; rows * n_embd], vec![0.0f32; rows * n_embd]);
            for row in 0..rows {
                let source = &qkv[row * 3 * n_embd..(row + 1) * 3 * n_embd];
                q[row * n_embd..(row + 1) * n_embd].copy_from_slice(&source[..n_embd]);
                k[row * n_embd..(row + 1) * n_embd].copy_from_slice(&source[n_embd..2 * n_embd]);
                v[row * n_embd..(row + 1) * n_embd].copy_from_slice(&source[2 * n_embd..]);
            }
            let (past_k, past_v) = match past {
                Some(p) if past_len != 0 => (Some(p.keys[layer].as_slice()), Some(p.values[layer].as_slice())),
                _ => (None, None),
            };
            let attended = self.attention(&q, &k, &v, batch, width, past_k, past_v, past_len);
            let projected = block.proj_weight.linear(&attended, &block.proj_bias, n_embd, n_embd);
            for (xi, pi) in x.iter_mut().zip(&projected) {
                *xi += *pi;
            }
            self.feed_forward(&mut x, block);
            if let Some(record) = record.as_deref_mut() {
                record.keys[layer] = k;
                record.values[layer] = v;
            }
        }
        layer_norm(&x, rows, &self.final_weight, &self.final_bias, n_embd)
    }

    fn build_prefix(&self, tokens: &[u32]) -> PrefixCache {
        let mut cache = PrefixCache::default();
        if tokens.is_empty() {
            return cache;
        }
        self.forward(tokens, 1, tokens.len(), None, Some(&mut cache));
        cache
    }

    /// 一个隐状态的整词表 log-softmax。
    fn distribution(&self, hidden: &[f32]) -> Vec<f32> {
        let n_embd = self.config.n_embd;
        let vocab = self.config.vocab;
        let mut logits = vec![0.0f32; vocab];
        let mut highest = f32::NEG_INFINITY;
        for (row, slot) in logits.iter_mut().enumerate() {
            let logit = self.token.dot_row(hidden, row, n_embd);
            *slot = logit;
            highest = highest.max(logit);
        }
        let mut sum = 0.0f32;
        for &logit in &logits {
            sum += (logit - highest).exp();
        }
        let offset = highest + sum.ln();
        for logit in &mut logits {
            *logit -= offset;
        }
        logits
    }

    /// 对每个隐状态，目标 token 在该状态分布下的 log P；目标是 `NO_TARGET` 的行跳过，报 0。
    fn target_log_probs(&self, hidden: &[f32], rows: usize, targets: &[u32]) -> Vec<f32> {
        let n_embd = self.config.n_embd;
        let vocab = self.config.vocab;
        let active_rows: Vec<usize> = (0..rows).filter(|&row| targets[row] != NO_TARGET).collect();
        let mut out = vec![0.0f32; rows];
        if active_rows.is_empty() {
            return out;
        }
        let mut logits = vec![0.0f32; active_rows.len() * vocab];
        let mut done = false;
        #[cfg(target_arch = "x86_64")]
        if self.token.quantized && avx2_fma_available() {
            let mut active_hidden = vec![0.0f32; active_rows.len() * n_embd];
            for (slot, &row) in active_rows.iter().enumerate() {
                active_hidden[slot * n_embd..(slot + 1) * n_embd].copy_from_slice(&hidden[row * n_embd..(row + 1) * n_embd]);
            }
            // SAFETY: 已检测到 AVX2 与 FMA；`tok.weight` 的几何在载入时校验过。
            unsafe { linear_int8_avx2(&active_hidden, &self.token.ints, &self.token.scales, None, active_rows.len(), n_embd, vocab, &mut logits) };
            done = true;
        }
        if !done {
            // 标量路径：一行词表只展开一次，供所有活动行共用。
            let mut scratch = vec![0.0f32; if self.token.quantized { n_embd } else { 0 }];
            for id in 0..vocab {
                let (weights, scale): (&[f32], f32) = if self.token.quantized {
                    for (i, &q) in self.token.ints[id * n_embd..(id + 1) * n_embd].iter().enumerate() {
                        scratch[i] = f32::from(q);
                    }
                    (&scratch, self.token.scales[id])
                } else {
                    (&self.token.floats[id * n_embd..(id + 1) * n_embd], 1.0f32)
                };
                for (slot, &row) in active_rows.iter().enumerate() {
                    logits[slot * vocab + id] = dot(&hidden[row * n_embd..(row + 1) * n_embd], weights) * scale;
                }
            }
        }
        for (slot, &row) in active_rows.iter().enumerate() {
            let source = &logits[slot * vocab..(slot + 1) * vocab];
            let mut highest = f32::NEG_INFINITY;
            for &v in source {
                highest = highest.max(v);
            }
            let mut sum = 0.0f32;
            for &v in source {
                sum += (v - highest).exp();
            }
            out[row] = source[targets[row] as usize] - (highest + sum.ln());
        }
        out
    }

    /// log P(下一个字符 | 文本)，整词表，按 token id 下标。
    pub fn next_log_probabilities(&self, text: &str) -> Vec<f32> {
        let mut tokens = vec![BOS];
        let encoded = self.encode(text);
        let room = self.config.context - 1;
        let start = encoded.len().saturating_sub(room);
        tokens.extend_from_slice(&encoded[start..]);
        let hidden = self.forward(&tokens, 1, tokens.len(), None, None);
        let n_embd = self.config.n_embd;
        self.distribution(&hidden[hidden.len() - n_embd..])
    }

    /// 每个文本一项：`Σ log P(字符 | 前文及文本里它前面的字符)`。词表外的字符按 `<unk>` 算，文本不会被丢掉。
    ///
    /// 整批一次跑完：前文（去掉最后一个 token）建成 K/V 缓存，前文不变时跨调用复用，各文本补齐等长后一起跑。
    /// 空 `texts` 返回空；文本或前文放不进窗口时从左边截。
    pub fn score_sentences(&self, context: &str, texts: &[String]) -> Vec<f64> {
        if texts.is_empty() {
            return Vec::new();
        }
        let limit = self.config.context;
        if limit < 2 {
            return vec![0.0; texts.len()];
        }
        // 每个候选的序列是 <bos> + 前文 + 候选。共享部分在最后一个 token 处切开：这个 token 打头每个批行（它产生的分布给候选
        // 的第一个字符打分），只有它之前的部分进缓存，因为只有那部分固定。
        let mut full = vec![BOS];
        full.extend(self.encode(context));

        let mut tails: Vec<Vec<u32>> = Vec::with_capacity(texts.len());
        let mut longest = 0;
        for text in texts {
            let mut ids = self.encode(text);
            // 候选比窗口减去打头 token 还长时留尾巴：那是剩下的前文还能提供信息的部分。
            if ids.len() > limit - 1 {
                ids.drain(..ids.len() - (limit - 1));
            }
            longest = longest.max(ids.len());
            tails.push(ids);
        }
        if longest == 0 {
            return vec![0.0; texts.len()];
        }
        // 候选的最后一个 token 只当目标，它的隐状态不预测任何要打分的字符。
        let width = longest;

        let last = *full.last().unwrap();
        let mut head: Vec<u32> = full[..full.len() - 1].to_vec();
        // 保持原来的前文上限，去掉没用的最后一个 token 不改变分数。
        let room = if limit > longest + 1 { limit - (longest + 1) } else { 0 };
        if head.len() > room {
            head.drain(..head.len() - room);
        }

        let batch = tails.len();
        // 短行用 token 0 补齐；补齐位置的目标是 NO_TARGET，不会被读。
        let mut flat = vec![0u32; batch * width];
        let mut targets = vec![NO_TARGET; batch * width];
        for row in 0..batch {
            flat[row * width] = last;
            for i in 0..tails[row].len() {
                // 位置 i 预测 token i+1，所以打头 token 的位置带第一个字符。
                targets[row * width + i] = tails[row][i];
                if i + 1 < tails[row].len() {
                    flat[row * width + 1 + i] = tails[row][i];
                }
            }
        }

        let scored = {
            let mut guard = self.cache.lock().unwrap();
            if guard.0 != head {
                guard.1 = self.build_prefix(&head);
                guard.0 = head;
            }
            let hidden = self.forward(&flat, batch, width, Some(&guard.1), None);
            self.target_log_probs(&hidden, batch * width, &targets)
        };

        (0..batch)
            .map(|row| {
                let mut total = 0.0f64;
                for i in 0..tails[row].len() {
                    total += f64::from(scored[row * width + i]);
                }
                total
            })
            .collect()
    }
}

// ---------------------------------------------------------------------------------------------
// 重排
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
pub struct RerankOptions {
    /// 采纳模型意见的比例：0 保持词格顺序，1 完全取代词格分。
    pub lambda: f64,
    /// 重打分的路径数；词格自己的顺序在头几条以后本来就是噪声。
    pub max_paths: usize,
    /// 给模型看的前文字数（字符，不是字节）。
    pub context_chars: usize,
}

impl Default for RerankOptions {
    fn default() -> Self {
        Self { lambda: 0.5, max_paths: 12, context_chars: 64 }
    }
}

/// 文本末尾 `count` 个 Unicode 字符（不是字节）。
pub fn last_characters(text: &str, count: usize) -> &str {
    if count == 0 {
        return "";
    }
    let bytes = text.as_bytes();
    let mut seen = 0;
    let mut start = bytes.len();
    for i in (0..bytes.len()).rev() {
        if bytes[i] & 0xC0 == 0x80 {
            continue; // 后续字节
        }
        start = i;
        seen += 1;
        if seen == count {
            break;
        }
    }
    &text[start..]
}

/// `sentences` 应该展示的次序（下标）。`static_scores` 是词格路径分（log10）。模型没法参考时（不足两句、λ 为零、长度不一致）
/// 返回空，调用方保持自己的顺序：只重排一部分，会把两套不可比的排名混在一起。只对前 `max_paths` 句重打分，其余保持相对次序排在后面。
pub fn rerank_order(model: &SentenceModel, context: &str, sentences: &[String], static_scores: &[f64], options: &RerankOptions) -> Vec<usize> {
    if sentences.len() < 2 || sentences.len() != static_scores.len() || options.lambda == 0.0 {
        return Vec::new();
    }
    let scored_count = sentences.len().min(options.max_paths);
    if scored_count < 2 {
        return Vec::new();
    }
    let neural = model.score_sentences(last_characters(context, options.context_chars), &sentences[..scored_count]);
    if neural.len() != scored_count {
        return Vec::new();
    }
    let combined: Vec<f64> = (0..scored_count)
        .map(|i| {
            let statik = static_scores[i] * LOG10_TO_NATS;
            statik + options.lambda * (neural[i] - statik)
        })
        .collect();
    let mut order: Vec<usize> = (0..sentences.len()).collect();
    // 稳定排序：两套模型打分相同的句子保持词格的顺序，没重打分的尾巴也保持原样。
    order[..scored_count].sort_by(|&a, &b| combined[b].partial_cmp(&combined[a]).unwrap_or(std::cmp::Ordering::Equal));
    order
}
