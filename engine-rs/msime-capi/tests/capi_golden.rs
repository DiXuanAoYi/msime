//! C 接口（Rust 实现）对着真实的 C++ `msime_capi` 核对。基线 `expected_capi.jsonl` 由 `msime_capi_probe` 导出：
//! 200 个会话（全拼、双拼、五笔、日语各 50 个，随机辅助码、学习、模糊音、纠错），每个跑随机的字符、命令、选词键、标点、
//! 选词、整句上屏，每步比较返回值、上屏文字和快照 JSON（长度与哈希）。
//!
//! 接口把 dictionaries 当成 user_data，所以测试把 msime.db、english.db 拷进用户目录；用户目录在会话之间不清（探针同样如此）。
//! 要 `installer/app_data/` 下的词库、语言模型和辅助码表，缺了就跳过。

use msime_capi::*;
use serde_json::Value;
use std::ffi::{c_char, CStr, CString};
use std::fs;
use std::path::{Path, PathBuf};

fn golden_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../engine/contracts/golden")
}

fn app_data() -> PathBuf {
    std::env::var_os("MSIME_APP_DATA").map(PathBuf::from).unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../installer/app_data"))
}

fn fnv(text: &[u8]) -> String {
    let mut hash: u64 = 0xcbf29ce484222325;
    for b in text {
        hash ^= u64::from(*b);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("{hash:016x}")
}

unsafe fn text_of(pointer: *const c_char) -> String {
    CStr::from_ptr(pointer).to_string_lossy().into_owned()
}

#[test]
fn capi_matches_cpp() {
    let data = app_data();
    if ["msime.db", "english.db", "sc.lm", "dict_japanese.dat"].iter().any(|name| !data.join(name).exists()) {
        eprintln!("跳过：{} 下缺 msime.db / english.db / sc.lm / dict_japanese.dat", data.display());
        return;
    }
    let work = std::env::temp_dir().join(format!("msime_capi_{}", std::process::id()));
    let _ = fs::remove_dir_all(&work);
    let user = work.join("user");
    fs::create_dir_all(&user).unwrap();
    fs::copy(data.join("msime.db"), user.join("msime.db")).unwrap();
    fs::copy(data.join("english.db"), user.join("english.db")).unwrap();
    let (resources, user_text, cache) = (
        CString::new(data.to_string_lossy().as_bytes()).unwrap(),
        CString::new(user.to_string_lossy().as_bytes()).unwrap(),
        CString::new(work.join("cache").to_string_lossy().as_bytes()).unwrap(),
    );

    assert_eq!(msime_abi_version(), 2);
    let text = fs::read_to_string(golden_dir().join("expected_capi.jsonl")).unwrap();
    let (mut sequences, mut steps, mut handled, mut committed) = (0, 0, 0, 0);
    for line in text.lines() {
        let record: Value = serde_json::from_str(line).unwrap();
        let scheme = CString::new(record["scheme"].as_str().unwrap()).unwrap();
        let options = MsimeOptions {
            resources_dir: resources.as_ptr(),
            user_data_dir: user_text.as_ptr(),
            cache_dir: cache.as_ptr(),
            scheme: scheme.as_ptr(),
            helpcode: record["help"].as_i64().unwrap() as i32,
            learning: record["learn"].as_i64().unwrap() as i32,
            fuzzy_rules: record["fuzzy"].as_u64().unwrap() as u32,
            autocorrect_types: record["ac"].as_u64().unwrap() as u32,
        };
        let mut error = [0 as c_char; 256];
        unsafe {
            let session = msime_session_create(&options, error.as_mut_ptr(), error.len());
            assert!(!session.is_null(), "创建失败：{}", text_of(error.as_ptr()));
            assert_eq!(text_of(msime_snapshot_json(session)), record["init"].as_str().unwrap(), "初始快照 第 {} 序列", record["round"]);
            for (index, step) in record["ops"].as_array().unwrap().iter().enumerate() {
                let c = || step["c"].as_i64().unwrap() as c_char;
                let rc = match step["op"].as_str().unwrap() {
                    "char" => msime_character(session, c(), 0),
                    "command" => msime_command(session, step["c"].as_i64().unwrap() as i32),
                    "candidate_key" => msime_candidate_key(session, c()),
                    "punctuation" => msime_punctuation(session, c()),
                    "select" => msime_select(session, step["i"].as_u64().unwrap() as usize),
                    "finish" => msime_finish(session),
                    other => panic!("不认识的操作 {other}"),
                };
                let commit = text_of(msime_commit_text(session));
                let snapshot = text_of(msime_snapshot_json(session));
                let label = format!("第 {} 序列 第 {index} 步 {}", record["round"], step["op"]);
                assert_eq!(i64::from(rc), step["rc"].as_i64().unwrap(), "返回值 {label}");
                assert_eq!(commit, step["commit"].as_str().unwrap(), "上屏 {label}");
                let digest = step["snap"].as_array().unwrap();
                assert_eq!(snapshot.len() as u64, digest[0].as_u64().unwrap(), "快照长度 {label}");
                assert_eq!(fnv(snapshot.as_bytes()), digest[1].as_str().unwrap(), "快照哈希 {label}");
                steps += 1;
                handled += rc;
                committed += i32::from(!commit.is_empty());
            }
            msime_session_destroy(session);
        }
        sequences += 1;
    }
    let _ = fs::remove_dir_all(&work);
    eprintln!("C 接口：{sequences} 个序列 {steps} 步，处理 {handled}，有上屏 {committed}");
    assert!(sequences == 200 && steps > 4000 && handled > 2500 && committed > 600);
}

#[test]
fn rejects_missing_options() {
    let mut error = [0 as c_char; 64];
    unsafe {
        assert!(msime_session_create(std::ptr::null(), error.as_mut_ptr(), error.len()).is_null());
        let options = MsimeOptions { resources_dir: std::ptr::null(), user_data_dir: std::ptr::null(), cache_dir: std::ptr::null(), scheme: std::ptr::null(), helpcode: 0, learning: 0, fuzzy_rules: 0, autocorrect_types: 0 };
        assert!(msime_session_create(&options, error.as_mut_ptr(), error.len()).is_null());
        assert_eq!(text_of(msime_commit_text(std::ptr::null())), "");
        assert_eq!(text_of(msime_snapshot_json(std::ptr::null_mut())), "{}");
        assert_eq!(msime_character(std::ptr::null_mut(), b'a' as c_char, 0), 0);
    }
}
