# 引擎黄金样本

给“另一个实现要和现在的引擎行为一致”用的规格：一份输入清单，加上现有 C++ 引擎对每条输入给出的预编辑和候选顺序。以后用 Rust 重写引擎的某一块（词库查询、切分、整句词格）时，输出对着这份比，一条一条对。

| 文件 | 内容 |
|---|---|
| `corpus.tsv` | 输入清单，每行 `方案<TAB>按键`。发现新的输入问题就加一行 |
| `expected.jsonl` | 现引擎的输出，每条保留前 40 个候选。提交进仓库，当作基线 |
| `../../tools/golden_probe.cpp` | 生成输出的探针，走 `capi/msime_capi.h` 这层 C 接口 |
| `../../tools/golden_compare.py` | 比对两份输出，列出差异 |
| `../../capi/msime_capi.h` | 其他语言前端认的唯一接口 |
| `ranking_cases.tsv`、`expected_ranking.jsonl` | 调频黄金用例及真实 `adjust_candidate_ranking` 的输出，探针是 `../../tools/ranking_probe.cpp`，用例由 `../../tools/gen_ranking_cases.py` 生成 |
| `expected_lattice.jsonl` | 词格解码与合并的随机场景（带解码时实际查询到的词表）及真实 `decode_word_lattice` / `merge_lattice_candidates` 的输出，探针是 `../../tools/lattice_probe.cpp`；不含语言模型 |
| `expected_lattice_lm.jsonl` | 同一个探针带真实 `sc.lm` 跑出的解码场景（含词库平局项、读音先验）；平局会让 `partial_sort` 的次序不确定，这类场景探针不收 |
| `lm_words.txt`、`expected_lm.jsonl` | kenlm 读取器的词表，以及真实 `ngram::LanguageModel` 对其中词的下标、单词分数和 700 个句子的逐词分数，探针是 `../../tools/lm_probe.cpp`；要用随包的 `installer/app_data/sc.lm` |
| `expected_wubi_japanese.jsonl` | 罗马字转换（穷举哈希加随机抽样）、五笔按键与 `wubi86` 码表查询、日语按键、模型词条查询、整句解码、矩阵搜索和候选提供者的输出，探针是 `../../tools/wubi_japanese_probe.cpp`；要用随包的 `msime.db` 和 `dict_japanese.dat` |
| `expected_local_modes.jsonl`、`custom_translations_fixture.txt` | Unicode、V 模式、日期时间、表情、颜文字、超级简拼、快捷短语、英文词典的输出，探针是 `../../tools/local_modes_probe.cpp`；要用随包的 `msime.db`、`others.db`、`english.db`（以及同目录的清简词库和行业词包），自定义翻译用后者这份样例 |
| `synthetic.gram`、`expected_gram.jsonl` | octagram 语法模型读取器的规格：探针 `../../tools/gram_probe.cpp` 现建的合成 `.gram`（真模型太大、另外下载），以及真实 `gram::GramDb` 在它上面的编码与查询输出 |
| `expected_custom_shuangpin.jsonl` | 双拼用户自定义方案的规格：探针 `../../tools/custom_shuangpin_probe.cpp` 对内置方案随机改动 1200 次，记录布局、`BuildCustomShuangpinProfile` 的错误信息和成功时的方案表 |
| `expected_shuangpin_query.jsonl` | 双拼输入串处理的规格：探针 `../../tools/shuangpin_query_probe.cpp` 对九套内置方案跑随机输入串，记录分段、转全拼、分隔符处理、全码辅助长度、句中辅助码解析和各光标位置的接受判断 |
| `expected_lattice_lookup.jsonl` | 词格词库查询的规格：探针 `../../tools/lattice_lookup_probe.cpp` 在随包 `msime.db` 上用真实 `make_lattice_db_lookup` 查随机跨度和全部单音节键，记录条数、内容哈希和前 12 项；要用随包的 `msime.db` |
| `expected_spelling_graph.jsonl` | 直接辅助码拼写图的规格：探针 `../../tools/spelling_graph_probe.cpp` 对九套内置方案跑随机输入串，辅码谓词用确定的哈希函数代替辅码表，记录边、分隔符、下一出字位置和唯一路径 |
| `expected_direct_helpcode.jsonl` | 直接辅助码整句解码与解析器的规格：探针 `../../tools/direct_helpcode_probe.cpp` 在哈希合成的词库/码表上跑真实 `decode_best_path` 和 `Resolver::resolve`；带语言模型的场景要 `sc.lm` |
| `expected_user_dictionary.jsonl`、`user_dictionary_initial_main.db`、`user_dictionary_initial_english.db` | 用户词库的规格：探针 `../../tools/user_dictionary_probe.cpp` 从同一份合成词库（两个 `.db`）和空日志出发，用真实 `user_dictionary::*`、`edit_personal_dictionary` 等顺序跑 160 个场景的随机操作；一行一步（参数、返回值、三个库逐表转储的哈希，不含 `updated_at`），`{"kind":"scenario"}` 行开新场景 |
| `expected_shuangpin_dictionary.jsonl`、`shuangpin_dictionary_initial.db` | 双拼词库的规格：探针 `../../tools/shuangpin_dictionary_probe.cpp` 用真实 `ShuangpinDictionary` 和 `quanpin::cut_pinyin_by_mode` 导出四类记录——`sort`（`std::sort` 的平局次序）、`cut`（全拼切分）、`read`（随包 `msime.db` 上的查询与辅助码）、写库场景（合成小词库上的造词/调频/删词，每步带返回值和库状态哈希，`{"kind":"scenario"}` 行开新场景）；读和写要 `installer/app_data/`（`msime.db`、`sc.lm`、`helpcodes/`，开领域词库的场景还要 `domain-input.db`）；读场景末尾每套方案多一段开了领域词库的输入（`"dom"` 是启用的词包，候选多一项输入域，空则省略）；再往后是 `{"kind":"sources"}` 整句来源记录（Google/两个神经模型/万象 `synthetic.gram` 的随机组合，`first` 是第一次查询，`second` 是后台重排算完后重查到稳定的结果，没有后台任务时为 null；神经重排与 Google 要 `sentence-model.safetensors`、`dict_pinyin.dat`） **输出文件必须写在本目录里**（`synthetic.gram` 取自输出文件所在目录，探针缺了会直接退出）。 |
| `expected_quanpin_dictionary.jsonl`、`quanpin_dictionary_initial.db` | 全拼词库的规格：探针 `../../tools/quanpin_dictionary_probe.cpp` 用真实 `QuanpinDictionary` 导出两类记录——读（随包 `msime.db` 上 1300 个取自词库真键的输入，含整串、前缀、简拼、混拼、没打完的尾音节、ü 别名、手动分隔符和等音节数打平的歧义串，每个跑 `query`，开模糊音时再跑 `fuzzy_candidates`，单字母输入跑 `expand_initial_candidates`，另有 `find_candidate`；随机整句开关、模糊音、显式切分、“用户在这个读音上调过频”的日志状态）和写库场景（合成小词库上的造词/调频/删词/按当前切分调频，夹着查询，每步带返回值和库状态哈希，`{"kind":"scenario"}` 行开新场景）；读要 `installer/app_data/`（`msime.db`、`sc.lm`，开领域词库的场景还要 `domain-input.db`）；读场景分四段：不带纠错的 1300 个输入、带纠错的 1100 个手误输入（`"ac"` 是纠错类型掩码）、开了领域词库的 700 个输入（`"dom"` 是启用的词包，候选多一项输入域，空则省略以保持老场景的哈希不变）、长句容错的约 200 个输入（词库键拼成的长串，几个音节缩成声母，再来一两处手误；`"fault":1`，记录多一项 `"fault_display"` 即仲裁胜出时的分段显示）；末尾 900 个 `{"kind":"sources"}` 整句来源记录，字段同双拼 **输出文件必须写在本目录里**：`sources` 记录的合成语法模型 `synthetic.gram` 取自输出文件所在目录，写到别处时万象来源失效、整份 `sources` 记录漂移（探针现在缺了会直接退出）。 |
| `expected_quanpin_autocorrect.jsonl` | 全拼纠错层的规格：探针 `../../tools/quanpin_autocorrect_probe.cpp` 导出八类记录——`kbest`/`cut1`（`autocorrect_cut_kbest` 与 k=1 的细节：随机手误输入 × 16 种类型掩码 × k=1/3/9，逐段音节、敲的字母、起点、是否纠正、权重，纠正边数、权重和、是否带生成式边）、`legal`（合法输入上的手误读法）、`reading`（读音对回敲的字母）、`jianpin`（简拼形状闸）、`fault`（长句容错槽位：切槽、各槽纠错目标、是否接管、读音对回原始字母）、`delvar`（漏字变体判定）、`umlaut`（ü 归一）；不读词库，不需要 `app_data` |
| `expected_domain_dictionary.jsonl` | 领域词库的规格：探针 `../../tools/domain_dictionary_probe.cpp` 在随包 `domain-input.db` 上用真实 `domain_dictionary::Dictionary` 导出 `ids`、`normalize`（启用串归一）、`enable`（切换启用集合）、`query`、`exact`、`lattice`、`merge`，条数、哈希、前 8 项含输入域；要 `installer/app_data/domain-input.db` |
| `expected_dictionary_state.jsonl` | 词库状态暂存与运行时目录的规格：探针 `../../tools/dictionary_state_probe.cpp` 用真实 `stage_dictionary_state` / `prepare_runtime_paths` 导出两类记录——`stage`（随机记录流，含越界/重复/超限等坏记录，新目录/已存在/落在资源目录里，合法与非法的内容 ID；记成败、目录是否留下、暂存出的日志与词库转储哈希、再导出的状态哈希）、`prepare`（新建、重复、日志里多了改动、根目录重叠、缺文件的旧代、已有 `.incoming`、非法 ID；记每次调用的成败和结果哈希）；初始库是 `user_dictionary_initial_main.db` / `user_dictionary_initial_english.db` |
| `expected_google_pinyin.jsonl` | Google 拼音解码器的规格：探针 `../../tools/google_pinyin_probe.cpp` 用真实 `MatrixSearch` 导出——`qsort` 平局次序、拼写解析、整句解码（含句中辅助码约束）、一次打一个键的增量打字；要 `installer/app_data/dict_pinyin.dat` |
| `expected_scheme.jsonl` | 输入方案的规格：探针 `../../tools/scheme_probe.cpp` 让真实的四个方案类吃随机按键序列，每步导出 `QueryRequest` 全部字段和预编辑；不需要词库 |
| `expected_pinyin_provider.jsonl` | 拼音候选提供者的规格：探针 `../../tools/pinyin_provider_probe.cpp` 用真实 `PinyinCandidateProvider` 吃方案生成的请求，记首次查询、插入云/AI 候选后的再次查询、单字母展开；要 `installer/app_data/`（`msime.db`、`sc.lm`、`helpcodes/`） |
| `expected_ime_session.jsonl` | 输入会话的规格：探针 `../../tools/ime_session_probe.cpp` 让真实的 `ImeSession` 吃随机操作序列（按键、整串替换、重置、清缓存、动态候选、单字母展开、方案切换、前缀查询），每步导出预编辑、请求主要字段、候选列表（后 240 个序列打开双拼直接辅助码）；要 `installer/app_data/`（词库、`sc.lm`、辅助码表、`dict_japanese.dat`） |
| `expected_punctuation.jsonl` | 中文标点翻译的规格：探针 `../../tools/punctuation_probe.cpp` 让真实的 `PunctuationPolicy` 吃 400 个随机字符序列，每个字符导出译文（不支持为 null）；不需要词库 |
| `expected_input_session.jsonl` | 兼容会话 `InputSession` 的规格（约 19 MB）：探针 `../../tools/input_session_probe.cpp` 在固定词库与固定时钟下，把 760 个操作序列（前 400 个随机配置，中间 300 个定向轮，最后 60 个长句容错轮：全拼全开纠错，打带句中简拼与手误的长句；定向轮：学习、调频、辅助码、混输，偏向打词、选词、取消、云/AI 候选；字母、数字、标点、选词、光标、本地模式触发、固定位置）逐步喂给真实会话，导出每步的按键结果、预编辑、候选哈希、上屏与光标 |
| `expected_session.jsonl` | 平台入口 `Session`的规格（约 6.8 MB）：探针 `../../tools/session_probe.cpp` 在固定词库下把 420 个操作序列（前 300 个随机配置，后 120 个定向轮）逐步喂给真实 `Session`，导出每步的按键结果与快照；日期时间本地模式关掉 |
| `expected_capi.jsonl` | C 接口的规格（约 370 KB）：探针 `../../tools/capi_probe.cpp` 通过 `capi/msime_capi.h` 驱动真实的 C++ 实现，200 个会话（四种方案各 50）的随机按键序列，每步导出返回值、上屏文字、快照 JSON 的长度与哈希 |
| `expected_input_contracts.jsonl` | 输入形状契约的规格：探针 `../../tools/input_contracts_probe.cpp` 直接调 `contracts/date_time_input.h` 与 `contracts/direct_helpcode.h`，导出随机串 × 光标 × 字符的接受结果；不需要词库 |
| `expected_candidate_queries.jsonl` | 候选查询的规格：探针 `../../tools/candidate_queries_probe.cpp` 让真实的 `CandidateQueries` 跑九种本地模式（固定时钟）和 1500 条英文/表情/颜文字混排，每条导出候选列表（条数、哈希、前 12 项）；要 `installer/app_data/` 下的 msime.db、others.db、english.db |
| `expected_neural.jsonl` | 神经重排的规格：探针 `../../tools/neural_probe.cpp` 用真实 `neural::SentenceModel` / `rerank_order` 在随包 `sentence-model.safetensors` 上导出 `load`（整份、截断、改字节的成败，窗口长度、署名、许可、若干字符的 token id）、`next`（`next_log_probabilities` 前 5 名与分布和）、`score`（`score_sentences`，双精度 %.17g）、`rerank`（随机静态分、λ、路径上限、前文，导出次序）、`last`（`last_characters`）；要 `installer/app_data/sentence-model.safetensors` |
| `expected_helpshuang.jsonl` | 双拼方案表、解码、分段与六套辅助码表的取码和匹配，探针是 `../../tools/helpshuang_probe.cpp` |

## 重新生成或核对

基线必须用**随包的出货词库**生成，不能用某个人本机正在学习的用户词库，否则每个人跑出来的权重都不同。用 `installer/app_data`（`Prepare-PackageFiles.ps1` 之后就有）复制一份当数据目录：

```powershell
# 1. 构建探针（它不进默认构建）。先配置一次，之后只需第二条
cmake -S server -B server/build-release
cmake --build server/build-release/engine --config Release --target msime_golden_probe

# 2. 复制出货数据，避免动到原件
Copy-Item installer/app_data $env:TEMP\golden-data -Recurse

# 3. 跑样本
server\build-release\engine\Release\msime_golden_probe.exe $env:TEMP\golden-data $env:TEMP\golden-data $env:TEMP\golden-data\cache engine\contracts\golden\corpus.tsv $env:TEMP\actual.jsonl

# 4. 和基线比
python engine/tools/golden_compare.py engine/contracts/golden/expected.jsonl $env:TEMP\actual.jsonl --top 20
```

退出码 0 表示和基线完全一致。同一份数据跑两遍结果逐字节相同（探针里 `learning` 关闭，不选词、不上屏）。

## 改引擎后怎么办

- 改动**不该**影响排序，比对出差异就是回归，先修。
- 改动**该**影响排序（词库升级、排序策略调整），确认差异符合预期后重新生成并提交 `expected.jsonl`，提交说明里写清变了什么。
- 重新生成时用 `golden_compare.py --trim 全量.jsonl expected.jsonl --top 40` 把全量输出裁成基线。
