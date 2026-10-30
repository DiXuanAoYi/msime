//! 读取输入法 Server 发来的状态流。
//!
//! 协议：命名管道 `\\.\pipe\MetasequoiaImeCompanion`，每行一个 JSON。只有候选窗的位置、大小、是否可见，
//! 以及语音是否正在录音；不含任何文字、按键或候选内容。坐标是屏幕物理像素。

use serde::Deserialize;
use std::sync::{Arc, Mutex};
use std::time::Duration;

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Feed {
    pub candidate: Option<PixelRect>,
    pub recording: bool,
    /// 是否连着输入法 Server；没连上时伴侣就当普通桌宠。
    pub connected: bool,
}

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct PixelRect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

#[derive(Deserialize)]
struct Wire {
    #[serde(rename = "type")]
    kind: String,
    #[serde(default)]
    candidate: WireCandidate,
    #[serde(default)]
    voice: WireVoice,
}

#[derive(Deserialize, Default)]
struct WireCandidate {
    #[serde(default)]
    visible: bool,
    #[serde(default)]
    x: f32,
    #[serde(default)]
    y: f32,
    #[serde(default)]
    w: f32,
    #[serde(default)]
    h: f32,
}

#[derive(Deserialize, Default)]
struct WireVoice {
    #[serde(default)]
    recording: bool,
}

/// 解析一行；不认识的消息返回 `None`，让以后加字段时旧版伴侣不出错。
pub fn parse_line(line: &str) -> Option<Feed> {
    let wire: Wire = serde_json::from_str(line.trim()).ok()?;
    if wire.kind != "state" {
        return None;
    }
    let candidate = wire.candidate.visible.then_some(PixelRect {
        x: wire.candidate.x,
        y: wire.candidate.y,
        w: wire.candidate.w,
        h: wire.candidate.h,
    });
    Some(Feed { candidate, recording: wire.voice.recording, connected: true })
}

pub type Shared = Arc<Mutex<Feed>>;

/// 后台线程：连上就读，断了每两秒重连。`wake` 在状态变化时调用，用来叫醒界面重绘。
pub fn spawn(wake: impl Fn() + Send + 'static) -> Shared {
    let shared: Shared = Arc::new(Mutex::new(Feed::default()));
    let writer = shared.clone();
    std::thread::spawn(move || loop {
        run_once(&writer, &wake);
        if let Ok(mut state) = writer.lock() {
            *state = Feed::default();
        }
        wake();
        std::thread::sleep(Duration::from_secs(2));
    });
    shared
}

#[cfg(windows)]
fn run_once(shared: &Shared, wake: &impl Fn()) {
    use std::io::{BufRead, BufReader};
    let Ok(pipe) = std::fs::File::open(r"\\.\pipe\MetasequoiaImeCompanion") else {
        return;
    };
    for line in BufReader::new(pipe).lines() {
        let Ok(line) = line else { return };
        if let Some(next) = parse_line(&line) {
            if let Ok(mut state) = shared.lock() {
                if *state != next {
                    *state = next;
                    wake();
                }
            }
        }
    }
}

#[cfg(not(windows))]
fn run_once(_shared: &Shared, _wake: &impl Fn()) {
    // 其他平台的输入法还没有对应的状态流；先当作没连上。
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_candidate_rectangle_and_recording_flag() {
        let feed = parse_line(
            r#"{"type":"state","candidate":{"visible":true,"x":100,"y":200,"w":300,"h":60},"voice":{"recording":true}}"#,
        )
        .unwrap();
        assert_eq!(feed.candidate, Some(PixelRect { x: 100.0, y: 200.0, w: 300.0, h: 60.0 }));
        assert!(feed.recording && feed.connected);
    }

    #[test]
    fn hidden_candidate_window_has_no_rectangle() {
        let feed = parse_line(r#"{"type":"state","candidate":{"visible":false,"x":1,"y":2,"w":3,"h":4},"voice":{"recording":false}}"#).unwrap();
        assert_eq!(feed.candidate, None);
    }

    #[test]
    fn ignores_unknown_messages_and_garbage() {
        assert!(parse_line(r#"{"type":"hello"}"#).is_none());
        assert!(parse_line("not json").is_none());
    }
}
