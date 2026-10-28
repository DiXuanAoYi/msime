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
| 整句词格：词图构建、Viterbi 束搜索、n-best 回溯去重、字约束、字级搭配回调、读音先验、与已有候选合并的全部来源规则（词格/搭配/两种神经重排/Unigram 共识） | `src/lattice.rs` | `tests/lattice_golden.rs`：150 个无模型解码场景（484 条路径，句子、键、词序、输入域逐项相同，分数误差 1e-9 内）、80 个带真实 `sc.lm` 的解码场景（257 条路径，含词库平局项与读音先验）和 150 个合并场景逐项相同；基线由 `msime_lattice_probe` 导出，带着解码时实际查询到的词表回放 |
| kenlm 二进制语言模型读取器（`sc.lm`，`-q 4 -a 22` 的量化 trie：词表哈希、量化表、Bhiksha 指针压缩、位压缩 n-gram、回退） | `src/kenlm.rs` | `tests/kenlm_golden.rs`：3000+ 个词的下标和单词分数、700 句（含 300 条沿真实二元/三元组走的贪心链）逐词累计分数与 C++ **逐位相同**；`KenLm` 实现 `lattice::LanguageModel`，上一行的带模型词格场景就是用它回放的。基线由 `msime_lm_probe` 导出；没有 `sc.lm`（`installer/app_data/sc.lm` 或 `MSIME_LM`）时这两组测试跳过 |
| 日语罗马字 ↔ 假名（165 项映射表、促音/拨音/长音规则、没打完的尾巴、前缀对应的假名集合、假名转回罗马字） | `src/romaji.rs`、`src/romaji_table.rs` | `tests/wubi_japanese_golden.rs`：全部 ≤3 位（含 `'`、`-`）和 4 位字母共 479740 个输入的转换结果、475254 个前缀的假名集合，与 C++ 逐项相同（滚动哈希）；1500 条随机输入、944 条假名串逐项相同。映射表由脚本从 C++ 源码抽出，不是手抄 |
| 五笔：按键方案（四码上限、混输放宽长度、z 通配）和 `wubi86` 码表查询（精确码优先、权重序、同字去重、GLOB 通配、50 行截断） | `src/wubi.rs` | 同上：350 个按键序列的逐步预编辑、请求和按键记录相同；1029 次码表查询（含 211 次通配）的候选、权重、次序相同 |
| 日语：按键方案、Mozc 词典模型读取与词条查询（精确/前缀/更长前缀/接续）、整句解码、矩阵搜索、候选提供者的拼装与云候选插入 | `src/japanese.rs` | 同上：300 个日语按键序列；1500 对连接成本；124 个假名前缀；290 个输入（230 个完整读音，其中 169 个有 6 个以上假名、104 个有 10 个以上，含手写的自然句子和“词＋助词＋词尾”拼出的句子；另 60 个没打完的）的词条查询、整句解码、矩阵搜索；721 个输入的完整候选列表；12 个带云候选缓存的查询，全部逐项相同。基线由 `msime_wubi_japanese_probe` 导出，模型 128 万个词条 |
| 本地模式：Unicode（U+XXXX）、V 模式（数字读法/大写/金额/算式）、Shift+T 日期时间（唤醒词与指定日期时间的形状、农历 1900–2100）、表情、颜文字、超级简拼（全拼/双拼）、快捷短语查询与插入位置 | `src/vmode.rs`、`src/datetime.rs`、`src/local.rs`、`src/lunar_table.rs` | `tests/local_modes_golden.rs`：317 个 Unicode 输入、552 个 V 模式输入加 300 组数字读法、900 个日期时间输入（含 252 个农历、556 个指定日期时间）、表情 384 / 颜文字 344 / 超级简拼 560 次查询、90 个快捷短语编码加 300 个插入位置场景，全部逐项相同。农历表由脚本从 C++ 源码抽出；基线由 `msime_local_modes_probe` 导出 |
| 英文词典：前缀补全、中英释义（自定义翻译侧车 > 清简词库 + 行业词包 > 旧表，义项合并去重，可按 ID 禁用词包） | `src/english.rs` | 同上：315 次前缀补全、441 次释义（含禁用组合、191 次多义项合并）、1040 次翻译词包查询逐项相同 |
| octagram `.gram` 语法模型读取器（搭配强度 `query`：上下文窗口、搭配/弱搭配/句尾惩罚；UTF-8 压缩编码；darts-clone 双数组遍历） | `src/gram.rs` | `tests/gram_golden.rs`：真模型约 390MB、另外下载，所以用探针现建一份格式相同的合成模型（`synthetic.gram`，7337 个键，字符池覆盖两字节区边界、变长区、补充平面、ASCII）；300 个文本的编码与码点辅助（含非法字节和 NUL）、2500 次查询（命中 939）的分数和统计与真实 `GramDb` 一致，逐位相同 95% 以上，其余差 1 ulp——MSVC 把 `值 / 10000.0` 编成了乘倒数。基线由 `msime_gram_probe` 导出 |
| 双拼用户自定义方案（布局校验、ue/ve 互补、er 默认 r、四种零声母规则与覆盖、声母/零声母抢键检测、全音节往返检查） | `src/shuangpin.rs`（`build_custom_profile`） | `tests/custom_shuangpin_golden.rs`：探针对九套内置方案随机改动 1200 次（换键、删项、未知项、非法按键、大小写、共用键），1197 个布局的成败、方案表（成功 277 个）和错误信息与真实 `BuildCustomShuangpinProfile` 一致。C++ 布局表是 `unordered_map`，错误先后、抢键谁先到、共用键时解码读成哪个都随遍历顺序而变，所以前半段错误按多重集比、声母冲突只比键、往返段比条数和汇总行；小写后撞名的 3 个布局跳过。基线由 `msime_custom_shuangpin_probe` 导出 |
| 双拼输入串处理：分段、音节边界、转全拼、手动分隔符、有效长度与前缀映射、全码辅助长度、大小写还原；句中辅助码（反引号/分号/大写触发）的解析、装饰、各光标位置“能否接反引号/第二码/分号”的判断；约束匹配与缓存签名 | `src/shuangpin_query.rs`、`src/mid_sentence.rs` | `tests/shuangpin_query_golden.rs`：九套内置方案 × 260 条随机输入串（2340 条，用方案里真能解出音节的两键码拼，掺单键、大写辅助码、反引号段、分隔符、杂字符；含辅助码段 625、双辅助码状态 108、带大写还原 1290），每条十几项输出与真实 `shuangpin::*` 一致，光标逐位置核对。`segment_raw_boundaries` 末尾的 `dedup` 看不出是否有触发路径（去掉后测试仍绿），保留以对齐 C++ |
| 词格的词库查询（跨度只收键完全相等的行；ü 另一种写法归一；模糊近音同批查、每错一个音节扣一份分、按折后权重稳定排序、同字去重、截回上限） | `src/dict.rs`（`Dictionary::lattice_lookup`） | `tests/lattice_lookup_golden.rs`：在随包 `msime.db` 上 1119 个跨度（700 个随机跨度取自词库真有的键并混入 jv/lue 写法、查不到的键和不完整音节，加全部单音节键各一次），随机模糊音规则和上限；条数、内容哈希、前 12 项（键、词、权重、扣分）与真实 `make_lattice_db_lookup` 一致，其中 885 个开了模糊音、156 个带扣分、359 个吃满上限。基线由 `msime_lattice_lookup_probe` 导出 |
| 双拼直接辅助码的拼写图（六种拼写：双拼两键、加一位辅码、两位辅码加斜杠/大写/不带标记、单声母；缩写边剪枝；大写标记严格档与兜底档；只留落在完整路径上的边；唯一路径判断） | `src/spelling_graph.rs` | `tests/spelling_graph_golden.rs`：九套内置方案 × 330 条随机输入串（2970 条，随机斜杠/大写标记开关，五种辅码谓词——空、哈希模 2/3/5、恒真——辅码表用确定的哈希函数代替，两边写同一个），边、分隔符、下一出字位置、唯一路径与真实 `build_spelling_graph` 一致，其中空图 208、唯一路径 1553、辅码边 2880。`single_path` 里 `min(count, 2)` 的上限换成 3 测不出（只有“是否恰好 1 条”起作用，等价变异）。基线由 `msime_spelling_graph_probe` 导出 |
| 直接辅助码的整句解码（在拼写图上铺词、一次 Viterbi 同时定切分与用字；占位边、搭配尾窗、语言模型状态、MSVC 的 `partial_sort` 束截断）和解析器（辅码索引、选切分、改写请求为句中辅助码形状、纯双拼时放行） | `src/direct_lattice.rs`、`src/direct_resolver.rs`、`src/msvc_sort.rs` | `tests/direct_helpcode_golden.rs`：九套内置方案 × 220 条随机输入串。词库、码表、辅码谓词都是哈希合成的（两边同一套，权重互不相同）；1980 次解码（整句完整 779，987 次带真实 `sc.lm`）的音节、整句、键、词、分数逐项一致，词边缓存跑两次结果不变；1980 次解析器调用（改写请求 856，带辅码约束 163，解出整句 14）的是否改写、改写出的请求字段、整句一致。`partial_sort` 的平局次序照 MSVC 的堆实现搬过来（`msvc_sort`，日语解码共用），所以不必回避平局。C++ 的 `QueryRequest` 没移植，改写结果放在 `Rewrite`；跨按键的查词/整句/搭配分缓存只影响速度，没搬。辅码索引回查时“只收单字”的过滤在合成词库里测不出（码表里只有单字，等价变异）。基线由 `msime_direct_helpcode_probe` 导出，要 `sc.lm` |

## 还在 C++ 里的块（按建议顺序）

1. 整句词格剩下的：Google 拼音解码器（`matrixsearch` + `dict_pinyin.dat`）、神经重排；以及全拼/双拼词典层（`quanpin_dictionary.cpp`、`shuangpin_dictionary.cpp`）把各路来源编排成候选列表的那段（词格的词库查询 `make_lattice_db_lookup`、直接辅助码解码与解析器已移植）
2. 用户词库的存取：操作日志与重放、固定位置、新词学习、快捷短语/英文/日期时间的槽位（调频的权重算法已移植，剩下的都绑着 SQLite 事务）
3. 五笔和日语里绑着用户词库的部分（调频、删词、造词写 SQLite）；双拼词库查询（`shuangpin_dictionary.cpp`）；辅助码在候选窗里的实际接线
4. 英文词典与本地模式里要写库的部分：`EnglishDictionary::ensure_schema` / `upsert_gloss`（建表、升级旧库、写入释义）、`local_database.cpp` 的共享连接缓存（只是性能手段，Rust 每次查询开一个只读连接）、`current_local_date_time`（读系统时钟，Rust 的日期时间函数都接受调用方给的“此刻”）

### 纠错里还留在 C++ 的部分

Rust 只有“静态错拼表 + k-best 切分”。下面这些仍是 C++ 的行为，黄金样本里对应的输入不拿来对照（`tests/golden.rs` 的 `AUTOCORRECT_CPP_ONLY` 逐条列出）：

- **简拼形状闸**：像简拼的输入（`nhao` = n+hao）不纠错，保留 您好；
- **合法输入闸**：本身合法的输入（`tianqi`）主切不动，纠错读法只在整句打分里参与；
- **表外生成读法**：任意字母替换/插入，只有静态表完全解释不了才出场（`nihap` 的 你还）；
- **头部纠错 + 简拼尾**、**纠错读法与字面整词的混排仲裁**。

### 整句词格里没有对过的部分

- 对过的：无模型时的启发式打分、惩罚分、字级搭配回调、字约束、束宽、n-best 去重、合并规则；带真实 kenlm 模型时的逐词打分、词库平局项（`dictionary_tiebreak`）和读音先验（`reading_prior*`）；
- 没对过的：octagram 的接线（`.gram` 读取器本身已对过，词格里用它做重排的那层只对过“有重排器”这一层）、神经重排（合并时只对过“有重排器”这一层的来源规则，重排器本身用桩函数）、词典层把词库查询接成 `lookup` 的胶水；
- 平局：C++ 用 `std::sort` / `partial_sort`，平局次序是实现定义的，Rust 用稳定排序。无模型基线的场景里音节不重复、权重与汉字互不相同，避开了平局；带模型的基线里，未登录词罚分相同，分数完全相等的路径很容易出现，探针会多解 4 条 n-best 来检测，**只要平局落在前 n 条里或恰好卡在 n-best 边界，这个场景就不进基线**。真实输入里的平局两边可能排得不同，但这类句子总分相等，选哪个都对；基线没法为此作证。

### 五笔与日语里没有对过、或要留意的部分

- 没移植：云候选缓存本身（`JapaneseCandidateProvider::dynamic_candidates_`，一个 128 项的环形缓冲，Rust 的 `Provider::query` 只接受调用方传入的那一份）、五笔的调频/删词/造词（写用户词库的事务）、日语词库的 `short_prefix_index_` 缓存（Rust 每次直接扫描，结果相同，只是慢一点）；
- **平局次序是实现定义的，Rust 照着 MSVC 的算法写**：句子解码和矩阵搜索用 `std::partial_sort` 截断，同代价的路径谁在前由堆算法决定。随机基线里真的撞上了（"いえが言えをします" 和 "言えがいえをします" 同为 18053），用稳定排序会排反，所以 `partial_sort_by_cost` 把 MSVC 的堆操作原样搬了过来，并用基线核对；换一个标准库实现，C++ 这一侧的次序可能不同。`std::sort` 在 ≤32 个元素时是插入排序（稳定），这里每处都在这个范围里，用稳定排序；
- 同理，`HiraganaToRomaji` 里同一个假名有几种等长写法时（じ → ji/zi，ぇ → xe/le，ちゃ → cha/cya/tya……）C++ 取哪个取决于 `unordered_map` 的遍历次序，量到的结果（じ→zi、ふ→fu、ちゃ→cya 但 ちょ→cho……）固定在 `romaji.rs` 的 `TIE_CHOICE` 里，并由基线逐项核对。这个函数只用来做显示/提示，没有候选排序依赖它；
- 日语搜索回退路径的疑似缺陷见下。

### 发现的疑似缺陷（C++）

`niihao` 只开“多键”（`autocorrect=8`）时候选为空；`nihaoo`、`shangg` 同样只开多键是正常的，`niihao` 开邻键或全开也正常。黄金样本 `expected_options.jsonl` 已把这个行为记成基线，修之前先确认是不是有意为之。

`JapaneseSentenceDecoder::PrefixLemmasContinuing` 一遇到读音不比前缀长的词条就 `break`，而词库按读音排序，读音恰好等于前缀的词条排在最前面——只要词库里有读音等于已打读音的词条，这个函数就一个也不返回。后果是矩阵搜索在“有没打完的罗马字”时，补不上“已打读音 + 下一个假名”开头的更长词条（候选提供者另外调了 `PrefixLemmas`，所以用户多半还是能看到联想）。Rust 保留了这个行为，`tests/wubi_japanese_golden.rs` 把它记成基线；修之前先确认是不是有意为之。

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
```

基线变了要在提交说明里写清为什么变。

## 重新生成双拼与辅助码基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_helpshuang_probe
server\build-release\engine\Release\msime_helpshuang_probe.exe engine\helpcode engine\contracts\golden\expected_helpshuang.jsonl
```

改了 `engine/shuangpin/shuangpin_profile.cpp` 的方案表后，要同时重新抽取 `src/shuangpin.rs` 末尾的方案函数：`python engine-rs/tools/extract_shuangpin_profiles.py`（脚本从 C++ 源码生成，没有手抄）。

## 重新生成词格基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_lattice_probe
server\build-release\engine\Release\msime_lattice_probe.exe engine\contracts\golden\expected_lattice.jsonl installer\app_data\sc.lm engine\contracts\golden\lm_words.txt engine\contracts\golden\expected_lattice_lm.jsonl
```

后三个参数可省；省略时只重新生成无模型的 `expected_lattice.jsonl`。带模型的基线必须用随包的 `sc.lm`（摘要见 `language-model/lock.json`）。

## 重新生成 kenlm 基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_lm_probe
server\build-release\engine\Release\msime_lm_probe.exe installer\app_data\sc.lm engine\contracts\golden\lm_words.txt engine\contracts\golden\expected_lm.jsonl
```

`lm_words.txt` 是词表（每行一个词，常用词在前、夹未登录词），换了模型要重新挑。

## 重新生成五笔与日语基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_wubi_japanese_probe
server\build-release\engine\Release\msime_wubi_japanese_probe.exe installer\app_data\msime.db installer\app_data\dict_japanese.dat engine\contracts\golden\expected_wubi_japanese.jsonl
```

必须用随包的 `msime.db` 和 `dict_japanese.dat`。罗马字映射表变了要重新抽取：`python engine-rs/tools/extract_romaji_table.py`。

## 重新生成英文与本地模式基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_local_modes_probe
server\build-release\engine\Release\msime_local_modes_probe.exe installer\app_data\msime.db installer\app_data\others.db installer\app_data\english.db engine\contracts\golden\custom_translations_fixture.txt $env:TEMP\local-modes engine\contracts\golden\expected_local_modes.jsonl
```

必须用随包的词库；自定义翻译用 `custom_translations_fixture.txt`（而不是用户机器上的侧车文件）。农历表变了要重新抽取：`python engine-rs/tools/extract_lunar_table.py`。

## 重新生成 octagram 基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_gram_probe
server\build-release\engine\Release\msime_gram_probe.exe engine\contracts\golden\synthetic.gram engine\contracts\golden\expected_gram.jsonl
```

两个文件要一起提交：`synthetic.gram` 是探针现建的合成模型，`expected_gram.jsonl` 是真实 `GramDb` 在它上面的输出。

## 重新生成自定义双拼基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_custom_shuangpin_probe
server\build-release\engine\Release\msime_custom_shuangpin_probe.exe engine\contracts\golden\expected_custom_shuangpin.jsonl
```

## 重新生成双拼输入串基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_shuangpin_query_probe
server\build-release\engine\Release\msime_shuangpin_query_probe.exe engine\contracts\golden\expected_shuangpin_query.jsonl
```

## 重新生成词格查询基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_lattice_lookup_probe
server\build-release\engine\Release\msime_lattice_lookup_probe.exe installer\app_data\msime.db engine\contracts\golden\expected_lattice_lookup.jsonl
```

## 重新生成拼写图基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_spelling_graph_probe
server\build-release\engine\Release\msime_spelling_graph_probe.exe engine\contracts\golden\expected_spelling_graph.jsonl
```

## 重新生成直接辅助码基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_direct_helpcode_probe
server\build-release\engine\Release\msime_direct_helpcode_probe.exe engine\contracts\golden\expected_direct_helpcode.jsonl installer\app_data\sc.lm
```
