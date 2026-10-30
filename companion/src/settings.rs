//! 伴侣自己的小设置：置顶、气泡、上次站的位置。存在本机，读不到就用默认。

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Settings {
    #[serde(default = "yes")]
    pub always_on_top: bool,
    #[serde(default = "yes")]
    pub bubbles: bool,
    /// 上次站在屏幕横向的哪里（0~1），下次启动回到这里。
    #[serde(default = "default_x")]
    pub x_ratio: f32,
}

fn yes() -> bool {
    true
}

fn default_x() -> f32 {
    0.8
}

impl Default for Settings {
    fn default() -> Self {
        Self { always_on_top: true, bubbles: true, x_ratio: default_x() }
    }
}

impl Settings {
    pub fn load(path: &Path) -> Self {
        let mut s: Self = std::fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
        s.x_ratio = if s.x_ratio.is_finite() { s.x_ratio.clamp(0.0, 1.0) } else { default_x() };
        s
    }

    pub fn save(&self, path: &Path) {
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        if let Ok(text) = serde_json::to_string_pretty(self) {
            let _ = std::fs::write(path, text);
        }
    }
}

pub fn default_path() -> Option<PathBuf> {
    crate::bond::default_path().map(|p| p.with_file_name("settings.json"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_when_missing_or_broken_and_clamps_position() {
        let dir = std::env::temp_dir().join(format!("zhiyu-settings-{}", std::process::id()));
        let path = dir.join("settings.json");
        assert_eq!(Settings::load(&path), Settings::default());
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(&path, r#"{"bubbles":false,"x_ratio":7.5}"#).unwrap();
        let s = Settings::load(&path);
        assert!(!s.bubbles && s.always_on_top);
        assert_eq!(s.x_ratio, 1.0);
        let saved = Settings { always_on_top: false, bubbles: true, x_ratio: 0.25 };
        saved.save(&path);
        assert_eq!(Settings::load(&path), saved);
        std::fs::write(&path, "oops").unwrap();
        assert_eq!(Settings::load(&path), Settings::default());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
