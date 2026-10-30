//! 宠物包：一个目录，里面放 `pet.json` 和立绘 PNG，换皮肤不用改代码。
//!
//! ```json
//! { "name": "我的宠物", "full": "full.png", "peek": "peek.png" }
//! ```
//! `peek` 可省略，省略时趴窗沿也用整身图。目录用环境变量 `ZHIYU_COMPANION_PET` 指定。

use serde::Deserialize;
use std::path::Path;

#[derive(Debug, Deserialize, PartialEq)]
pub struct Manifest {
    #[serde(default)]
    pub name: String,
    pub full: String,
    #[serde(default)]
    pub peek: Option<String>,
}

#[derive(Debug)]
pub struct Pack {
    pub name: String,
    pub full: Vec<u8>,
    pub peek: Option<Vec<u8>>,
}

/// 文件名只能是包目录里的普通文件，不许带路径分隔或 `..`。
fn safe_name(name: &str) -> bool {
    !name.is_empty() && !name.contains(['/', '\\']) && name != ".." && name != "."
}

pub fn load(dir: &Path) -> Result<Pack, String> {
    let text = std::fs::read_to_string(dir.join("pet.json")).map_err(|e| format!("读不到 pet.json：{e}"))?;
    let manifest: Manifest = serde_json::from_str(&text).map_err(|e| format!("pet.json 格式不对：{e}"))?;
    let read = |name: &str| -> Result<Vec<u8>, String> {
        if !safe_name(name) {
            return Err(format!("立绘文件名不合法：{name}"));
        }
        std::fs::read(dir.join(name)).map_err(|e| format!("读不到 {name}：{e}"))
    };
    let full = read(&manifest.full)?;
    let peek = manifest.peek.as_deref().map(read).transpose()?;
    Ok(Pack { name: manifest.name, full, peek })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(tag: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("zhiyu-pack-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn loads_a_pack_with_and_without_peek() {
        let dir = temp("ok");
        std::fs::write(dir.join("a.png"), b"A").unwrap();
        std::fs::write(dir.join("b.png"), b"B").unwrap();
        std::fs::write(dir.join("pet.json"), r#"{"name":"鱼","full":"a.png","peek":"b.png"}"#).unwrap();
        let pack = load(&dir).unwrap();
        assert_eq!((pack.name.as_str(), pack.full.as_slice(), pack.peek.as_deref()), ("鱼", &b"A"[..], Some(&b"B"[..])));
        std::fs::write(dir.join("pet.json"), r#"{"full":"a.png"}"#).unwrap();
        assert!(load(&dir).unwrap().peek.is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn rejects_bad_manifests_and_path_escapes() {
        let dir = temp("bad");
        assert!(load(&dir).is_err(), "没有 pet.json");
        std::fs::write(dir.join("pet.json"), "{").unwrap();
        assert!(load(&dir).is_err());
        std::fs::write(dir.join("pet.json"), r#"{"full":"../secret.png"}"#).unwrap();
        assert!(load(&dir).unwrap_err().contains("不合法"));
        std::fs::write(dir.join("pet.json"), r#"{"full":"missing.png"}"#).unwrap();
        assert!(load(&dir).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
