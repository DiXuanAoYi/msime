# 候选发音和英文候选

设置 → **语音与发音** → **候选发音**。默认 Windows 本机语音，不使用云端。
按住 Ctrl 点击原词或译文即可朗读；不会选择候选、上屏或发送消息。
可关闭发音，分别选择中英文音色，调整音量和语速，输入短文本试听。

## 云端发音

Azure、火山引擎、腾讯云、ElevenLabs 的凭据独立保存；切换服务不会覆盖其他厂商配置。
每家都有可点击的控制台、API/音色文档和价格入口。只有明确选择云服务并点击发音/试听时才请求该服务。
Azure 使用 SSML；腾讯使用 TextToVoice / TC3；火山使用 HTTP 单向流；ElevenLabs 支持 v3 和 Flash v2.5。
免费额度、价格、延迟以各厂商当前页面及实际账号为准；Flash v2.5 为 32 种语言。
本轮没有云端 TTS 凭据，验证了请求构造、签名、音频解析和错误处理，未声称完成云端实调用。

## 本地模型按需下载

安装包只包含运行组件和目录，**不含模型权重，不自动下载模型**。
展开“开源发音模型”查看特点、官方介绍、下载网址、文件大小和硬件要求。
目录大小固定到已核对的版本；额外 Python/PyTorch、音频 tokenizer 等依赖不计入权重大小。

- **Kokoro 82M**：中英 INT8 CPU 运行包 147,031,220 字节，可下载、续传、暂停、校验并启动。103 个数字音色 ID；常用别名 af_heart、zf_xiaobei。
- **Qwen3-TTS CustomVoice 0.6B / 1.7B**：约 2.50 / 4.52 GB，支持内置下载和隔离运行。首次启动另行准备依赖，建议独显。运行组件固定配套 torch / torchaudio 2.9.1，避免 Windows 动态库入口不匹配；模型加载只读取本机文件。0.6B 已用现有权重验证 CPU 合成，1.7B 未实测。
- **CosyVoice 2、CSM、Dia、VibeVoice、VibeVoice Realtime、MOSS-TTS-Nano**：提供官方链接、仓库大小和部署说明，需按官方指南运行后接入本机兼容服务。CSM 需要模型访问授权；VibeVoice 1.5B 原长文本 TTS 推理代码已被上游撤下，不能一键启动。Realtime 是另一版本。

Qwen 的 Base 克隆、VoiceDesign 音色设计与 CustomVoice 是不同权重；当前内置运行 CustomVoice，不把克隆和设计宣传为已实现的按钮。
下载依据锁定文件大小及 SHA-256 校验，数据保存在用户数据目录 models/tts，不随升级删除。
启动模型后会切换发音引擎；停止模型不会自动切换到任何云服务，可手动选择 Windows 本机语音。

内置本地服务将最近的发音缓存在内存中（最多 128 条、16 MiB），重复的文字、音色、语言、语速可直接复用。停止服务即释放，不写入磁盘；首次朗读新词仍需要推理，缓存不等于降低首次合成延迟。

候选词即时朗读仍推荐默认的 Windows 本机语音。希望尝试更轻量的开源模型，可查看 [Piper/VITS 官方运行包目录](https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/vits.html)：例如英语 lessac-medium 的 ONNX 模型约 61 MB，中文 zh-ll 约 115 MB；这些是模型文件大小，完整运行包另含词典等文件。它们尚未集成一键启动，也未在本机测得延迟，不能承诺比本机语音更快。当前版本不自动下载这些模型。

外部服务接口：本机回环 HTTP `POST /v1/audio/speech`，JSON 字段 `input/model/voice/language/speed/response_format`，返回 PCM16 WAV。
内置服务端口 18767 有本机令牌验证；外部服务请使用不同端口。只填写回环地址，不能通过“本地”设置把文字发往远程主机。

官方参考：
[Qwen3-TTS](https://github.com/QwenLM/Qwen3-TTS)、[Kokoro 运行包](https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/kokoro.html)、[CosyVoice](https://github.com/FunAudioLLM/CosyVoice)、[CSM](https://github.com/SesameAILabs/csm)、[Dia](https://github.com/nari-labs/dia)、[VibeVoice](https://github.com/microsoft/VibeVoice)、[MOSS](https://github.com/OpenMOSS/MOSS-TTS-Nano)。

## 英文状态

安装或升级后，请先保存工作，将之前已打开的软件完整退出后重新打开（部分聊天软件要从托盘退出）。这些进程可能仍加载旧版输入法 DLL，仅关闭窗口或切换中英文并不一定更新。若仍未生效，可保存工作后注销 Windows 再登录，无需卸载或清空配置。任务栏应选择水杉输入法，再在输入法内切换到“英”；Windows 的 ENG 键盘不提供水杉候选。

切换到英文状态后输入字母会显示离线英文词典候选和中文释义。
输入 `hell`、`thank`、`could` 等前缀可看到常用整句（30 句离线短语，非云端生成）。
空格选词、数字选候选、Enter 原样上屏、Esc 取消；Ctrl+Enter 可上屏选中译文。
CapsLock 在未开始组合时保留直接大写输入，Ctrl/Alt/Windows 应用快捷键继续交给应用。

## 聊天上下文

QQ NT 优先使用消息 ID、正文及发言人结构，排除输入草稿和成员侧栏；历史位置暂停预回复。
微信用已确认的左右气泡证据识别本人/私聊联系人，群聊仍以成员标签为依据。
未确认的布局、图片或发言人不强行推断。预回复不会调用发送按钮或模拟发送键。
