//! 好感度：摸头、喂小鱼干都会涨，存在本机，下次启动接着算。
//!
//! 只记次数和点数，不记任何输入内容。存档坏了或读不到就当新开始，不影响伴侣运行。

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// 摸头每次 +1，两次之间要隔够这么久才算；喂食每次 +3，同样有冷却，免得连点刷满。
pub const PAT_COOLDOWN: f32 = 3.0;
pub const FEED_COOLDOWN: f32 = 20.0;
const PAT_POINTS: u32 = 1;
const FEED_POINTS: u32 = 3;
/// 每升一级要的点数，等级越高越慢。
const STEP: u32 = 10;

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Bond {
    #[serde(default)]
    pub points: u32,
    #[serde(default)]
    pub pats: u32,
    #[serde(default)]
    pub feeds: u32,
    #[serde(skip)]
    pat_wait: f32,
    #[serde(skip)]
    feed_wait: f32,
}

impl Bond {
    /// 时间流逝，冷却减少。
    pub fn tick(&mut self, dt: f32) {
        self.pat_wait = (self.pat_wait - dt).max(0.0);
        self.feed_wait = (self.feed_wait - dt).max(0.0);
    }

    /// 摸头；冷却中返回 `false`（她还是会开心，只是不涨好感）。
    pub fn pat(&mut self) -> bool {
        if self.pat_wait > 0.0 {
            return false;
        }
        self.pat_wait = PAT_COOLDOWN;
        self.pats += 1;
        self.points += PAT_POINTS;
        true
    }

    pub fn feed(&mut self) -> bool {
        if self.feed_wait > 0.0 {
            return false;
        }
        self.feed_wait = FEED_COOLDOWN;
        self.feeds += 1;
        self.points += FEED_POINTS;
        true
    }

    pub fn feed_ready(&self) -> bool {
        self.feed_wait <= 0.0
    }

    /// 等级从 1 开始：第 n 级到第 n+1 级要 `n * STEP` 点。
    pub fn level(&self) -> u32 {
        self.level_and_progress().0
    }

    /// （当前等级，本级已攒点数，本级所需点数）。
    pub fn progress(&self) -> (u32, u32, u32) {
        let (level, into) = self.level_and_progress();
        (level, into, level * STEP)
    }

    fn level_and_progress(&self) -> (u32, u32) {
        let (mut level, mut left) = (1u32, self.points);
        while left >= level * STEP {
            left -= level * STEP;
            level += 1;
        }
        (level, left)
    }

    pub fn load(path: &Path) -> Self {
        std::fs::read_to_string(path).ok().and_then(|text| serde_json::from_str(&text).ok()).unwrap_or_default()
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

/// `%LOCALAPPDATA%\zhiyu-companion\bond.json`；没有这个环境变量（非 Windows）就放在当前用户目录下。
pub fn default_path() -> Option<PathBuf> {
    let base = std::env::var_os("LOCALAPPDATA").map(PathBuf::from).or_else(|| {
        std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local").join("share"))
    })?;
    Some(base.join("zhiyu-companion").join("bond.json"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pats_count_but_not_faster_than_the_cooldown() {
        let mut b = Bond::default();
        assert!(b.pat());
        assert!(!b.pat(), "冷却中不涨");
        b.tick(PAT_COOLDOWN + 0.1);
        assert!(b.pat());
        assert_eq!((b.pats, b.points), (2, 2));
    }

    #[test]
    fn feeding_is_worth_more_and_cools_down_longer() {
        let mut b = Bond::default();
        assert!(b.feed_ready());
        assert!(b.feed());
        assert!(!b.feed_ready() && !b.feed());
        b.tick(PAT_COOLDOWN + 1.0);
        assert!(!b.feed_ready(), "喂食冷却比摸头长");
        b.tick(FEED_COOLDOWN);
        assert!(b.feed());
        assert_eq!(b.points, 6);
    }

    #[test]
    fn levels_get_slower() {
        let mut b = Bond::default();
        assert_eq!(b.progress(), (1, 0, 10));
        b.points = 10;
        assert_eq!(b.progress(), (2, 0, 20));
        b.points = 29;
        assert_eq!(b.progress(), (2, 19, 20));
        b.points = 30;
        assert_eq!(b.level(), 3);
    }

    #[test]
    fn survives_a_round_trip_and_a_broken_file() {
        let dir = std::env::temp_dir().join(format!("zhiyu-bond-{}", std::process::id()));
        let path = dir.join("bond.json");
        let mut b = Bond::default();
        b.pat();
        b.feed();
        b.save(&path);
        let loaded = Bond::load(&path);
        assert_eq!((loaded.points, loaded.pats, loaded.feeds), (4, 1, 1));
        assert!(loaded.feed_ready(), "冷却不存盘，重启后可以直接喂");
        std::fs::write(&path, "{ not json").unwrap();
        assert_eq!(Bond::load(&path), Bond::default());
        assert_eq!(Bond::load(&dir.join("missing.json")), Bond::default());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
