# 本地语音模型与云服务

设置 → 语音输入 →「本地语音模型 · 下载与使用」。安装包包含 CPU 运行组件，模型按需下载。
选择「下载运行包」，等待 SHA-256 校验和展开完成，再点「启动并使用」。需要换模型时先停止当前模型。
模型在用户数据目录 `models/asr` 下，升级安装包不会要求重新下载。

| 模型 | 本页实际下载 | 格式 | 用途 |
| --- | ---: | --- | --- |
| Qwen3-ASR-1.7B | 4.70 GB | 官方原版 | 30 种语言、22 种中文方言，噪声、歌唱和背景音乐场景 |
| Qwen3-ASR-0.6B | 878.7 MB | ONNX INT8 | 多语言与中文方言，更轻量 |
| NVIDIA Parakeet TDT 0.6B v2 | 482.5 MB | ONNX INT8 | 英语高速转写，v2 不支持中文 |
| FunASR SenseVoice-Small | 163.0 MB | ONNX INT8 | 中、英、粤、日、韩五种语言，轻量离线识别 |
| FireRedASR-AED | 2.07 GB | ONNX FP16 | 中文、方言、英语和歌唱；CPU 耗时较长 |

大小来自 `scripts/local_asr/catalog.json` 的锁定文件清单，1 GB = 10 亿字节。
下载中显示已下载 / 总大小及百分比，可暂停续传。完成后检查每个文件的 SHA-256。
ONNX 运行包与官方原版权重大小不同，两者在卡片中分别显示。量化可能影响精度；内存建议是预留参考，不是实测显存占用。
Qwen 1.7B 首次启动还会准备隔离的 Python / PyTorch / Qwen-ASR 环境，额外运行依赖不计入模型下载大小。
其余四款 CPU 模型无需用户安装 Python 或 NVIDIA 驱动。所有模型每段录音最多 60 秒。

识别服务仅监听 `127.0.0.1:18766`，使用每次启动生成的本机凭据，不接受网页跨域请求。
本地识别不上传音频。如果同时开启云端「文本润色」，识别后的文字仍会交给所选润色服务；要完全离线，请关闭文本润色。
运行包缺失、校验失败、端口占用、下载失败会显示原因；不会自动改用云端识别。

每张模型卡提供官方介绍和原版下载链接：

- [Qwen3-ASR-1.7B](https://huggingface.co/Qwen/Qwen3-ASR-1.7B)、[Qwen3-ASR-0.6B](https://huggingface.co/Qwen/Qwen3-ASR-0.6B)
- [Parakeet TDT v2](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v2)
- [SenseVoiceSmall](https://huggingface.co/FunAudioLLM/SenseVoiceSmall)
- [FireRedASR-AED-L](https://huggingface.co/FireRedTeam/FireRedASR-AED-L)

阿里云语音使用百炼 `qwen3-asr-flash`，填写对应地域/工作空间的 API Key 与兼容接口地址。
腾讯云语音使用一句话识别 `SentenceRecognition`，填写 SecretId 与 SecretKey；签名在本机生成。
腾讯云「文本润色」使用混元 OpenAI 兼容 API Key，它与 ASR 的 SecretId/SecretKey 不同。
各提供商旁都有官网、开通/密钥控制台和配置文档链接。凭据按提供商分开保存。

构建本地组件：`scripts/local_asr/Build-Runtime.ps1`。模型不随安装包、源码包或 Git 提交。
模型许可请查对应官方模型卡，运行组件说明见 `scripts/local_asr/THIRD-PARTY-NOTICES.txt`。
