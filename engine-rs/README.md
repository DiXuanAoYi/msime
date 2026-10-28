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
| 模糊音（十一条规则、近音路径、与精确候选按权重混排） | `src/fuzzy.rs`、`src/dict.rs` | `tests/golden.rs`：34 条输入、1012 个词，其中 2581 个模糊词，相对顺序和 C++ 一致 |
| 纠错：错拼表（换位、邻键、漏键、多键）和静态表上的 k-best 切分 | `src/autocorrect.rs` | `tests/autocorrect_table_matches_cpp.rs`：四张表（806 / 3728 / 194 / 11044 个错拼键）和 `engine/quanpin/autocorrect_table.h` 逐项相同；`tests/golden.rs`：7 条输入、210 个词，首选词和相对顺序一致 |

## 还在 C++ 里的块（按建议顺序）

1. 整句词格（Google 解码器、kenlm）
2. 用户词库、调频与学习
3. 辅助码、双拼、五笔、日语
4. 英文与本地模式

### 纠错里还留在 C++ 的部分

Rust 只有“静态错拼表 + k-best 切分”。下面这些仍是 C++ 的行为，黄金样本里对应的输入不拿来对照（`tests/golden.rs` 的 `AUTOCORRECT_CPP_ONLY` 逐条列出）：

- **简拼形状闸**：像简拼的输入（`nhao` = n+hao）不纠错，保留 您好；
- **合法输入闸**：本身合法的输入（`tianqi`）主切不动，纠错读法只在整句打分里参与；
- **表外生成读法**：任意字母替换/插入，只有静态表完全解释不了才出场（`nihap` 的 你还）；
- **头部纠错 + 简拼尾**、**纠错读法与字面整词的混排仲裁**。

### 发现的疑似缺陷（C++）

`niihao` 只开“多键”（`autocorrect=8`）时候选为空；`nihaoo`、`shangg` 同样只开多键是正常的，`niihao` 开邻键或全开也正常。黄金样本 `expected_options.jsonl` 已把这个行为记成基线，修之前先确认是不是有意为之。

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
