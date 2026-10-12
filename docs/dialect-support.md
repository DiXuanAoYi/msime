# 方言与民族语言支持范围

声音音色与方言是不同能力。选择参考音色或克隆声音，不保证模型能够说原本不支持的方言。输入法不会自动下载新增模型权重。

| 语言或方言 | 已接入方式 | 使用条件 |
|---|---|---|
| 北京话、四川话 | 本地 Qwen CustomVoice 的 Dylan / Eric；MOSS 有京味参考音色 | 选择模型真实内置音色，先试听 |
| 上海话（吴语） | Azure `wuu-CN-XiaotongNeural` | 需 Azure 服务凭据和可用地域；不等于苏州话 |
| 河南、陕西、四川、山西、安徽、湖南、甘肃、山东、辽宁、闽南 | Azure XiaoxiaoDialects 音色加实际 locale | 需凭据，方言通过 SSML `lang` 发送 |
| 东北、陕西、四川 | 火山 Vivi 加 `explicit_dialect` | 需对应资源权限；其他方言不能假定 Vivi 通用 |
| 火山其他支持的方言 | 已接 `音色ID|方言代码` | 控制台必须明确此音色支持该方言；云端模型版本会影响范围 |
| 中部藏语、安多藏语 | 本地 MMS，官方单音色 | 输入藏文，用户按需下载；逐句生成，非句内流式 |
| 维吾尔语 | 本地 MMS `uig-script_arabic` | 输入阿拉伯文字 |
| 蒙古语 | 本地 MMS `mon` | 输入西里尔文字；不宣称支持传统蒙古文 |
| 哈萨克语 | 本地 MMS `kaz` | 输入对应语言文字 |
| 苏州话 | 暂无已核实内置权重或音色 | 可连接用户已验证的本机兼容服务，当前不能声明输入法已内置支持 |

MMS 模型为 CC-BY-NC-4.0，仅限非商业用途。新增的 MMS 权重没有自动下载，模型实际音质与延迟仍待用户按需下载后试听验证。这里的官方元数据、下载大小与文件摘要核验，不代表真实模型推理已完成。

本机兼容接口使用 `/v1/audio/speech` 请求，用户可填写自部署服务已登记的参考音色 ID。登记、参考音频和真正的方言能力由该服务负责；本输入法不会把普通话换音色标成方言。

核查依据：

- [Azure 语言与音色支持](https://learn.microsoft.com/en-us/azure/cognitive-services/speech-service/language-support?tabs=speaker-recognition)：XiaoxiaoDialects 的 secondary locales 包含河南等 locale 和 `wuu-CN`。
- [火山 HTTP 流式 API](https://docs.volcengine.com/docs/DoubaoVoice/unidirectional-streaming-text-to-speech-http?lang=zh)：`explicit_dialect` 代码必须搭配支持对应方言的 speaker。
- [火山 V3 音色限制](https://docs.volcengine.com/docs/DoubaoVoice/WebSocketBidirectionalStreaming-V3?lang=zh)：该版本明确 Vivi 支持东北、陕西、四川，因此预置采用这三个已核实组合。
- Meta 官方 MMS：[蒙古语](https://huggingface.co/facebook/mms-tts-mon)、[维吾尔语](https://huggingface.co/facebook/mms-tts-uig-script_arabic)、[安多藏语](https://huggingface.co/facebook/mms-tts-adx)。
- 苏州话检索到的 [MagicHub 吴语语料](https://huggingface.co/datasets/MagicHub/magicdata-dialect-wu-chinese-tts-lite) 是数据集，不能当成可直接运行的 TTS 模型。
