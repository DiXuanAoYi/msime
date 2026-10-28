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
| 调频算法（选中项的目标名次、权重中点、整排重排、选中次数计数；只算该写的权重，不碰 SQLite） | `src/ranking.rs` | `tests/ranking_golden.rs`：`engine/contracts/golden/ranking_cases.tsv` 的 253 个用例（含权重相等、相邻、超上限、接近 i64 上限、混读音、显示顺序不等于权重顺序、连选触发次数），每行最终权重和真实 `adjust_candidate_ranking` 逐个相同；基线由 `msime_ranking_probe` 在合成词库上生成 |
| 双拼：九套内置方案表、按键解码、贪心分段、转全拼、全码辅助判断 | `src/shuangpin.rs` | `tests/shuangpin_helpcode_golden.rs`：方案表原样相同；9 套方案 × 756 个码的解码相同；885 条随机串的分段、转全拼、全码辅助相同。方案表由脚本从 C++ 源码抽取，不是手抄 |
| 辅助码：码表加载、取码、单码匹配与重排、双码匹配与筛选、大写结尾的模式判断 | `src/helpcode.rs` | 同上：6 套内置码表（共约 7.4 万行）、1621 个词的取码和匹配、重排与筛选逐项相同；基线由 `msime_helpshuang_probe` 导出 |

## 还在 C++ 里的块（按建议顺序）

1. 整句词格（Google 解码器、kenlm）
2. 用户词库的存取：操作日志与重放、固定位置、新词学习、快捷短语/英文/日期时间的槽位（调频的权重算法已移植，剩下的都绑着 SQLite 事务）
3. 五笔、日语；双拼里的用户自定义方案（`BuildCustomShuangpinProfile`）和双拼词库查询（`shuangpin_query.cpp`、`shuangpin_dictionary.cpp`）；辅助码在候选窗里的实际接线
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

## 重新生成调频基线

改了 `engine/tools/gen_ranking_cases.py` 或 C++ 的调频逻辑后：

```powershell
python engine/tools/gen_ranking_cases.py engine/contracts/golden/ranking_cases.tsv
cmake --build server/build-release/engine --config Release --target msime_ranking_probe
server\build-release\engine\Release\msime_ranking_probe.exe engine\contracts\golden\ranking_cases.tsv $env:TEMP\ranking-scratch engine\contracts\golden\expected_ranking.jsonl
anking_cases.tsv $env:TEMP
anking-scratch engine\contracts\golden\expected_ranking.jsonl
```

基线变了要在提交说明里写清为什么变。

## 重新生成双拼与辅助码基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_helpshuang_probe
server\build-release\engine\Release\msime_helpshuang_probe.exe engine\helpcode engine\contracts\golden\expected_helpshuang.jsonl
```

改了 `engine/shuangpin/shuangpin_profile.cpp` 的方案表后，要同时重新抽取 `src/shuangpin.rs` 末尾的方案函数：`python engine-rs/tools/extract_shuangpin_profiles.py`（脚本从 C++ 源码生成，没有手抄）。
