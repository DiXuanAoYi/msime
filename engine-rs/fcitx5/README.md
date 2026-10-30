# fcitx5-msime

水杉输入法的 Linux 前端：一个 fcitx5 引擎插件，背后是 Rust 的 `msime-capi`（与 Windows 端用同一份引擎逻辑，经 C 接口 `engine/capi/msime_capi.h` 调用）。

## 结构

| 文件 | 职责 | 依赖 fcitx5 |
|---|---|---|
| `src/snapshot.*` | 解析 `msime_snapshot_json` 的快照 | 否 |
| `src/key_map.*` | 按键（X11 keysym + 修饰键）→ 对 C 接口的一次调用 | 否 |
| `src/driver.*` | 持有会话，执行动作，读回上屏文字与快照；首次使用把词库拷进用户目录 | 否 |
| `src/engine.*` | `InputMethodEngineV2`：收按键、上屏、预编辑、候选窗（`CommonCandidateList` 翻页） | 是 |
| `data/` | addon 与输入法的 `.conf` | — |
| `tests/frontend_test.cpp` | 快照解析、按键映射、真实 capi 的端到端 | 否 |

前三个文件不依赖 fcitx5，所以没装 fcitx5 开发包也能构建和测试；引擎本身没有页的概念，候选窗翻页由 fcitx5 的候选列表负责，数字键换算成总序号（页首序号 + 数字 − 1）后调 `msime_select`。

按键约定：Ctrl / Alt / Super 组合一律交还应用；Shift+大写字母作为 `shift_only`（本地模式触发键）；组合中空格上屏高亮候选、回车上屏原文、Esc 取消、PageUp/PageDown 翻页；没有组合时只有字母和标点会被接管。

## 构建与测试

```bash
cmake -S engine-rs/fcitx5 -B build/fcitx5 -DCMAKE_BUILD_TYPE=Release
cmake --build build/fcitx5
MSIME_APP_DATA=<资源目录> ctest --test-dir build/fcitx5 --output-on-failure
```

资源目录要有 `msime.db`、`english.db` 和语言模型等（仓库里是 `installer/app_data`）。没设 `MSIME_APP_DATA` 时端到端部分跳过。

装到系统后，资源放 `/usr/share/msime`（可在插件配置里改 `ResourcesDir`），用户词库在 `~/.local/share/fcitx5/msime`。

## 状态

- 核心（快照解析、按键映射、会话封装）在 WSL 里对真实 capi 通过测试。
- 插件在真实的 fcitx5 `Instance` 里通过集成测试（`tests/engine_test.cpp`，用 fcitx5 自带的 testfrontend 加载 `libmsime.so`、敲 `nihao`、空格上屏第一个候选、Backspace/Esc、Ctrl 组合放行）。需要 `libfcitx5core-dev`、`fcitx5-modules-dev`，资源目录同样取 `MSIME_APP_DATA`。
- 引擎读取 `MSIME_RESOURCES_DIR`、`MSIME_USER_DATA_DIR`、`MSIME_CACHE_DIR`（优先于配置），便携安装和自动测试用。
- 还没做：真实桌面会话里的手动试用、候选窗外观（走 fcitx5 的经典界面）、状态栏图标、中英文切换、设置页。
