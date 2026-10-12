# P0–P4 实施记录

记录日期：2026-10-12；构建版本：`0.10.2`。用户已确认全部规划并授权编译、实机验证及个人仓库 Release。**P0–P4 源码、指定编译与自动化回归、真实升级及代表性实机验证已完成；Release 与未具备真实条件的边界按最终交付区单独记录。**

范围对应[功能规划](qingjian-upstream-v010-plan.md)和[79项版本对照](upstream-v010-release-checklist.md)。对照表“当前状态”是冻结分支 `7ac7b2a` 的移植前基线；下面逐编号给出实施覆盖。自动化通过不表示全部软件和所有环境实机通过。

## 保留边界

- 升级前配置、聊天、个人词库、学习和统计数据库已经备份。实机升级及新版启动后语义与记录比对通过，38 个已有模型及语音草稿文件大小/SHA256逐项一致；最终重新安装的清洁模板包另作同样比对。
- 不自动下载 ASR/TTS/OCR/QJM/desktop 权重，不自动发送回复、语音或聊天。keyboard 速度档仍按固定来源随包；完整词典、开发依赖和模型权重分别处理。
- API 使用当前用户 DPAPI；聊天文件原加密格式不变。不可解密的 API 密文存档、不作 token，不声称旧版或其他账户可直接解密。
- 旧值即使等于历史默认也保留，扩展槽位、模板、学习记录和原模型不因移植重置；不采用“清旧模型重下”等冲突方案。
- 日志路径相对本轮工作区根。`work/`、`outputs/` 不是用户必须创建的磁盘；日志中的 W: 只是本机工作目录别名。

## 编译与执行证据

只引用最终成功结果。早期失败日志保留用于定位，不计为通过；例如 `work/vnext-dictionary-verify.log` 是验证错误父目录的失败记录，源构建应核对 E08 的实际 out manifest。

| 证据 | 执行操作与结果 | 记录和证据边界 |
|---|---|---|
| E01 Server | `ctest --test-dir server/build-release -C Release --output-on-failure`：最终4/4通过，28.61秒（727原生用例、102共享契约夹具） | `work/v0102-domain-final-ctest.log`、`v0102-domain-final-last-test.log`；英文学习隔离、语音控制、主套件、WebView合同；不是真云或全软件实机。 |
| E02 TSF | canonical 两架构 clean-first 构建；对应 `windows/build32-release`、`build64-release` 执行 `ctest -C Release --output-on-failure`，各16/16通过 | `work/v0102-canonical-tsf-{32,64}-{build,ctest}.log`、`v0102-canonical-tsf-freshness.json`；每架构6类所需object、stale=0，DLL/PDB摘要与0.10.2.0版本。涵盖隐私、运行时设置、功能按键、Caps/前缀标点和撤销策略；真实输入另验。 |
| E03 Engine | 独立Release全构建后 `ctest -C Release --output-on-failure`：28/28通过，35.94秒 | `work/v0918-engine-final-all-tests.log`。普通neural_sentence缺模型会skip；真实权重另见E04，不拿skip当推理成功。 |
| E04 真实已有权重 | keyboard 4,486,280B、desktop 25,480,184B复制到中文加空格目录，SHA256匹配锁；`test_neural_sentence.exe`成功，`bench_neural_rescore.exe 5`：180场景全ok | `work/v0918-neural-real/{verification.json,parity.log,latency.csv}`。keyboard批次/逐字参考对拍，desktop真实加载/异步缓存/重排；每冷暖组45样本，加载不计时、模型KV跨行保留、负载未控制，不是生产TSF操作延迟或全模型标量速度对照。 |
| E05 原生UI | 独立UI `ctest -C Release -V`：1个目标内52项通过，2.84秒 | `work/v0918-ui-native-final-all-tests.log`；实际离屏D2D目标DPI100/125/150/200%、缩放/旋转/裁剪、拼音/译文及发音命中；不等于物理双屏拖动。 |
| E06 Settings | `pnpm test`：最终41文件/221测试通过；`pnpm build`成功 | `work/v0102-ui-post-isolation-tests.log`、`v0102-ui-post-isolation-build.log`；包括主题/临时配置提示、完整统计/筛选分页、软件规则编辑校验、消息及候选预览异常隔离，不能替代全部原生文件对话框和导航。 |
| E07 11领域库 | `test_domain_input.exe <真实DB> <assets/domain-input>`：11真实包逐包生产全拼召回；构建/Engine fixture覆盖132,916条、逐库及双拼/简拼/词格、来源与失败保护 | `work/v0918-domain-production-queries-final.log`、E03；DB44,068,864B，SHA256 `6775928530f4a4ace872bbb529ae720d81b29fc72b851b0dd42f6b23131380d5`。真实逐包是全拼；其他方式是fixture，不冒充逐条实机。 |
| E08 源词典 | 固定 `msime-dictionary@4909582a8ce345c23809750e675e3d9e5f9516df` 全量build-dictionary.py成功；test_dictionary_build.py的7个坏输入/脏缓存/复制/rename/旧产物保护测试通过 | `work/vnext-source-dictionary-build.log`、`vnext-source-dictionary/out/local-dictionary.json`、`vnext-source-builder-fixtures-escalated.log`；隔离目录，manifest记录来源、builder与输出摘要，无用户词库覆盖。 |
| E09 全义项/导出 | 912,640机器数据叠加人工覆盖后912,762条；真实GUI再次另存为成功，逐行计数和Get-FileHash核对TSV | `outputs/upstream-cn-en.tsv`：912,762数据行、34,642,886B，SHA256 `48521c367c1172a674c6441ade3f720454c45c79a9d4365540e0afcbb1fe9b58`。PID10408本次export trace begin83943.2/end103768.7无failed/generic modal；早先一次后台error原根因没有回溯定位，安全诊断/页内提示已加入回归，不声称原因已找到。 |
| E10 QJM/管理 | 既有Torch执行test_neural_qjm.py：9/9，人工tiny Transformer真实CPU KV/beam/重排、格式/导入保留；打包runtime生命周期fixture6/6 | `work/vnext-final-qjm-fixture-tests.log`、`vnext-packaged-model-lifecycle.log`；不是含章正式大权重、GPU/质量或官方下载实测，没有自动下载权重。 |
| E11 runtime | Build-Runtime.ps1的46测试执行成功（其中4因打包解释器无Torch而skip）；PyInstaller收集成功 | `work/vnext-runtime-final.log`；E10在另一个已有Torch环境实际执行对应tiny推理。ASR/TTS/OCR/神经worker与native依赖保留，不等于各正式模型/云厂商推理已测。 |
| E12 回环HTTP | 真CURL/worker连接自有HTTP fixture：拼音/补全/问字/多音/模型表/连接/重排；候选泰意代码、个人优先、晚回scope和英文库保留均true | `outputs/vnext-ai-loopback/native-report.json`、`outputs/vnext-candidate-translation-loopback/native-report.json`；人工HTTP响应验证调用链，不能证明真实云API或模型质量。 |
| E13 包装保护 | full/light、来源/许可、测试EXE排除、失败staging、构建失败中止及皮肤备份fixture通过，真实完整包收集成功 | `work/vnext-package-fixtures-final.log`、`vnext-build-failure-fixtures.log`、`vnext-bundled-skin-fixtures.log`、`vnext-package-real.log`；实际升级另验。 |
| E14 开发依赖 | EnsureBuildDeps准备pnpm/固定pypinyin与离线缓存路径成功 | `work/vnext-ensure-build-deps-second.log`、`vnext-dependency-offline.log`；Vulkan显式SDK/loader bootstrap，不装driver，不保证自动装全部VS/Boost/应用运行库。 |
| E15 CI/cache | Python4、PS cache6、registry4 fixture通过；真实MSVC/Ninja故障cache编译/运行/增量bypass通过；全仓actionlint1.7.12与6个PS文件parse通过 | `work/v0918-p4-ci-verification.log`；本机未安装shellcheck/pyflakes，actionlint禁用这两外部检查；未跑hosted作业/SignPath，不宣称提速。 |
| E16 fallback宿主 | native_recovery_probe：3自有DComp HWND cloak=0、旧region清除、位置、可见帧、命中查询、回滚、foreground保持 | `work/v0918-native-recovery-verification.json`、`v0918-native-recovery-pixel-details.log`；实际120DPI，无输入注入，屏幕颜色容差受色彩管理；不是全产品按钮重放/其他物理DPI。 |
| E17 产品/安装器 | Server/Settings/runtime和TSF产出；Inno成功编译MetasequoiaIME_Setup_v0.10.2.exe | `work/v0102-regression-product-final-build.log`、`v0102-ui-post-isolation-build.log`、`vnext-runtime-final.log`、`vnext-installer-build.log`；不等于已安装/升级/签名/发布，后续重包以交付摘要为准。 |
| E18 候选策略 | 正式测试源独立实编执行9/9（Emoji首页末位、英文/固定槽、重排metadata及五笔开关） | `work/v0918-candidate-policy-smoke.log`；真实组合输入另验。 |

E04混合场景settle P50/P95（ms）：keyboard冷103.110/140.566、暖104.882/145.864；desktop冷333.082/606.567、暖338.068/633.267。初始静态候选P95为4.435–5.025ms。仅描述本轮基准，不得据此声称暖态一定更快或新版本提升多少。

## 阶段完成映射

“实现+回归”限于已接线源码、编译与指定自动化。“按批准策略保留”是落实取舍；“可选框架”不表示外部服务已授予资格。

| 阶段 | 已完成 | 仍需真实条件 |
|---|---|---|
| P0 | 配置/凭据保留、敏感/未知门禁、临时前文scope、即时英文快照、前缀标点/分页、fallback、更新身份；E01/E02/E06/E13/E16 | 密码/PIN/普通输入实机、真实升级文件比对、上线资产 |
| P1 | 切分/消费、mixed/模糊词格/日语、三语/五笔、11领域库、完整义项、路径/八行/八股文；E01–E05/E07–E09/E18 | 新版TSF上屏/消费/撤销、逐软件真实词库使用 |
| P2 | 问字/补全、模型管理/QJM适配、local/Custom、模板/槽位/模型列表/Resource ID；E01/E06/E10–E12 | 含章正式权重/GPU质量、真实云、全导航 |
| P3 | fit-content、阴影/贴边/跨屏策略、工具栏、统计、矢量图标、像素/皮肤/帮助；E01/E05/E06/E13/E18 | 全主题/实际双屏、系统默认输入法、各软件操作 |
| P4 | 固定源、依赖/cache/条件hosted、旧宿主清理、runtime/数据/许可及安装器；E08/E11/E13–E17 | 最终安装/保留/发布；hosted SignPath仍为未执行可选条件 |

## 79项逐编号实施覆盖

对应冻结版本表的全部编号；一个套件通过不等于该行所有环境已验收。

| 编号 | 当前实施结果 | 证据 | 剩余边界 |
|---|---|---|---|
| D01 | 完整中译英独立来源层、开关、完整导出 | E08/E09 | 完整回导另验 |
| D02 | 复用 Ctrl+Enter 去词性上屏、子页及 Ctrl 发音 | E01/E02/E05 | 真实上屏另验 |
| D03 | 两 renderer 的词性/分号颜色及完整详情 | E01/E05/E06 | 两 renderer 实机主题另验 |
| D04 | 泰语、意大利语与既有越南语的候选配置/厂商代码 | E01/E06/E12 | 真实厂商未测 |
| D05 | 按批准策略保留原 provider 与云总门禁，不强制新装切小牛 | E01/E06 | 实际升级回读另验 |
| D06 | 取消六义项合并上限；去重、个人覆盖、详情及导出保全 | E01/E03/E06/E09 | 完整 GUI 回导另验 |
| A01 | 用户触发模型列表、作用域/错误/取消接线 | E01/E06/E12 | 真实厂商列表未测 |
| A02 | 可手填 combobox、键盘及失败列表保留 | E06 | 全设置键盘导航另验 |
| A03 | 复用测试连接并扩 local/Custom | E01/E06/E12 | 连接不等于模型质量 |
| A04 | 独立 Custom/local endpoint/model/token 槽位 | E01/E06/E12 | 正式模型质量未测 |
| A05 | DeepSeek 思考强度、预算/取消按用途保存，默认关闭 | E01/E06 | 真实 DeepSeek 未测 |
| S01 | Regular/Sensitive/Unknown 合同、AI与学习门禁 | E01/E02 | 真实密码/PIN/未知输入框另验 |
| S02 | client/epoch/焦点变化清临时前文，保留聊天历史 | E01/E02 | 应用切换实机另验 |
| S03 | 发送副本过滤疑似凭据，不改原文 | E01 | 不保证识别全部私人信息 |
| S04 | 扩展 TTS/OCR/API DPAPI、旧明文迁移及失败保留 | E01/E13 | 实际升级后比对另验 |
| S05 | 不可解密密文存档且不作 token，跨账户/降级边界 | E01 | 不承诺旧版可解密 |
| M01 | desktop 按需管理、固定大小/摘要；keyboard 随包 | E01/E06/E10/E13 | 未执行新正式权重下载 |
| M02 | 不清旧模型重下；冲突/校验或迁移失败保留 | E10/E13 | 升级后实际模型 hash 另验 |
| M03 | 按批准策略保留本分支新装默认及旧用户值 | E01/E13 | 实际新装/升级分别另验 |
| I01 | 中日英 worker 事务、结束组合失败保护、旧端降级 | E01/E02 | 三语实机另验 |
| I02 | Vim Esc 软件白名单、修饰键/组合条件，默认关闭 | E02/E06 | 编辑器实机另验 |
| I03 | 双拼 Shift+Enter 转全拼，保前缀且不学成英文词 | E01/E02/E03 | 真实上屏另验 |
| I04 | Caps 标点边沿与 Ctrl+. 手动覆盖 | E02 | 真实长按/锁屏另验 |
| I05 | 五笔四码唯一自动上屏开关 | E01/E02/E18 | 真实五笔操作另验 |
| I06 | 五笔第五码顶字开关及混拼防吞键 | E01/E02/E18 | 真实五笔操作另验 |
| I07 | 采用 J05 最终首页末位，未先移植中间后置方案 | E18 | 见 J05 |
| I08 | 八行 int8、尾行/bias/宽度标量对拍和真实权重重排 | E03/E04 | 无全模型标量速度 A/B 结论 |
| I09 | 已选前缀加标点一次提交，普通/延迟共享策略 | E01/E02 | 真实标点及撤销另验 |
| I10 | mixed 简拼字面领衔、消费及缓存联动 | E01/E03 | 新版 TSF 实机另验 |
| I11 | 首声母保护按实际字面候选判定，真纠错仍可选 | E01/E03 | sshang 无字面短语仍提供“上” |
| I12 | 日语 exact 与较长预测优先级分离 | E03 | 日语 TSF 实机另验 |
| I13 | nn 双字符消费；兼容 konni/konnichiha/konnichiwa | E03 | 不承诺全部非标准罗马字结果不变 |
| I14 | Caps held 去重及运行时回灌 | E02 | 真实锁屏/外部状态另验 |
| I15 | UTF-8 文件系统模型路径；既有两权重中文/空格加载 | E03/E04 | 无新权重下载 |
| U01 | 采用 J07 最终中文键盘排序入口，未采用旧 override 初版 | E01/E06 | 系统实际排序另验 |
| U02 | 本分支下载与上游帮助参考入口分开 | E06 | 外链实际操作另验 |
| U03 | 个人镜像未验证支持，保持禁用/回退个人 GitHub 直链 | E06/F01 | 不声称官方镜像托管个人资产 |
| U04 | 两 renderer 阴影及关闭开关，保留扩展按钮 | E01/E05/E06 | 工具栏主题实机另验 |
| U05 | 可见内容贴工作区边、阴影可出界、工作区 region | E01/E05 | 真实任务栏各边另验 |
| U06 | 中心选屏、负坐标、建议 RECT 与可见屏裁剪 | E01/E05 | 物理双屏/拔屏未测 |
| U07 | 新按键真实条件及原语音/发音/撤销帮助 | E02/E06 | 快捷键实机另验 |
| U08 | 独立 wechat_glass 纹理、许可证及修改备份 | E13 | 纹理近似，不是真透视玻璃 |
| U09 | 豆包 Resource ID 可编辑、自动保存、清空回落 | E01/E06 | 真实豆包未测 |
| U10 | 非豆包隐藏鉴权控件，不清原槽位 | E01/E06 | 实际切厂商回读另验 |
| U11 | 结构化 payload 保留逗号、PUA、拼音及发音 | E01/E05 | 两 renderer 真实输入另验 |
| U12 | 页码/箭头/选中进入快照与刷新签名，拒旧代次 | E01/E02/E06 | 实机翻页另验 |
| U13 | VC runtime 14.40 门槛及双注册表视图检测 | E01/E13 | 最低版本机器启动未测 |
| U14 | 提示打开微软直链并重检 | E01/E13 | 不声称自动完成运行库安装 |
| U15 | Settings WebView 同步/异步失败阶段与 HRESULT 提示 | E01 | 实际全部故障路径另验 |
| U16 | 进程原子 native fallback、旧 HWND 状态事务及 late 回调门禁 | E16/源码 | 保存 backend 不变；完整产品故障注入另验 |
| U17 | 热力图首贴最新，用户看历史后不抢位置 | E06 | 真实窄窗/滚动另验 |
| U18 | 月份标签、边界与跨年间距计算 | E06 | 实际布局另验 |
| U19 | 手写撤销/清除改矢量图标加中文文字 | E17 | 真实笔画撤销另验 |
| U20 | 1:1 transform/clip 物理像素对齐，保留缩放/旋转 | E05 | 其他物理屏幕 DPI 未测 |
| U21 | 直接辅码帮助采用“三码字优先、四码词优先” | E01/E06 | 保留算法；实机另验 |
| B01 | 原 SignPath 暂存/还原/请求框架保留 | 可选 | 未执行正式 SignPath，未证明本仓签名资格 |
| B02 | 仅 SignPath 条件 hosted Windows，其他签名模式保留自托管 | E15 | Hosted 实际作业未执行 |
| B03 | Ninja/MSVC cache helper 及 setup/service/compile 故障回退 | E15 | 无命中率/提速结论 |
| B04 | 固定完整源构建、外部锁、拒伪缓存、旧产物保留 | E08/E13 | 最终资产来源 manifest 另核 |
| B05 | 主动开发构建时准备 pnpm/pypinyin 及明确 Vulkan 依赖 | E14/E17 | 不装 driver，不下载模型 |
| B06 | 按阶段计时/归属与诊断门禁 | E01/E02 | 不记输入/凭据；实际慢键归因另验 |
| B07 | 追踪后删旧 Settings 宿主，现代 Settings EXE 及扩展保留 | E01/E06/E17 | 全导航/回读实机另验 |
| J01 | 模糊组与原读音按折扣词频混排 | E01/E03 | 真实语料质量另评 |
| J02 | 模糊变体进入词格，按实际读音扣分 | E01/E03 | 真实语料质量另评 |
| J03 | 等音节合法切分仲裁，预编辑跟首选、保手动分隔 | E01/E03 | 新版 TSF 实机另验 |
| J04 | 查询/消费共享合法全拼判据并保真手误 | E01/E02/E03 | 真实选“再”剩余串另验 |
| J05 | 首 Emoji 首页末位，英文相邻，固定位置优先 | E01/E18 | 真实翻页/迟回另验 |
| J06 | skin fit_content +用户三态；Web/native/预览同步 | E01/E05/E06 | 两 renderer 实机另验 |
| J07 | 用户点击才设中文键盘第一位；仅清本产品旧 override | E01/E06/E13 | 系统设置/Win+Space 另验 |
| J08 | 两种工具栏投递同一三语 worker 事务 | E02/E06/E18 | 真实点击切换另验 |
| J09 | 按批准策略保留本分支翻译默认及旧 true/false | E01/E13 | 与 P03 一致；实际升级另验 |
| J10 | 瑠/弔辅助码及分发输入补齐，用户覆盖优先 | E01/E08/E13 | 真实辅码另验 |
| J11 | 稳定 ico 安装/注册引用及缺失回退 | E02/E13 | 升级/重登录图标另验 |
| J12 | 旧默认值/扩展键、模型冲突及校验/rename 失败保护 | E01/E10/E13 | 实际升级后 hash 另验 |
| P01 | 八股文一次加成和统一量纲；六参数可配置、旧值保留 | E01/E03 | tiny gram 指标不冒充全部真实语料质量 |
| P02 | 单一有效候选 provider、独立槽位、本地/个人优先 | E01/E06/E12 | 不覆写 Vision 同名配置 |
| P03 | 保留旧翻译 true/false、厂商及坏 TOML 恢复 | E01/E13 | 实际升级比对另验 |
| P04 | 纯简拼精确优先、纠错仍可选，预编辑/消费联动 | E01/E03 | 新版 TSF 实机另验 |
| F01 | 个人产品/repo/chat 频道及资产校验后比较，发现 prerelease | E06/E15 | 最终 Release 与摘要上线核对另验 |

## 功能规划的新增配套

| 项目 | 实现与证据 | 边界 |
|---|---|---|
| 11领域输入库 | E07；全拼/双拼/简拼/词格，逐库和软件inherit/replace，来源徽标/导出；新装默认idioms | 新版TSF逐软件实机另验 |
| 每软件学习隔离 | E01/E03；造词/英文学习/调频与domain集合、隐私学习门禁 | 实际应用/输入框切换另验 |
| 问字/补全 | E01/E06/E12；独立来源、取消/epoch、多答案可编辑、读音/复制/手动上屏，不自动发送或把问题学成拼音 | 正式模型准确率及全部焦点组合未测 |
| QJM/desktop | E04/E10；通变生成/知微重排能力分开、管理和旧文件保护 | tiny真实CPU不代替含章正式大权重/GPU；不充当通用问答 |
| 模板/厂商控件 | E01/E06/E12；CRUD/导入导出/自动保存、模型下拉、独立槽位和Resource ID | 所有文件对话框/厂商API未测 |
| 完整统计/历史 | E01/E06；原四概览/三速度/热图/时段/分类/九列明细保留，日期筛选与30行分页新增，六项生产渲染流程 | 实机两天数据及日期选择通过；30天以上翻页为自动化证据 |
| 原扩展保留 | E11/E13/E17；ASR/TTS/OCR、语音/嘴替/剪贴板/资料库、拼音与发音的入口和资源保留 | 本轮不是重新证明所有大模型/云API/聊天客户端/OCR质量 |

## 已有部分实机证据

根代理已记录本地问字“三个水”→“淼 miǎo”，深色AI任务/润色模板下拉菜单正常；E09完整词典导出重复成功且摘要一致。领域卡片在实际窗口中已修复版本号溢出与相对来源链接：条数、大小、许可分行，8位版本号和固定源码链接可见，隔离开关自动保存后重新打开仍保留。以上是明确场景，不扩大为全导航、全部主题、真模型质量或所有软件输入成功。原后台error根因未回溯定位；新诊断和页内提示已回归。

新版 x64 TSF 实机夹具实际加载安装目录 `msime_v0.10.2/MetasequoiaImeTsf.dll`（0.10.2.0），活动 GUID 为 `{4D59B1B4-D503-44AE-9259-BAD9BB2778AB}`。英文 `hell` 显示单词、句子和中文释义；在同一宿主 PID 50208、Server PID 49928 中通过安装设置关闭/开启英文候选，分别直出文本/立即弹出候选，原配置已恢复。中文 `nihao` 显示带调拼音与完整译义，空格提交“你好”后 Ctrl+Alt+Backspace 撤销为空；选择第3项“你”后 Shift+/ 上屏“你？”，没有原始拼音残留。密码/PIN框的合成字符遮蔽且无候选，测试字符清空，状态导出不读取含密信息。证据：`outputs/v0102-live-tests.json`、`outputs/v0102-tsf-state.json`、`work/v0102-installed-binaries.json`。这不代替未执行的其他软件、RichEdit、物理双屏或云端质量验收。

最终提交检查清空了出厂模板遗留的未知非空 `ai_assistant.token`；18个出厂凭据项均为空或明确占位。用户原有配置及凭据不变。x86也实际加载 `msime_v0.10.2.1` 的0.10.2.0 DLL，在普通EDIT中验证中文拼音/译义/上屏、英文单词和句子及中文释义；状态报告为 `outputs/v0102-tsf-state-x86.json`。本轮后续修复限于设置与服务端资料库，TSF DLL摘要与上述实测版本一致。

## 用户反馈后的回归复核

主题“所有模式都浅色”、统计“变简单”的直接原因是根代理留下了隔离测试设置：该进程的设置页独立主题固定为light，并读空的测试数据目录。用户正常配置仍是system且各界面follow，个人stats.db仍含原有记录。关闭临时窗口、启动不带环境覆盖的日常设置后，真实窗口恢复深色及完整统计；没有用空测试目录覆盖用户数据库。

修复设置主题交互：主主题与设置页独立主题常显，明确说明覆盖关系，一键仅让设置页跟随，不更改其他界面的独立值；主题跟随完整原生配置快照即时应用。非空环境覆盖由原生只提供boolean，测试窗口标题与页内提示明确标识，空环境变量不误报，也不向页面泄露本机路径。

保留统计原四个概览、三个速度、53周热力图、24小时分布、五类字符和九列明细；空快照也保留完整框架，关闭统计不隐藏历史。新增明细开始/结束日期与每页30行分页，默认显示最近30条，不修改总览或数据库。实机临时设置使用个人stats.db的只读备份（不含输入文本），核验全部图表/明细实际布局，日期选择2026-10-12只显示当日，再点全部日期恢复两天；总览数据未改变。长历史翻页/坏范围由六项生产渲染流程测试覆盖，未冒称2天样本完成30天以上实机翻页。

其他确切修复：领域目录缺失/损坏不再连带阻断常用语、语音收藏、复习及软件词库；目录依赖操作先严格校验，失败不保存。已有软件配置坏记录仅从响应副本略过，原文件记录完整保留；所有响应构建在持久化之前，真实保存失败保持旧文件字节。编辑已有英文软件规则与新增规则共用格式/重名校验，无效修改不发送保存。WebView消息订阅与候选预览回调异常隔离，错误提示不抢焦点、不包含配置或异常内容，其余功能继续更新。

实际设置窗口已验证深色、浅色、跟随系统即时切换，独立浅色说明及一键跟随恢复深色；菜单/标题栏/正文同步。修改均在明确的临时配置中测试，用户原配置、聊天、统计及38个模型/语音草稿大小与摘要仍保留。源调用链复核未发现旧消息/枚举或语音、嘴替、剪贴板、OCR、翻译入口删除。详细复核记录在工作区 `outputs/vnext-statistics-regression-audit.md` 与 `vnext-native-regression-audit.md`，不将未测试的正式模型或云端质量宣称为通过。

最终回归包安装exit0、无需重启，注册的x86/x64 DLL仍是0.10.2.0且与canonical产物一致。普通配置设置正常打开为system/follow深色，无临时测试提醒。安装后x64宿主PID47176、Server45392重测 `nihao` 带调拼音/译义、空格上屏“你好”及Ctrl+Alt+Backspace撤销、英文 `hell` 单词/句子/中文释义通过；已关闭测试宿主、恢复原英文状态。原配置/聊天/数据库再次比对及38个模型/语音草稿摘要一致，详见 `outputs/v0102-live-tests.json` 和 `work/v0102-final-installed-binaries.json`。

## 最终交付与实机区

| 门槛 | 已完成证据 | 边界 |
|---|---|---|
| 版本/来源 | 0.10.2，个人频道v0.10.2-chat.1；显式源文件及固定submodule归档 | 最终commit见Release来源manifest；不携带用户数据 |
| 编译/回归 | 两架构TSF各16/16、Engine28/28、原生UI52、Settings221、Server4/4；runtime与真实已有模型见E04/E10/E11 | 各项范围分开，不把skip当推理成功 |
| 真实安装/打开 | 最终重包安装exit0/无需重启，四个安装二进制摘要匹配，正常配置窗口深色且无测试提醒 | 最终资产大小/摘要及安装结果见验证文件 |
| 升级保留 | 配置语义/凭据/聊天/词库/学习统计行、38模型及语音草稿逐项比对 | 允许本地ASR重启轮换回环token和统计正常递增 |
| 文件交互 | 完整中译英GUI重复导出912762条，摘要一致 | 未将完整回导或所有文件对话框批量标为通过 |
| TSF实机 | x64/x86普通EDIT；x64英文即时开关、普通/密码/PIN、前缀标点和撤销；x86中英候选及上屏 | RichEdit、三语/五笔/Shift+Enter和全部第三方应用未逐个重测 |
| 桌面布局 | 深浅/系统主题、独立覆盖、统计实际图表/明细/日期选择、AI/润色菜单、领域卡片 | 物理双屏混合DPI未测；不是全部页面/故障路径操作重放 |
| 模型/云/签名服务 | tiny QJM、原有keyboard/desktop实际推理；模型按需管理保留 | 正式QJM/GPU质量、真实云及hosted SignPath未测 |
| 最终安装资产 | Inno完整包252359122B，SHA256 `ca44c2109c7c25b7e08123782309efdc2af1b3705eba483d1a9ce78ea9855b2d`，NotSigned，Server uiAccess=false | 不声称高完整性应用支持或正式可信签名；摘要见Release |
| Release | 授权推送DiXuanAoYi/msime并发布v0.10.2-chat.1 | 上线后核对tag/资产GitHub摘要及个人频道 |

## 文档核对

`docs_drift.py --repo <docs/plans> --no-git-root --format json`：本轮3份文档的明确链接/命令规则无发现；另逐编号核对79项，读取最终日志、源manifest及导出数据行/大小/SHA256。零发现不证明全部语义或实机安全；没有做新的全仓文档审计，也没有为写此记录改产品代码或用户配置。
