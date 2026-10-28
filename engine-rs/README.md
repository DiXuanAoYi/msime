# engine-rs

水杉输入法引擎的 Rust 核心，按块替换 `engine/` 里的 C++。**不是一次重写**：现在产品仍然跑 C++，Rust 这边每做完一块，就对着 `engine/contracts/golden/` 里现引擎的输出核对，过了才算数。

## 已经替换的块

| 块 | 位置 | 怎么核对 |
|---|---|---|
| 音节表 | `msime-core/src/syllables.rs` | `tests/syllables_match_cpp.rs`：和 C++ 源码里的列表逐项比 |
| 全拼切分（最少音节，平局取更短首音节） | `src/segment.rs` | 单元测试，加黄金样本 |
| 分表命名与键 | `src/table.rs` | 单元测试，规则来自 `engine/contracts/dictionary/format.json` |
| 词库精确查询 | `src/dict.rs` | `tests/golden.rs`：44 条输入、475 个词，相对顺序和 C++ 一致 |
| 简拼、没打完的音节（精确键 → 键前缀 → 混合简拼 → 纯简拼的查询顺序，按词去重） | `src/segment.rs`、`src/dict.rs` | `tests/golden.rs`：`n`、`nh`、`zg`、`srf`、`zgr`、`wm`、`jt`、`mt`、`bj`、`sh` 等输入的相对顺序和 C++ 一致 |

## 还在 C++ 里的块（按建议顺序）

1. 模糊音与纠错
2. 整句词格（Google 解码器、kenlm）
3. 用户词库、调频与学习
4. 辅助码、双拼、五笔、日语
5. 英文与本地模式

## 运行测试

```powershell
cd engine-rs
cargo test
```

`tests/golden.rs` 需要随包词库：默认读 `installer/app_data/msime.db`（`Prepare-PackageFiles.ps1` 之后就有），也可以设 `MSIME_DB` 指向别处；两者都没有就跳过并打印原因。

## 约定

- 词库只读打开，不写、不产生 -wal 文件。
- 用户词库的写入走事务和回放日志，要等到那一块再碰，不要提前自己写。
- C++ 的行为是规格：发现 Rust 和 C++ 不一致，先查是不是 C++ 的有意设计（`engine/AGENTS.md` 里有几条踩过的坑），不要自作主张“修正”。
