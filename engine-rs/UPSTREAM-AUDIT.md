# 上游引擎改动审计（第 0 步）

对象：`origin/develop`（metasequoiaime/msime-windows）在本分支分叉点之后对引擎的改动。只读审计，没有改任何引擎代码。

| | |
|---|---|
| 分叉点 | `5d6eaa5`（2026-10-05） |
| 上游现状 | `e255232`（2026-10-28），分叉点之后 355 个提交，本分支另有 92 个 |
| 动到引擎核心目录的非合并提交 | 42 个（core、quanpin、shuangpin、japanese、english、googlepinyinime-rev、user_dictionary、local_modes、neural、wubi、direct_helpcode、ngram） |
| 引擎核心 diff | 约 1.7 万行，其中 `quanpin/autocorrect_table.h` 一个文件就是 9000 行（生成表） |

## 关键事实

- **Rust 移植与本分支的 C++ 逐位一致，不与上游一致。** 摘取任何一个上游改动 = 先改 C++、重导 golden 基线、再改 Rust 并重新过 golden。成本由 C++ 改动的大小决定，不是由补丁行数决定。
- **整体应用冲突面不大但集中在最核心处。** 把上游引擎核心的整段 diff 用 `git apply --check -3` 试：16 个文件有冲突，全在 `quanpin/`（dictionary、query、utils、word_lattice、lattice_rerank）、`shuangpin/`（engine、dictionary）、`english_dictionary`、`input_session_composition`、`sentence_association_options`、`word_item`、`romaji_converter`。逐提交试，42 个里只有 `6e15526`（常驻内存/去重性能）不能直接套。也就是说单点摘取多数干净，整体合入冲突集中在拼音候选主干。
- **上游删了本分支移植过的东西。** `1671c3a` 删除 `core/nine_key_session.*` 及其测试（Rust 的 `NineKeySession` 没有上游对应物）；`d414268` 删除 `local_modes/unicode_query.*`，换成 `radical_query`（U 模式改为部件拆字反查）。跟进这两处会让对应 Rust 模块作废或重写。

## 分档

### 一档：小、独立、值得摘（每个 4–80 行）

| 提交 | 内容 | Rust 侧 |
|---|---|---|
| `89a2bea` | 单字母输入不再把字面整词候选顶到首位 | 要改 |
| `f71ac56` | 前缀候选不进整词优先 | 要改 |
| `93c7b81` | 日语 `nn` 当作单个拨音 | 要改（romaji） |
| `0bad4c1` | 日语分段选词吞掉后续输入 | 要改（日语分段） |
| `777fb2a`、`29c8917` | 重复声母简拼保护、句首声母混拼 | 要改 |
| `9a08042` | 日语预测紧跟整读音转换 | 要改 |
| `d912a92` | Google 解码器 `get_lpis` 去重越界写（`lma_buf[-1]`，双拼 `zjle` 崩溃） | **不用改**：Rust 只移植了整句解码（`best_sentence`），没有 `get_lpis` 候选扩展，不存在这个越界；C++ 侧仍要修，但那是 Windows 端的事 |

### 二档：候选质量，中等大小

`a60e294`（整句联想按 P(读音|字) 压多音字生僻读音，125 行）、`7ed4f2e`（回溯切分判定合法全拼，55）、`fc90cb1`（简拼否决权，纠错不再劫持 `nbn`，39）、`d270395`（简拼尾放开静态邻键纠错，92）、`1499027`（光标在拼音首单元内部不收候选窗，27）、`23bfdd4`（符号/emoji 混输，57）、`93f95d1`（emoji/颜文字精确命中优先，252）、`692bed7`（出厂词库换过时回放不用旧量级权重，178）。

### 三档：大块且互相依赖的纠错/词格改写

`60c2b26`（多字纠错收窄，3591 行含表）、`e38626d`（长句手误走容错槽位词格仲裁，1048）、`7d45c51`、`5092125`（漏字/多字纠错独立开关）、`2f63067`（纠错生成表改编译期常量，3675）、`cf07b77`（等音节数字面切分按整句打分裁决，367）、`b434240`（模糊读音按词频混排，218）、`d495ea2`（octagram 加成收敛，217）、`36ab259`（原样混拼优先级，110）。它们改的是同一批函数（`quanpin_dictionary`、`quanpin_utils`、`word_lattice`），要按序整体摘，不能挑着取。工作量相当于重新移植一次拼音候选主干，需要重导 InputSession/Session 两份约 26 MB 基线。

### 四档：Windows/产品功能，暂不跟

U 模式部件反查（`d414268`）、S 模式符号输入（`3cd429b`）、五笔 T/V 模式（`456b23e`）、按上文重排词库候选（`551d5fa`，实验性）、用户词库 checkpoint 挪到空闲与锁等待收进客户端回复窗口（`637f6b7`、`1691a61`，针对 Server 的 IPC 时限，Rust 每次新开连接，不适用）、常驻内存/去重性能（`6e15526`）、双拼间接辅助码（`5de73de`）、翻页/方向键/顿号等界面交互（`58a27f3`、`c0a9d37`、`75d8c71`）。

## 建议

1. 先做一档（7 项里 6 项要改 Rust）：每项都小，各自加定向序列进探针，C++ 与 Rust 同提交，风险低。
2. 二档按质量收益挑：`a60e294`、`7ed4f2e`、`fc90cb1`、`d270395` 优先。
3. 三档单独立项。要么整体按序摘（等于再来一轮"拼音候选主干"的移植 + 基线重导），要么承认 Rust 核心基于 10-05 的拼音行为，把差异当作已知差距记录。不要零散摘三档里的单个提交。
4. 四档不进跨平台核心，等产品侧定稿再说；`nine_key_session` 上游已删，Rust 的九键保持原样但不再有对照源，后续不扩。

## 摘取结果

### 一档

逐项核对本分支实际内容后，一档 8 项里 5 项早已在树里（`9eaffd6` 带进来的是演进过的同类实现），审计时按"上游 diff 能不能套"判断高估了待办：

| 提交 | 结果 |
|---|---|
| `29c8917`、`777fb2a` | 已有：`looks_like_jianpin_spelling` 含句首声母规则，Rust 在 `autocorrect.rs` |
| `93c7b81` | 已有：`nn` 收成单个拨音（还多了 konnichiwa 例外） |
| `0bad4c1` | 已有：日语分段选词续接 `replace_japanese_raw_input`、`reading_length` |
| `9a08042` | 已有：`LongerPrefixLemmas` |
| `89a2bea`、`f71ac56` | **新摘**：C++ 与 Rust 同改（`quanpin_dictionary`：字面整词要求匹配码覆盖完整输入；单字母输入不提升字面整词，打 `e` 首位不再是「饿」） |
| `d912a92` | **新摘**，只改 C++（Rust 没有 `get_lpis`） |

基线：capi、ime_session、pinyin_provider、input_session、session 五份按新 C++ 重导；`expected_quanpin_dictionary.jsonl` 只覆盖受影响的 9 行。全拼/双拼词库探针的 `sources` 记录（整句来源）曾在重导时与旧基线差 61 行，原因已查明：探针从**输出文件所在目录**取合成语法模型 `synthetic.gram`，我把输出写到了临时目录，万象来源静默失效。输出写进 `engine/contracts/golden/` 重跑，与基线逐字节一致；因此 `expected_quanpin_dictionary.jsonl` 现在就是一次完整的正确重导（受这次摘取影响的是 9 行）。两个词库探针现在缺 `synthetic.gram` 会直接退出，README 也写了。

### 二档

| 提交 | 结果 |
|---|---|
| `7ed4f2e`、`fc90cb1` | 基本已有（逐块核对只剩各一处措辞差异） |
| `1499027` | 新摘：光标落在首单元内部时前缀取整到首单元末尾（`prefix_end == 0` 只留给 `caret == 0`） |
| `93f95d1` | 新摘：emoji/颜文字精确命中排在前缀命中之前（`local_database` 的 `query_ranked_entries`，Rust 在 `local.rs`） |
| `a60e294` | 新摘：词格对单字行按 P(读音\|字) 扣分（全拼、双拼共用；Rust 在 `dict.rs` 的 `load_char_reading_totals`） |
| `d270395` | 新摘：简拼尾组合纠错 head 用完整掩码，生成式最优切分不组合，有自洽明文读法时整个组合让位 |
| `119c439` | 新摘（`692bed7` 的前置，审计时漏了）：日文候选标 `JapaneseRomaji` 方案，不再被当拼音键写进拼音日志；调频没改到行不记日志；启动清理与回放跳过带假名的自动学习条目 |
| `692bed7` | 新摘：回放用 `dictionary-manifest.json` 里 `msime.db` 的 SHA-256 认出厂词库，换库时自动调频行只在高过新出厂权重时写回并改记日志；`record_weight_edit`。用户词库探针新增 80 个换库场景（清单有无、写坏、日文残留） |
| `23bfdd4` | 只摘引擎部分：`query_mixed_emoji`（混输里 emoji 只认整串精确匹配）。S 模式符号输入与词库构建器属 Server/词库侧 |

### 三档

- `b434240`、`d495ea2`、`36ab259`、`cf07b77`：逐块核对大部分已在树里（`9eaffd6` 带进来的是演进过的同类实现），没有新摘。
- `60c2b26`、`e38626d`、`7d45c51`、`5092125`、`2f63067`（纠错改写链）**没摘**。试套这五个提交的引擎部分，`quanpin_utils.h`、`quanpin_dictionary.cpp`、`quanpin_query.cpp`、`input_session*.{h,cpp}`、`engine.cpp` 等都冲突；`e38626d` 单独就在拼音词库里新增约 550 行、词格 100 行、纠错工具 200 行（长句手误超预算时的容错槽位词格仲裁），`60c2b26` 还重写了 1500 多行生成表。这相当于把全拼纠错层整体重做一遍（C++ 合并加 Rust 重移植加 `autocorrect_table` 与基线重导），收益是长句手误的召回，风险集中在最核心的拼音主干。留作单独立项。

### 四档

未动。

