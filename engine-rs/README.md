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
| 用户词库：操作日志（造词/删词/调频/重放，两库提交次序）、固定位置、调频的权重算法（中点、占用权重冲突、16 行重排、上限/下限/逼近 int64）、五笔/快捷短语/英文词的学习与槽位、日期时间格式顺序、个人词库的校验/编辑（回执、过期编辑检测）/分页、词库状态导出；`EnglishDictionary::ensure_schema` / `upsert_gloss` | `src/user_dictionary.rs`、`src/english.rs`（`ensure_schema`、`upsert_gloss`） | `tests/user_dictionary_golden.rs`：160 个场景 × 8 到 25 步随机操作（2651 步，28 种操作），每个场景从同一份合成 `msime.db`/`english.db` 和空日志出发；每步的返回值和执行后三个库逐表转储的哈希（不含 `updated_at`）与真实 `user_dictionary::*`、`edit_personal_dictionary`、`personal_dictionary_entries`、`stream_dictionary_state` 一致。合成词库里专门放了走进重排各分支的形状（权重超上限、成片相同、16 个连号、阶梯末位恰好等于下限、权重逼近 int64 上限、连号链撞上上限），探针把选中项摆到能走进这些分支的位置。变异检查：重排间隔与行数、上限、中点取整、阶梯起点、冲突阈值、阶梯下限边界、逼近上限判断、英文词长度边界、个人词库分页与回执 ID 长度边界都能被测出。测不出的是等价变异：`slot >= max_slot` 已提前返回，后面 `(slot + 1).min(max_slot)` 的 `min` 去不去都一样（英文槽位同理）；重放按 `updated_at, rowid` 排序在这套数据里无影响（各日志行落在词库的不同行上）。`ensure_schema` 只核对了建出的表名和 `user_version`，`upsert_gloss` 没有单独的基线。基线由 `msime_user_dictionary_probe` 导出，初始库一并提交 |
| 双拼词库（按双拼切分查全拼词库：整串、声母、前缀子串、单字母 24 条与扩展；句中辅助码约束；单码/双码辅助码筛选与重排；词格整句并入候选；造词/调频/删词的 SQL 与校验、写库和日志）；共用的全拼切分（贪心、带纠错的别名表）、按来源区分的混合简拼、`std::sort` | `src/shuangpin_dictionary.rs`、`src/quanpin_query.rs`、`src/dict.rs`（`QuerySource`、`query_initial`、`query_keyed_flat`）、`src/msvc_sort.rs`（`sort_by`） | `tests/shuangpin_dictionary_golden.rs`：四类。**切分** 4500 条（随机音节、手误写法、同一个有歧义的写法重复多遍；纠错与贪心不同 1565，多条路径 386，不完整 2406）的贪心/纠错路径、是否完整拼音、简拼与真实 `cut_pinyin_by_mode` 一致。**`std::sort`** 204 个数组（大小 0 到 5000，平局很多）的最终下标顺序与 MSVC 一致（`<algorithm>` 的内省排序照头文件搬）。**读**：随包 `msime.db` 上九套方案 × 70 个随机输入串，`generate`/`generateSeries`/句中辅助码筛选/单码双码辅助码/单字母扩展/查权重/造词查询共 2600 多次，条数、内容哈希、前 8 项与真实 `ShuangpinDictionary` 一致（随机词格开关、显示下一条、模糊音规则、句中辅助码约束）。**写**：54 个场景 683 步的造词/调频/删词（含双拼原串、带分隔符/不带分隔符的全拼、带手误的全拼），返回值和词库、日志逐表转储的哈希一致。变异检查：双拼声母严格匹配、单字母上限、单码末字优先、权重 +1、前缀子串循环、受约束跨度上限、别名表各条、纠错路径上限与排序键都能被测出。测不出的是等价变异：`query_keyed_flat` 换稳定排序（SQL 已按权重降序，随包词库上这一步不改变次序；排序本身由 `std::sort` 那组样本单独核对）；`do_validate` 里 `jp.len() != han_count` 改成 `>`（前面已有段数等于字数的检查）；n-best 条数 12 改 5（显示下一条的场景里没出现第 6 条以后的差别）；纠错路径排序的第三个键（别名备选的名次，生成次序本来就是它）。没搬的见下面的清单。读和写要 `installer/app_data/` 下的 `msime.db`、`sc.lm`、`helpcodes/`（或 `MSIME_APP_DATA`），缺了就跳过。基线由 `msime_shuangpin_dictionary_probe` 导出，合成小词库一并提交 |
| 全拼词库（按切分查词库：整串、前缀子串、稀疏拼音回退；等音节数切分的备选并入；词格整句并入；字面混合简拼/整词领衔；合法输入上等音节数切分的整句仲裁；模糊音并入；单字母 24 条与扩展；造词/调频/删词）和纠错层（k-best 纠错切分、合法输入上的手误读法与仲裁、读音对回敲的字母、上下文消解、纠错标记） | `src/quanpin_dictionary.rs`、`src/quanpin_utils.rs`（音节图、完整/最少音节切分、别名归一）、`src/autocorrect.rs`（`autocorrect_cut_kbest`、`legal_input_correction_cuts`、`correction_cut_for_reading` 等）、`src/lattice.rs`（`Item` 多了 `fuzzy`、`corrected_from`） | `tests/quanpin_dictionary_golden.rs`：随包 `msime.db` 上 3100 个输入（取自词库真键的整串、前缀、简拼、混拼、没打完的尾音节、ü 别名、手动分隔符、最少音节数打平的歧义串、手误输入 × 16 种纠错掩码、开了领域词库的输入）的 `query` / `fuzzy_candidates` / `expand_initial_candidates` / `find_candidate`，随机整句开关、模糊音、显式切分和用户调频日志；合成小词库上 60 个场景 719 步的造词/调频/删词，返回值和逐表转储的哈希一致。`tests/quanpin_autocorrect_golden.rs`：k-best 3000、k=1 的细节 600、合法输入 2500、读音对回敲的字母 2500、简拼形状 1500。变异检查（约 140 处变异，分页/比例/路径上限/稳定排序/各个门槛）：能被测出的都补了数据；测不出的是等价变异或浮点边界平局（浮点打分恰好相等的分支、n-best 条数 12→11 需要 12 条以上路径、`reopen` 在 Rust 里没有对应的缓存可失效）。没搬的：`handleVkCode`、`insert_word_to_series_cache`、各种结果缓存。整句来源（Google 解码器、神经重排、万象搭配）见 `sentence_sources` 一行。基线由 `msime_quanpin_dictionary_probe`、`msime_quanpin_autocorrect_probe` 导出，合成小词库一并提交 |
| 领域词库（随包 `domain-input.db`，按词包过滤的 TEMP 视图，只读、失败即整库关闭；`query`/`exact`/`lattice` 带输入域；`merge`）；已接进全拼和双拼词库（`set_enabled_domain_dictionaries`，并入查询、备选切分、模糊候选、词格跨度和合法输入仲裁） | `src/domain_dictionary.rs`、`src/dict.rs`（`Dictionary::from_connection`） | `tests/domain_dictionary_golden.rs`：200 次切换启用集合 × 14 步（`query` 1097 条非空 237、`exact` 577 条非空 151、`lattice` 552 条非空 119、`merge` 574 条非空 219）与 300 条启用串归一，条数、哈希、前 8 项（含输入域）一致。全拼/双拼的基线里另有开了领域词库的读场景（全拼 700 个输入、双拼 9 套方案 × 120 个，带输入域的候选行 1357 / 1456 条）。C++ 的 `membership` 结果缓存没搬（只影响速度）。已知差异：`merge_alternative_segmentations` 在词库没有备选整词、只有领域词库有时，C++ 对空向量取 `front()`（未定义行为），Rust 当作没有保护位 |
| 运行时目录与词库代际（`prepare_runtime_paths`：三个根目录互不重叠、SQLite 备份接口拷词库、重放日志、`.ready` 标记、整体改名、切回已就绪代际时再重放一遍；`stage_dictionary_state`：在新建的代际目录里按记录流重建日志——词条、固定位置、选择计数，越界/重复/超限整体失败并删掉本次目录） | `src/runtime_paths.rs`（`rusqlite` 开了 `backup` 特性） | `tests/dictionary_state_golden.rs`：260 个暂存场景（成功 106，失败 154：空键、超长、权重/位置/计数越界、身份重复、位置冲突、记录上限、目标目录已存在或落在资源目录里、非法内容 ID）比成败、目录是否留下、暂存出的日志与词库转储哈希、再导出的状态；80 个 `prepare` 场景（新建、重复、日志里多了改动后再准备、换内容 ID、根目录重叠、缺文件的旧代、已有 `.incoming`、非法 ID）比每次调用的成败和结果转储哈希。基线由 `msime_dictionary_state_probe` 导出，初始库用用户词库探针的两份。C++ 里 `std::filesystem::permissions(owner_all)` 在 Windows 上没有效果，没搬 |
| 神经重排（`sentence-model.safetensors`：字符级 decoder-only Pre-LN Transformer，F32/F16/I8 张量，int8 按输出行对称量化；`score_sentences` 给一批成型句子在前文之后打 `log P`，前文 K/V 缓存、整批一次跑；`rerank_order` 把词格路径静态分和神经分按 `静态 + λ·(神经 − 静态)` 插值后稳定重排；`last_characters`） | `src/neural.rs`（`serde_json` 解析 safetensors 头；x86_64 上 int8 矩阵走 `std::arch` 的 AVX2+FMA 8 路累加，和 C++ 的 `linear_int8_avx2` 同序，其他平台走 4 路标量累加） | `tests/neural_golden.rs`：随包模型上的载入成败（整份、12 个截断长度、5 处改字节）、署名/许可/窗口长度/若干字符的 token id、40 段文本的 `next_log_probabilities`（前 5 名 id 逐项相同，值与分布和相对差 3e-9）、约 200 批共 1162 个 `score_sentences`（含空句、超长句、前文超过窗口、同前文第二批命中缓存；与 C++ 的最大相对差 2e-16，即在 AVX2 机器上逐位一致，容差放在 2e-3）、120 个 `rerank_order`（77 个改变了次序，次序逐项相同）、60 个 `last_characters`。`RescoreWorker` 在 `src/sentence_sources.rs`，见下一行。没搬的：`shared_sentence_model` 的进程级缓存（那一层由 `SentenceSources` 按路径缓存）。基线由 `msime_neural_probe` 导出 |
| 候选提供者登记处（按方案把查询、调频、删词、动态候选缓存分给拼音、五笔、日语；五笔调频/删词写用户词库；日语云候选缓存） | `src/provider.rs` | 只有单元测试（日语云候选缓存的每来源一条与清缓存、五笔/日语没有的写操作返回成功、五笔库打不开时调频失败）。五笔、日语的查询本体已由 `tests/wubi_japanese_golden.rs` 对过，这里只是把 `QueryRequest` 接到它们的请求上，接线本身没有单独的 C++ 基线；五笔调频/删词的库写入由 `tests/user_dictionary_golden.rs` 对过 |
| 输入方案（全拼、双拼、五笔、日语罗马字：按键 → 原始输入串 → `QueryRequest`，含句中辅助码的段替换、直接辅助码的斜杠与分号、五笔扩展长度/混输/通配）与请求类型；全拼末尾大写辅助码的检测与剥离 | `src/scheme.rs`、`src/request.rs`、`src/quanpin_utils.rs`（`detect_active_helpcode_length`） | `tests/scheme_golden.rs`：全拼 160、双拼九套方案 × 四种开关 × 40、五笔八种开关 × 40、日语 160 个随机按键序列（字母大小写、撇号、反引号、斜杠、分号、减号、数字、退格、回车、退出、整串替换），共 15586 步，每步请求的全部字段与预编辑逐项一致。五笔的 `reset` 会清掉扩展长度，测试同样先 reset。基线由 `msime_scheme_probe` 导出。`wubi.rs`、`japanese.rs` 里更早的 `Scheme` 是同一份逻辑的旧副本，后续接会话时并过来 |
| 拼音候选提供者（全拼引擎、双拼引擎：开关下发、末尾辅助码、双拼单码/双码、模糊音并入、句中辅助码约束、按方案打标、云/AI 候选插入缓存列表、单字母展开、按方案造词/调频/删词/查词）；两个词库的整串结果缓存（定长缓存 `src/cache.rs`，键与清理时机照 C++） | `src/pinyin_provider.rs`、`src/cache.rs`、`src/quanpin_dictionary.rs`、`src/shuangpin_dictionary.rs` | `tests/pinyin_provider_golden.rs`：全拼与三套双拼各 300 个输入 1200 个（辅助码 624、模糊音 395、句中辅助码、插动态候选 336、展开 29）的首次查询、插入动态候选后不清缓存再查、单字母展开逐项一致。变异检查测不出的是等价变异：纠错路径里 `count > typed.len()` 的边界（模糊候选的音节数不会超过敲出的）、单码辅助里偶数长度的判断（后面的完整音节检查已经挡掉）、句中辅助码开关关着时不下发约束（探针只在开着时才带约束）。没搬的：双拼直接辅助码解析器的接线 `resolve_direct_helpcode` |
| Google 拼音解码器（`dict_pinyin.dat` 的拼写 Trie、词典列表与 Trie、n-gram 码本；`MatrixSearch` 的拼音解析、候选扩展与剪枝、句子回溯；句中辅助码约束；`qsort` 平局次序） | `src/google_pinyin.rs`、`src/msvc_sort.rs`（`qsort_by`） | `tests/google_pinyin_golden.rs`：拼写解析、整句解码（含带约束的）逐项与真实解码器一致，一次打一个键的增量打字（497 条记录）与整串重新解码一致；`qsort` 的平局次序与 MSVC 运行库一致。剪枝分数按 double 比较、第一行上限这类边界在测试数据里测不出，等价变异保留。基线由 `msime_google_pinyin_probe` 导出 |
| 整句来源的接线（全拼、双拼词库共用的 `SentenceSources`：Google 解码器、快速档/高精度档神经模型、万象 `.gram` 的按路径缓存；`RescoreWorker` 后台重排线程——第一次查询按静态顺序，算完回调后重查；整句兜底行与独立 Google 整句行、句中辅助码随解码、神经 n-best 重打分、万象搭配逐边叠加、重排前文） | `src/sentence_sources.rs`、`src/quanpin_dictionary.rs`、`src/shuangpin_dictionary.rs`、`src/quanpin_utils.rs`（`to_google_spelling`） | `tests/quanpin_dictionary_golden.rs`、`tests/shuangpin_dictionary_golden.rs` 各自的 `sentence_sources_match_cpp`：全拼 900 个、双拼 443 个随机组合（词格/Google/两个神经模型/万象权重与重排/显示下一条/前文/模糊音/句中辅助码），第一次查询和后台重排算完后的结果逐项相同；C++ 探针每收到一次回调就重查到不再有回调，Rust 测试等线程停了重查到结果不变。没搬的：双拼的 `direct_sentence_` 预计算路径；Rust 的查询没有按串缓存，查询慢到后台已经算完时“第一次”会看到重排后的次序（测试里不会发生） |

## 还在 C++ 里的块（按建议顺序）

1. 会话层：`ImeSession`（按键 → 方案 → 请求 → 提供者 → 候选列表，含五笔拼音混输的合并）、`input_session*`（Server 仍在用的兼容会话）、`nine_key_session`、`capi`；双拼的 `resolve_direct_helpcode`（直接辅助码解析器接到词典层）、`query_single_char_rows`；两个词库的 `handleVkCode` 按键状态机。Google 解码器、三类整句来源、方案、拼音提供者和提供者登记处已移植
2. 用户词库里没搬的一处：按默认路径缓存连接（只是性能手段，Rust 每次打开新连接）。`stage_dictionary_state` 和 `prepare_runtime_paths` 已移植
3. 五笔和日语里调用用户词库的接线（用户词库本身已移植）；辅助码在候选窗里的实际接线
4. 本地模式：`local_database.cpp` 的共享连接缓存（只是性能手段，Rust 每次查询开一个只读连接）、`current_local_date_time`（读系统时钟，Rust 的日期时间函数都接受调用方给的“此刻”）

### 纠错

错拼表、k-best 切分和全拼词典层上的纠错（简拼形状闸、合法输入闸、表外生成读法、头部纠错 + 简拼尾、纠错读法与字面整词的混排仲裁、上下文消解）都已移植，由 `tests/quanpin_autocorrect_golden.rs` 和 `tests/quanpin_dictionary_golden.rs` 对照真实 C++。`tests/golden.rs` 里的 `AUTOCORRECT_CPP_ONLY` 是更早只有静态表时留下的清单，这些输入现在由词典层的黄金样本覆盖。

### 整句词格里没有对过的部分

- 对过的：无模型时的启发式打分、惩罚分、字级搭配回调、字约束、束宽、n-best 去重、合并规则；带真实 kenlm 模型时的逐词打分、词库平局项（`dictionary_tiebreak`）和读音先验（`reading_prior*`）；
- 没对过的：词典层把词库查询接成 `lookup` 的胶水（词典层的 `sentence_sources_match_cpp` 已把 octagram、神经重排、Google 的接线连同真实模型对过）；
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

## 重新生成用户词库基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_user_dictionary_probe
$work = Join-Path $env:TEMP 'udprobe'
server\build-release\engine\Release\msime_user_dictionary_probe.exe $work engine\contracts\golden\expected_user_dictionary.jsonl
Copy-Item $work\initial_main.db engine\contracts\golden\user_dictionary_initial_main.db
Copy-Item $work\initial_english.db engine\contracts\golden\user_dictionary_initial_english.db
```

## 重新生成全拼词库基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_quanpin_dictionary_probe
$work = Join-Path $env:TEMP 'qpdprobe'
server\build-release\engine\Release\msime_quanpin_dictionary_probe.exe installer\app_data $work engine\contracts\golden\expected_quanpin_dictionary.jsonl
Copy-Item $work\quanpin_dictionary_initial.db engine\contracts\golden\quanpin_dictionary_initial.db
```

## 重新生成双拼词库基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_shuangpin_dictionary_probe
$work = Join-Path $env:TEMP 'spdprobe'
server\build-release\engine\Release\msime_shuangpin_dictionary_probe.exe installer\app_data $work engine\contracts\golden\expected_shuangpin_dictionary.jsonl
Copy-Item $work\shuangpin_dictionary_initial.db engine\contracts\golden\shuangpin_dictionary_initial.db
```

## 重新生成纠错与领域词库基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_quanpin_autocorrect_probe msime_domain_dictionary_probe
server\build-release\engine\Release\msime_quanpin_autocorrect_probe.exe engine\contracts\golden\expected_quanpin_autocorrect.jsonl
server\build-release\engine\Release\msime_domain_dictionary_probe.exe installer\app_data\domain-input.db engine\contracts\golden\expected_domain_dictionary.jsonl
```

## 重新生成词库状态基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_dictionary_state_probe
$work = Join-Path $env:TEMP 'dsprobe'
server\build-release\engine\Release\msime_dictionary_state_probe.exe engine\contracts\golden\user_dictionary_initial_main.db engine\contracts\golden\user_dictionary_initial_english.db $work engine\contracts\golden\expected_dictionary_state.jsonl
```

## 重新生成 Google 拼音解码器基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_google_pinyin_probe
server\build-release\engine\Release\msime_google_pinyin_probe.exe installer\app_data\dict_pinyin.dat $env:TEMP\gpprobe_user.dat engine\contracts\golden\expected_google_pinyin.jsonl
```

## 重新生成神经重排基线

```powershell
cmake --build server/build-release/engine --config Release --target msime_neural_probe
server\build-release\engine\Release\msime_neural_probe.exe installer\app_data\sentence-model.safetensors engine\contracts\golden\expected_neural.jsonl
```
