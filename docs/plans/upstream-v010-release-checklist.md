# 水杉 v0.10.0 与 v0.10.1 更新对照

日期：2026-10-12。功能实施状态：用户已确认 P0–P4，按文档顺序实施中。

本表将两版发布说明与固定标签源码逐项对照，再决定在个人分支复用、移植或保留原策略。具体任务设计、实施顺序和统一验收见[功能移植规划](qingjian-upstream-v010-plan.md)。每行的验收项用于下一轮实施，未执行的测试不计为通过。

| 基线 | 提交 |
|---|---|
| 当前个人分支 C | `7ac7b2a31028e614a9e4a434eb125ad3dcc75e21` |
| v0.10.0 U0 | `0d7ae4ca795708261c354316e9f0554a9cbc35d6` |
| v0.10.1 U1 | `0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2` |
| 发布后 P | 截至 `fc90cb1f2c765976ae2c3e48e5b9ea47766685a6` |

“已有”只表示当前源码有能力并将复用；“部分”表示存在基础但需补齐行为；“待移植”表示缺少对应接线；“保留原策略”表示上游的默认或发行环境不直接适用于本分支。v0.10.1 以最终语义覆盖 v0.10.0 的相关方案，发布后提交另列。

发布说明：[v0.10.0](https://github.com/metasequoiaime/msime-windows/releases/tag/v0.10.0)、[v0.10.1](https://github.com/metasequoiaime/msime-windows/releases/tag/v0.10.1)。源码差异：[U0 至 U1](https://github.com/metasequoiaime/msime-windows/compare/0d7ae4ca795708261c354316e9f0554a9cbc35d6...0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2)、[U1 至 P](https://github.com/metasequoiaime/msime-windows/compare/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2...fc90cb1f2c765976ae2c3e48e5b9ea47766685a6)。

## v0.10.0 词库与翻译

| 编号与更新 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| D01 912,640 条全义项中译英（#784） | 待数据移植；两标签仍锁定旧库。本机相同 SHA 的基础中译英表为 31,371 条，另加的青简、行业层不等同于这批新数据。 | P1 固定新来源、许可和完整义项，单独构建并核对最终安装数据。未拆官方 EXE，不宣称其实际包含或不包含该数量。 | [产品锁][D01a]、[发行取库][D01b]、[构建顺序][D01c] |
| D02 Ctrl+Enter 去词性上屏（#784） | 已有。当前有效译文可进子页，去词性后上屏；跨来源完整义项限制见 D06。 | 保留现有子页与发音。测试至少五义项、多种词性、分号及读音注释；只改上屏文本，不改原始释义。 | [当前译义处理][D02a]、[当前按键路径][D02b] |
| D03 词性与分号独立配色（#848） | 部分。已有 muted/fresh 和生词色，但缺独立 pos/sep 皮肤键；摘要仍只显示两项加数量提示。 | P1 合并 token 分段和可选颜色，增加完整详情；两渲染器、旧皮肤、选中行及 Ctrl 发音全部回归。 | [当前视图][D03a]、[U1 分段视图][D03b] |
| D04 泰语、越南语、意大利语（#694） | 候选译词已有越南语，缺泰语和意大利语；截图翻译菜单已有不代表候选入口已支持。 | 补候选配置、校验、菜单与实际厂商代码。切语言清缓存，缺本地词库且云关闭时不请求；不串英文候选中文释义。 | [当前语言校验][D04a]、[U1 配置][D04b] |
| D05 新装默认小牛翻译（#836） | 保留原策略。当前原厂商选择与 `candidate_translation_cloud=false` 门禁优先。 | 新装策略单独确认，升级保留 provider、开关、endpoint 和凭据；空值及占位值不请求。与 J09、P03 一起验收。 | [当前模板][D05a]、[U1 模板][D05b] |
| D06 全部义项保存与展开（#784） | 原数据库保留原文，子页已有；跨来源合并最多六项，候选摘要只显示两项加提示。 | P1 原始数据、合并结果和摘要分开；详情与导出不截断，五项及超过六项的多来源例子回导一致，曝光只计实际显示部分。 | [当前合并][D06a]、[完整源入库][D06b] |

[D01a]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/product-lock.json#L5
[D01b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/.github/workflows/release.yml#L565
[D01c]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/scripts/dictionary_build/build.py#L113
[D02a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/candidate_translation_policy.h#L29
[D02b]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/event_listener_keys.cpp#L605
[D03a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/window/candidate_view_model.h#L31
[D03b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/window/candidate_view_model.h#L66
[D04a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/config/ime_config_services.cpp#L240
[D04b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/config/ime_config.cpp#L285
[D05a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/installer/default_config/config.default.toml
[D05b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/installer/default_config/config.default.toml#L104
[D06a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/english/english_dictionary.cpp#L114
[D06b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/scripts/dictionary_build/english_db.py#L503

## v0.10.0 AI 辅助与保护

| 编号与更新 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| A01 查询厂商模型列表（#711） | 待移植，缺 `apiModelList` 和后端查询。 | P2 用户触发获取模型名；复用代理与 requestId，检查空表、401、429、超时、过大响应、切厂商晚回。无 token 本地服务可用，不下载权重。 | [U1 查询后端][A01a]、[U1 菜单请求][A01b] |
| A02 可手填下拉、空列表查询和键盘交互（#829/#837/#841） | 模型文本框已有，查询下拉缺少。 | P2 可编辑 combobox，支持外部点击关闭、键盘选择、手填值；列表失败不替换原模型，深浅主题与等高布局回归。 | [当前设置][A02a]、[U1 设置][A02b] |
| A03 测试连接（#711） | 已有“测试配置”前后端。 | 复用而不加第二套。扩展 Custom，错误不含凭据；成功只证明连接/请求被接受，结果质量和完整 JSON 另测。 | [当前测试][A03a]、[U1 测试][A03b] |
| A04 Custom 提供商（#711） | 当前仅四个固定 AI 厂商；手改某厂商地址不等于独立 Custom 槽。 | P2 新增独立 endpoint/model/token，支持回环本机服务。五槽切换、升级回读及提示词库 CRUD 全量保留。 | [当前 provider][A04a]、[U1 provider][A04b] |
| A05 DeepSeek 思考强度（#824/#821） | 待移植。AI 与润色请求目前固定关闭思考，润色只有原短超时。 | AI 与润色分别保存关闭、低、高、最高；默认仍关闭。仅 DeepSeek 加专属参数，预算及取消配套；长推理不阻塞输入、不开启云润色。 | [当前 AI 请求][A05a]、[U1 请求构建][A05b] |
| S01 密码与 PIN 输入不发 AI（#845） | 当前缺 InputScope 到 SensitiveField 再到请求/历史门禁的完整链路。 | P0 共享合同和两端实现；普通/敏感/未知三态，未知抑制自动请求。读取拒绝、无声明、旧 DLL、延迟键与焦点切换均测，基础输入不受影响。 | [U1 TSF 判断][S01a]、[U1 Server 门禁][S01b] |
| S02 换应用或输入框清空 AI 前文（#845） | 部分。当前有晚回归属校验，但共享前文仍可带入下一应用请求。 | P0 按 client 和 activation epoch 清理临时前文；A→B→A、同应用换框、重连回归。每人/群持久聊天记录继续保留。 | [当前上下文][S02a]、[U1 所有权][S02b] |
| S03 发送前过滤疑似凭据（#845） | 待移植。不能用它替代 S01。 | P0 过滤发送副本，保留用户原文；测试 ASCII 长度边界、UTF-8、纯数字 PIN、日期误判。规则并不识别所有私人信息。 | [U1 过滤规则][S03a] |
| S04 API 配置 DPAPI 与旧明文迁移（#840） | API 配置缺统一加密；聊天文件已有 DPAPI，两者分开。 | P0 凭据清单补齐 TTS/OCR 新字段；原子写入、失败保留、合成 token 回归，模型调用读取明文但日志不输出值，不重加密聊天格式。 | [当前配置写入][S04a]、[U1 凭据字段][S04b]、[U1 DPAPI][S04c] |
| S05 降级与跨账户/机器说明 | 新 API 密文旧版无法读取，跨账户不可直接用。 | 与 S04 同交付，保留密文存档，失败只重配对应项，不能发送密文或为便携改成机器级默认。说明新版备份与降级边界。 | [U1 解密路径][S05a] |

[A01a]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/settings/api_credential_test.cpp#L315
[A01b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/modules/model-fetch.ts#L20
[A02a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/modules/ai-settings.ts#L41
[A02b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/modules/ai-settings.ts#L179
[A03a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/settings/api_credential_test.cpp
[A03b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/settings/api_credential_test.cpp#L149
[A04a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/config/ime_config.cpp#L287
[A04b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/config/ime_config.cpp#L294
[A05a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ai/ai_assistant.cpp#L45
[A05b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/ai/ai_assistant_request.cpp#L10
[S01a]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/windows/src/Key/KeyEventSink.cpp#L82
[S01b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/ipc/event_listener.cpp#L518
[S02a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/event_listener.cpp#L506
[S02b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/ipc/event_listener.cpp#L585
[S03a]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/ipc/ai_context_policy.h#L12
[S04a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/config/ime_config_write.cpp#L45
[S04b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/config/ime_config_credentials.cpp#L34
[S04c]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/config/credential_protection.cpp#L46
[S05a]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/config/ime_config_credentials.cpp#L58

## v0.10.0 模型与安装默认

| 编号与更新 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| M01 效果档按需下载、导入、删除、浏览器下载（#838） | 两档模型仍随包；ASR/TTS/OCR 管理已有，不等同于神经 desktop 管理。 | P2 增加 desktop 管理，25,480,184 字节与固定摘要；速度档保留，效果档新装按需、旧装迁移。缺模型不自动下载，加载及删除状态一致。 | [当前收集][M01a]、[U1 管理][M01b] |
| M02 v0.10.0 清除旧效果模型的升级提示 | 与用户保留要求冲突。U1 后续增加接收旧模型，见 J12。 | 不采用清掉后重下。对匹配、不匹配、自定义、新旧冲突及迁移失败都保留原文件；用小 fixture 测安装中断与恢复。 | [U1 接收旧文件][M02a] |
| M03 新安装默认 fluent 横排、中文，辅助码/Emoji/颜文字/语音/AI 关闭（#659/#793/#847） | 与本分支默认有差异，升级保留名单不足。 | 保留原策略，只有确认新装默认时才改变。旧值即使等于历史默认仍保留；所有扩展字段及模板、音色、聊天配置回读。 | [当前出厂模板][M03a]、[U1 保留名单][M03b] |

[M01a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/installer/Prepare-PackageFiles.ps1#L169
[M01b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/settings/neural_model.cpp#L29
[M02a]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/installer/msime_setup.iss#L944
[M03a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/installer/default_config/config.default.toml
[M03b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/config/ime_config_template.cpp#L68

## v0.10.0 输入与整句

| 编号与更新 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| I01 中日英循环（#717） | 各语言已有，循环合同与事务缺少。 | P1 移植可选循环及严格结束组合；原串只提交一次，拒绝结束时不切模式；旧两端降级二态。 | [当前][I01c]、[合同][I01u] |
| I02 按软件 Vim Esc 切英（#737） | 待移植。普通 Esc 取消组合已有。 | P3 默认关闭白名单；无组合的裸 Esc 仍交给应用，组合内只取消；重复键及 Ctrl/Alt/Win+Esc 不误切。 | [当前][I02c]、[上游][I02u] |
| I03 双拼 Shift+Enter 转全拼（#642） | 待移植。普通 Enter 原串上屏不能替代。 | P1 合并 Server 与普通/延迟 TSF 分类器；小鹤 `nihc`→`nihao`，保留已选前缀，不当英文新词学习；空串交应用。 | [当前][I03c]、[上游][I03u] |
| I04 Caps Lock 标点及 Ctrl+. 手动覆盖（#771/#796） | 部分，当前跟随输入模式但忽略 Caps 边沿。 | P0 Caps 开用英文，关后恢复；开时手动改中文不被重复广播改回。与 I14、三语、迟键一起测。 | [当前][I04c]、[策略][I04u] |
| I05 五笔四码唯一自动上屏开关（#675） | 行为已有，缺独立开关。 | P1 默认保持 true；关后等待选择，非唯一、造词中、退格重算不误提交；与 I06 四组合测试。 | [当前][I05c]、[上游][I05u] |
| I06 五笔第五码顶字开关（#675） | 顶字已有，缺独立开关。 | P1 关且混拼开时保留第五码发展混拼；混拼关仍安全顶字，防四码截断吞键；首词仅提交一次。 | [当前][I06c]、[上游][I06u] |
| I07 Emoji/颜文字全后置初版（#677） | 余项已有后置，第一项仍插入较前位置。 | 由 J05 的最终策略覆盖，不先实现中间版本；保留固定位置和已有混输开关。 | [当前][I07c]、[U0][I07u] |
| I08 八行神经打分（#638） | 当前最高四行块，待扩充。 | P1 大于四行用八行，保留小块和标量回退；测尾行、bias 与多输入宽度误差，再测本机冷/暖 P50/P95。 | [当前][I08c]、[U0][I08u] |
| I09 选前缀后标点丢字（#769） | 待修复，TSF 普通标点路径缺已选前缀。 | P0 `nihao` 选“你”再逗号，完整上屏一次；prefetch、同步、智能标点、成对标点与最近上屏撤销回归。 | [当前][I09c]、[上游][I09u] |
| I10 混合简拼原样匹配优先（#827） | 有纠错和简拼基础，缺字面领衔保护。 | P1 `kandy`/`gaizenmb` 原样优先，纠错仍可选；预编辑、部分选词消耗、缓存和用户置顶一致。 | [当前][I10c]、[上游][I10u] |
| I11 声母加完整拼音不误纠错（#650） | 有尾声母保护，缺首声母保护。 | P1 `ywen`、`sshang` 保留边界；`zheg` 和真手误分别回归，查询与预编辑同判据。 | [当前][I11c]、[上游][I11u] |
| I12 日语完整读音转换优先（#667） | 已有转换/预测，当前预测仍可能抢首位。 | P1 精确与更长预测分离并限制预测权重；`hashi`、`shin'you` 不重复，不被二次排序推回错误首位。 | [当前][I12c]、[上游][I12u] |
| I13 日语 nn 作为单个ん（#669） | 日语已有，当前 nn 只消耗一个 n。 | P1 采用双字符消费规则；`rennai`、`n'`、nnn、后续元音及 pending 回归。旧常见拼法 `konnichiha` 与新规则的差异另评审，不承诺结果完全不变。 | [当前][I13c]、[上游][I13u] |
| I14 Caps 工具栏长按或锁屏反相（#822） | 有状态显示，缺 held 去重及完整回灌。 | P0 长按只反转一次，松开后可再切；TSF 快照、锁屏返回和外部状态更新一致，保留手动标点选择。 | [当前][I14c]、[上游][I14u] |
| I15 标签源码补充：模型中文路径 | 当前使用窄路径文件流，不等价支持 UTF-8 路径。 | P1 用文件系统路径转换；已有权重放中文、空格目录测试，坏文件和缺文件可读失败，无自动下载。 | [当前][I15c]、[U0][I15u] |

[I01c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/config/ime_config_input.cpp#L749
[I01u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/engine/contracts/trilingual_input.h#L16
[I02c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/windows/src/Key/KeyEventSink.cpp#L695
[I02u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/windows/src/Key/VimMode.h#L7
[I03c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/event_listener_keys.cpp#L862
[I03u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/server/src/ipc/input_key_policy.h#L33
[I04c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/windows/src/Composition/CompositionProcessorEngine_Modes.cpp#L344
[I04u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/windows/src/Key/CapsLockPunctuationPolicy.h#L3
[I05c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/input_key_policy.h#L73
[I05u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/server/src/ipc/input_key_policy.h#L81
[I06c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/input_key_policy.h#L79
[I06u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/server/src/ipc/input_key_policy.h#L94
[I07c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/candidate_selection_policy.h#L135
[I07u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/server/src/ipc/candidate_selection_policy.h#L134
[I08c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/neural/int8_linear_avx2.cpp#L64
[I08u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/engine/neural/int8_linear_avx2.cpp#L64
[I09c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/windows/src/Key/KeyHandler.cpp#L1554
[I09u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/windows/src/Key/PunctuationCommitText.h#L5
[I10c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/quanpin/quanpin_dictionary.cpp#L297
[I10u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/engine/quanpin/quanpin_utils.cpp#L16
[I11c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/quanpin/quanpin_utils.cpp#L155
[I11u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/engine/quanpin/quanpin_utils.cpp#L193
[I12c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/providers/japanese_candidate_provider.cpp#L107
[I12u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/engine/providers/japanese_candidate_provider.cpp#L122
[I13c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/japanese/romaji_converter.cpp#L65
[I13u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/engine/japanese/romaji_converter.cpp#L65
[I14c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/window/window_hook.cpp#L49
[I14u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/server/src/window/window_hook.cpp#L19
[I15c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/neural/sentence_model.cpp#L673
[I15u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/engine/neural/sentence_model.cpp#L674

## v0.10.0 设置与运行环境

| 编号与更新 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| U01 帮助页设默认输入法（#802/#807） | 待移植。 | 直接采用 J07 的中文键盘排序方案，不部署旧 override 初版；只在用户点击时执行，系统拒绝或未注册可解释失败。 | [U0 实现][U01u] |
| U02 Wiki、FAQ 和官网入口 | 现有帮助及官网，链接不完整且仍指官方。 | P3 补参考文档入口，区别上游资料与本分支下载；外链按统一校验打开。 | [当前][U02c]、[上游][U02u] |
| U03 国内镜像下载（#706） | 缺少；个人资产镜像本次 HEAD 返回 403。 | 验证支持个人仓库及同一资产摘要后才显示；错误 owner、repo 或资产拒绝，失效回退个人 GitHub 直链，见 F01。 | [当前][U03c]、[上游校验][U03u] |
| U04 工具栏柔和阴影及关闭开关（#809） | 有固定阴影，缺独立配置及新样式。 | P3 两渲染器和深浅主题一致，保留语音、嘴替、剪贴板按钮与尺寸，关闭无残影。 | [当前][U04c]、[上游][U04u] |
| U05 阴影开启时可贴边（#809） | 已有限位，仍按整个 host 而非可见工具栏处理。 | P3 阴影允许出界，可见按钮仍能点击；四边四角及任务栏不同位置测试，不吞任务栏点击。 | [当前][U05c]、[上游][U05u] |
| U06 混合 DPI 双屏拖动（#657） | 已有多屏区域处理，缺最终中心选屏和统一放置。 | P1 与阴影依赖一起合并；100/125/150/200%、负坐标、横纵排列、拔屏回归，保留扩展按钮计数。 | [当前][U06c]、[上游][U06u] |
| U07 快捷键页补全（#794） | 部分，已有语音、发音、撤销等本分支说明。 | P3 补 Ctrl+Enter、Shift+Enter 和 Ctrl+Shift+E 的真实条件；未实现按键不提前写成可用，保留原扩展。 | [当前][U07c]、[上游][U07u] |
| U08 微信玻璃外部皮肤（#732） | 可选新皮肤。 | P3 独立加入，不改原默认；保留许可和同名修改备份。它采用纹理近似，不能宣传为真正透视毛玻璃。 | [皮肤定义][U08u] |
| U09 豆包 Resource ID 可编辑（#648） | 后端、配置回读和测试参数已有，页面缺可编辑字段。 | P2 补自动保存字段及清空回落，重开仍保持；不复制上游精简 ASR 列表丢掉本地、腾讯或阿里选项。 | [当前读取][U09c]、[上游字段][U09u] |
| U10 非豆包隐藏鉴权方式（#746） | 已有。 | 复用，只扩本地及各新增厂商切换回归；隐藏不重置凭据、Resource ID 或鉴权槽。 | [当前][U10c]、[上游][U10u] |
| U11 候选逗号与私用字符（#766） | 旧编码用 U+F000 替换逗号，原生私用字符仍可能被误还原。 | P0 改结构化视图数据，保留拼音和 Ctrl 发音；逗号、引号、HTML 字符、原生 U+F000、空槽与异体字两渲染器回归。 | [当前][U11c]、[上游][U11u] |
| U12 仅页码及箭头变化也刷新（#834） | 当前签名未包含完整页状态。 | P0 同一不可变快照包括页号、前后页与选中；相同词但不同页状态须刷新，旧代次不可反盖。 | [当前][U12c]、[上游][U12u] |
| U13 VC++ 运行库最低 14.40（#801） | 当前要求 14.20，未等价完成。 | P0 升级最低检测并保留双注册表视图；缺失、14.20、14.39、14.40 和更新版本测试，复核实际启动。 | [当前][U13c]、[上游][U13u] |
| U14 缺运行组件时打开直接下载地址（#801） | 已有提示，当前多为介绍页。 | P0 改官方直链并说明用户需安装后重新检测；拒绝不下载。这是打开地址，不宣称已自动安装运行组件。 | [当前][U14c]、[上游][U14u] |
| U15 设置 WebView2 启动失败说明（#801） | 当前部分失败只结束 splash 或返回。 | P0 同步及异步失败均给阶段和 HRESULT，结束空白窗口；保留新增路由，Server 打字正常。 | [当前][U15c]、[上游][U15u] |
| U16 缺 WebView2 候选临时降级 D2D（#801） | 缺少完整降级。 | P0 只改本进程有效后端，不覆写用户选择；缺组件仍打字，安装后重启恢复，D2D 发音及拼音仍正常。 | [上游][U16u] |
| U17 热力图默认看最新日期（#696） | 首次恢复 scrollLeft=0，未实现跟随最新。 | P3 首次贴最新，用户主动看历史后刷新不抢位置；窄窗、无记录和周数变化测试。 | [当前][U17c]、[上游][U17u] |
| U18 月份标签不重叠（#696） | 有标签布局，首列补标签缺最小间距。 | P3 计算月份标签并保留统计管线；残月一两列、月初第一周、跨年和不同周窗口回归。 | [当前][U18c]、[上游][U18u] |
| U19 手写撤销图标（#749） | 当前直接使用文本字符，缺字体回退。 | P3 图标字体有回退，仍无字则文字；撤销一笔、重写、深浅主题与高 DPI 不改变识别记录。 | [当前][U19c]、[上游][U19u] |
| U20 D2D 原尺寸图片对齐物理像素（#649） | 待移植。 | P3 合并 transform 与 clip 守卫；100/125/150%，缩放、旋转、裁剪、皮肤图及 OCR 预览回归，不让通用 UI 反向依赖产品。 | [当前][U20c]、[上游][U20u] |
| U21 直接辅码“三码字优先、四码词优先”（#753） | 当前帮助仍有旧描述。 | P3 按实际引擎规则改说明并给样例；不拿文案代替算法验证，保留原设置结构。 | [当前][U21c]、[上游][U21u] |

[U01u]: https://github.com/metasequoiaime/msime-windows/blob/0d7ae4ca795708261c354316e9f0554a9cbc35d6/server/src/settings/default_input_method.cpp#L20
[U02c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/partials/help-settings.html#L1
[U02u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/partials/help-settings.html#L1
[U03c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/modules/about-settings.ts#L7
[U03u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/modules/update-manifest.ts#L27
[U04c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/window/floating_toolbar_presenter.cpp#L46
[U04u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/window/floating_toolbar_presenter.cpp#L46
[U05c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/window/ime_windows_toolbar.cpp#L83
[U05u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/window/ime_windows_toolbar.cpp#L53
[U06c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/window/ime_windows_toolbar.cpp#L120
[U06u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/window/ime_windows_toolbar.cpp#L83
[U07c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/partials/shortcut.html#L1
[U07u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/partials/shortcut.html#L38
[U08u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/skins/wechat_glass/skin.toml#L1
[U09c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/modules/voice.ts#L129
[U09u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/partials/voice.html#L92
[U10c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/modules/voice.ts#L232
[U10u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/modules/voice.ts#L221
[U11c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/webview2/candidate_window_template.h#L13
[U11u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/webview2/candidate_window_template.h#L10
[U12c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/window/ime_windows_candidate.cpp#L120
[U12u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/window/candidate_view_model.h#L122
[U13c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/installer/msime_setup.iss#L256
[U13u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/installer/msime_setup.iss#L264
[U14c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/installer/msime_setup.iss#L291
[U14u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/installer/msime_setup.iss#L377
[U15c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/settings/settings_app.cpp#L1428
[U15u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/settings/settings_app.cpp#L132
[U16u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/webview2/windows_webview2.cpp#L1299
[U17c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/modules/stats.ts#L246
[U17u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/modules/stats.ts#L274
[U18c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/modules/stats.ts#L323
[U18u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/modules/stats.ts#L177
[U19c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/handwriting-panel/HandwritingPanel.cpp#L227
[U19u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/handwriting-panel/HandwritingPanel.cpp#L77
[U20c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui/src/Image.cpp#L127
[U20u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui/src/Image.cpp#L42
[U21c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/partials/helpcode.html#L57
[U21u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui-html/webview2/settings/ime-settings/src/partials/helpcode.html#L57

## v0.10.0 构建与发行

| 编号与更新 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| B01 SignPath 签名（#709） | 已有暂存、还原、请求签名结构，个人仓库服务配置未具备。 | 可选发行工作；需要本仓申请与凭据。校验只签自有产物、排除第三方运行库；无配置仍发明确未签名包，不宣称 uiAccess。 | [当前流程][B01c]、[上游流程][B01u] |
| B02 SignPath 使用托管 runner（#709） | 普通 CI 已用托管，当前 Release Windows 构建仍自托管。 | P4 仅 SignPath 路径使用托管 Windows，证书路径保留对应环境；干净 runner 上打齐本分支 runtime、词库与许可证，不能删掉额外资源。 | [当前][B02c]、[条件选择][B02u] |
| B03 Engine sccache 及 MSVC 回退（#770/#832） | Server 有缓存，Engine 和失败回退不完整。 | P4 增加启动器能力判断、Ninja Multi-Config 与服务错误回退；缺二进制、setup 失败、服务不可达均能构建，不虚报提速。 | [当前][B03c]、[上游助手][B03u] |
| B04 从 msime-dictionary 源构建（#654/#784） | 缺完整新构建模块；正式标签发行链路仍用旧锁，见 D01。 | P1 固定源版本和外部输入锁，保留青简及行业层；坏输入、脏缓存和生成失败不得复用旧产物，比较数据库与最终 manifest。 | [源构建][B04u]、[输入模块][B04b] |
| B05 首次本地构建准备依赖（#823） | vcpkg/CI 部分已有，首次 pnpm/pypinyin 准备不足。 | P4 开发人员主动运行构建时准备这些依赖；离线已有缓存和安装失败可解释退出，不宣称自动装齐 VS、SDK、Boost，不自动拉语音或 OCR 模型。 | [当前][B05c]、[上游][B05u] |
| B06 按键各阶段耗时（#643） | 已有整键、队列和候选阶段，缺更细子阶段。 | P0 合并 RAII 分段及阈值，按 request/client/epoch 关联；慢引擎、选词和异步分开归因，诊断关闭无持续 IO，日志不含输入与凭据。 | [当前][B06c]、[上游][B06u] |
| B07 移除旧设置宿主约 2500 行（#764） | 两旧文件仍编译，含扩展字段及消息代码；有效调用须追踪。 | 迁移闭环后才清理；先验证实际 Settings 全部消息和配置双向回读，不按上游 CMake 全文件覆盖。语音、嘴替、OCR、资料库及模板入口保持。 | [当前构建][B07c]、[U1 构建][B07u] |

[B01c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/.github/workflows/release.yml#L548
[B01u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/.github/workflows/release.yml#L1
[B02c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/.github/workflows/release.yml#L156
[B02u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/.github/workflows/release.yml#L156
[B03c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/.github/workflows/ci.yml#L136
[B03u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/scripts/ci/cmake-sccache-args.ps1#L13
[B04u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/scripts/build-dictionary.py#L1
[B04b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/scripts/dictionary_build/inputs.py#L1
[B05c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/installer/Invoke-LocalTest.ps1#L56
[B05u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/installer/Invoke-LocalTest.ps1#L58
[B06c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/event_listener_keys.cpp#L523
[B06u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/ipc/event_listener_keys.cpp#L520
[B07c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/CMakeLists.txt#L282
[B07u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/CMakeLists.txt#L1

## v0.10.1 最终行为与源码补充

| 编号与更新 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| J01 模糊读音按词频混排（#860） | 模糊查询已有，排序未等价。 | P1 原读音和模糊分组混排，保留折扣、标记、去重与用户偏好；`min`、规则关闭、温态缓存、全拼/双拼均测。 | [当前][J01c]、[U1][J01u] |
| J02 模糊音参与整句词格（#860） | 待移植，不等同于 J01 词级候选。 | P1 全拼/双拼词格下发选项、限定变体数与逐音节扣分；`lin'yi'bian` 可得“另一边”，实际读音保留，不污染精确查询。 | [当前][J02c]、[U1][J02u] |
| J03 等音节切分按整句仲裁及预编辑跟首选（#859） | 有词格和纠错基础，缺字面枚举及显示联动。 | P1 `yibananzhao`→“一般按照”，预编辑 `yi'ban'an'zhao`；保护手动分隔、整词与用户调频；平局保原切分，有限枚举不阻塞。 | [仲裁][J03u]、[预编辑][J03b] |
| J04 合法全拼不误纠错、不吞 j（#859） | 当前完整拼音判定仍贪心。 | P0 `zaijinuta` 选“再”剩 `jinuta`；查询与消费同判据。`buuhui` 等真手误仍正确消费，辅助码与撤回回归。 | [当前][J04c]、[U1][J04u] |
| J05 第一 Emoji 首页末位（#853） | 有混输插槽，缺最终放置策略。 | P1 覆盖 I07。余 Emoji 末尾；与英文补全共存时英文倒数第二，用户固定位置仍优先。零/一页容量、迟回顺序和翻页不丢候选。 | [当前][J05c]、[U1][J05u] |
| J06 候选按文字收紧宽度（#858） | 缺 fit_content 全链路。 | P3 配置、皮肤、WebView2 与 D2D 同步；短词、长句、拼音、译文、页箭头及高 DPI 不裁字，关闭保原最小宽度。 | [皮肤][J06u]、[D2D 测量][J06b] |
| J07 设默认改为中文键盘第一位（#862） | 待移植。 | P3 采用最终排序而非 U01 旧 override；验证系统设置和 Win+Space，卸载只清本产品旧 override，不改其他语言及输入法。 | [最终 API][J07u]、[卸载清理][J07b] |
| J08 工具栏也中日英循环（#857） | 目前按钮仍二态。 | 与 I01 一起完成，按钮投递权威 worker 事务，不在页面自建状态；键盘与点击交错一致，结束组合失败不广播假状态。 | [当前][J08c]、[U1][J08u] |
| J09 新安装关闭候选翻译（#855） | 当前新装默认 true，属于策略差异。 | 不自动改本分支默认或旧开关；若确认采用新装 false，先完成 P03。新装与旧默认 true/用户 false 分别验收。 | [U1 模板][J09u]、[U1 漏项][J09b] |
| J10 辅码补瑠和弔（#856） | 缺源码及分发产物更新。 | P1 加 `瑠=wt`、`弔=ga`，重建校验默认表；用户自定义覆盖优先，单辅、双辅、直接辅码均测。 | [U1 表][J10u] |
| J11 任务栏稳定图标路径（#863） | 当前用带版本 DLL 路径，缺稳定图标方案。 | P1 稳定 ico 配合安装器；升级、旧目录清理、x64/x86 与重新登录检查；缺 ico 可退 DLL 资源，profile 身份不变。 | [当前][J11c]、[U1][J11u] |
| J12 标签额外修复：旧设置与模型保留（#852） | 当前无同等迁移；U1 保留部分默认并尝试接收 SHA 匹配模型，但翻译漏项仍见 P03。 | P0 旧设置即使等于旧默认仍保留；模型校验失败、目标冲突或 Rename 失败也不删。测试备份、迁移、中断恢复和全部扩展配置。 | [设置保留][J12u]、[模型迁移][J12b] |

[J06u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/skin/candidate_skin_catalog.cpp#L422
[J06b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/ui/src/CandidateList.cpp#L456
[J07u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/settings/default_input_method.cpp#L145
[J07b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/installer/msime_setup.iss#L1703
[J09u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/installer/default_config/config.default.toml#L58
[J09b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/config/ime_config_template.cpp#L68
[J12u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/config/ime_config_template.cpp#L68
[J12b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/installer/msime_setup.iss#L944
[J01c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/quanpin/quanpin_dictionary.cpp#L1920
[J01u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/engine/quanpin/quanpin_dictionary.cpp#L2208
[J02c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/quanpin/quanpin_query.cpp#L1282
[J02u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/engine/quanpin/quanpin_query.cpp#L1284
[J04c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/engine/quanpin/quanpin_utils.cpp#L46
[J04u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/engine/quanpin/quanpin_utils.cpp#L84
[J05c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/candidate_selection_policy.h#L235
[J05u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/ipc/candidate_selection_policy.h#L242
[J08c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/server/src/ipc/event_listener_worker.cpp#L367
[J08u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/server/src/ipc/event_listener_keys.cpp#L900
[J11c]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/windows/src/Register/Register.cpp#L48
[J11u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/windows/src/Register/Register.cpp#L36
[J03u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/engine/quanpin/quanpin_dictionary.cpp#L1183
[J03b]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/engine/core/input_session_composition.cpp#L421
[J10u]: https://github.com/metasequoiaime/msime-windows/blob/0b39cbfa2752ab9724a241c6b05cf13d1e00f4d2/engine/helpcode/helpcodes/helpcode.txt#L8263

## v0.10.1 发布后修复

以下四项不属于两版已发布内容。后续移植以固定提交和必要依赖为单位，分别记录迁移与回归。

| 编号与更新 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| P01 八股文单次加成与量纲 | 当前有解码和重排双注入结构。 | P1 移植统一加成；保留用户高级参数并解释新映射。〔万象〕整句排序、同音错误、关闭模型与旧配置分别评测。 | [d495ea2][P01a] |
| P02 AI 候选翻译与单一 provider | 当前有多个 enabled 的优先级，Vision 的同名字段属另一个工作台。 | 独立适配，不跨功能覆写。保持本地/个人释义优先和在线总门禁；旧开关组合、全部凭据、缓存作用域与晚回响应回归。 | [add122fc][P02a] |
| P03 老用户候选翻译开关保留（#884） | U1 将新装默认改 false，却遗漏旧值保留名单，仍可能关掉旧默认 true。 | P0 在任何新默认前移植。旧 true/false、旧 baseline 缺失、坏 TOML恢复及旧在线 provider 均保持原选择。 | [341ca93][P03a] |
| P04 纯简拼不被纠错抢占（#881） | 当前缺此最终排序规则。 | P1 保留纠错，纯简拼精确词优先；`nbn`、`nqn`、预编辑与单字消耗回归，`bng` 仍保留“帮”等纠错可选项。 | [fc90cb1][P04a] |

[P01a]: https://github.com/metasequoiaime/msime-windows/commit/d495ea2ad0ce2141c9cbeaed456a643638c1bb21
[P02a]: https://github.com/metasequoiaime/msime-windows/commit/add122fcc69aa22ff7ec3959467d01bd09d0ee10
[P03a]: https://github.com/metasequoiaime/msime-windows/commit/341ca93be56efbfc9a2ccde003fad8f333e32add
[P04a]: https://github.com/metasequoiaime/msime-windows/commit/fc90cb1f2c765976ae2c3e48e5b9ea47766685a6

## 青简需求与本分支保留项目

| 编号与保护项 | 当前状态 | 安排与验收 | 源码依据 |
|---|---|---|---|
| F01 个人分支更新身份与预发布频道 | 官方 manifest 与数值比较仍是当前入口；校验器只接受上游，个人 `/releases/latest` 因 prerelease 返回 404。 | P0 先固定产品、`DiXuanAoYi/msime` 仓库、chat 频道和资产，再比较版本；不推荐官方覆盖个人功能，镜像受 U03 约束。测试错误身份拒绝和本分支预发布可发现。 | [当前更新入口][F01a]、[当前 URL 校验][F01b] |

[F01a]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/modules/about-settings.ts#L7
[F01b]: https://github.com/DiXuanAoYi/msime/blob/7ac7b2a31028e614a9e4a434eb125ad3dcc75e21/ui-html/webview2/settings/ime-settings/src/modules/update-manifest.ts#L64

英文状态候选即时同步、11 类领域输入库、含章 P2C、本地或云端整句补全、问字与模板管理按主规划实施。这些是本分支需求，不冒充水杉 v0.10.0 或 v0.10.1 的新功能。

移植必须保留语音与发音、模型下载大小和手动启动、嘴替预回复且不自动发送、每人/每群聊天上下文、OCR与翻译工作台、字幕编辑保护、剪贴板分组、软件独立学习、带调拼音开关、最近上屏撤销及现有真实消息合同。上游源文件删除这些扩展的差异不作为“清理死代码”自动套用。
