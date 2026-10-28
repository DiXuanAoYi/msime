//! 水杉输入法引擎的 Rust 核心。
//!
//! 不是一次重写，而是按块替换 `engine/` 里的 C++：每替换一块，就对着 `engine/contracts/golden/`
//! 里现引擎的输出核对。现在这一块是「全拼词库查询」：把一串拼音切成音节，再到 `msime.db`
//! 里按音节数和首字母分表取词，顺序和 C++ 的 `QuanpinDictionary` 一致。
//!
//! 还没做的（仍在 C++）：简拼、纠错、模糊音、整句词格、联想、用户词库调频。

pub mod dict;
pub mod segment;
pub mod syllables;
pub mod table;

pub use dict::{Dictionary, Entry};
pub use segment::cut_min_segments;
pub use table::table_name;
