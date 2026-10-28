//! 水杉输入法引擎的 Rust 核心。
//!
//! 不是一次重写，而是按块替换 `engine/` 里的 C++：每替换一块，就对着 `engine/contracts/golden/`
//! 里现引擎的输出核对。现在这一块是「全拼词库查询」：把一串拼音切成音节，再到 `msime.db`
//! 里按音节数和首字母分表取词，顺序和 C++ 的 `QuanpinDictionary` 一致。
//!
//! 简拼和没打完的音节（`nh`、`nih`、`zhongg`）也在这里：`Dictionary::query` 按 C++ 的顺序依次试
//! 精确键、键前缀、混合简拼、纯简拼。
//!
//! 还没做的（仍在 C++）：纠错、模糊音、整句词格、联想、用户词库调频。

pub mod autocorrect;
pub mod dict;
pub mod fuzzy;
pub mod segment;
pub mod syllables;
pub mod table;

pub use dict::{dedupe_by_text, Dictionary, Entry, KeyedEntry};
pub use segment::{cut_min_segments, cut_min_segments_prefix};
pub use table::table_name;
