//! 输入引擎的 C 接口（Rust 实现），ABI 与 `engine/capi/msime_capi.h` 一致：同样的函数名、同样的 `msime_options` 布局、
//! 同样的快照 JSON。对应 `engine/capi/msime_capi.cpp`，由 `tests/capi_golden.rs` 对着 C++ 版逐步核对。
//!
//! 字符串都是 UTF-8、以 0 结尾；返回的 `const char *` 归会话所有，到下一次对同一句柄的调用前有效。

use msime_core::input_session::{Command, KeyResult};
use msime_core::open_session::{open, OpenOptions};
use msime_core::request::SchemeType;
use msime_core::runtime_paths::RuntimeRoots;
use msime_core::session::Session;
use std::ffi::{c_char, c_int, c_uint, CStr, CString};
use std::path::PathBuf;

const ABI_VERSION: c_int = 2;

#[repr(C)]
pub struct MsimeOptions {
    pub resources_dir: *const c_char,
    pub user_data_dir: *const c_char,
    pub cache_dir: *const c_char,
    pub scheme: *const c_char,
    pub helpcode: c_int,
    pub learning: c_int,
    pub fuzzy_rules: c_uint,
    pub autocorrect_types: c_uint,
}

pub struct MsimeSession {
    session: Session<'static>,
    commit: CString,
    snapshot: CString,
}

fn c_string(text: &str) -> CString {
    CString::new(text.replace('\0', "")).unwrap_or_default()
}

impl MsimeSession {
    fn record(&mut self, result: &KeyResult) -> c_int {
        self.commit = c_string(result.commit.as_deref().unwrap_or(""));
        c_int::from(result.handled)
    }
}

fn write_error(buffer: *mut c_char, length: usize, message: &str) {
    if buffer.is_null() || length == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let count = bytes.len().min(length - 1);
    // SAFETY: 调用方保证 buffer 至少有 length 字节。
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer as *mut u8, count);
        *buffer.add(count) = 0;
    }
}

fn parse_scheme(name: *const c_char) -> Option<SchemeType> {
    if name.is_null() {
        return Some(SchemeType::Quanpin);
    }
    // SAFETY: 调用方传的是以 0 结尾的字符串。
    let text = unsafe { CStr::from_ptr(name) }.to_string_lossy();
    match text.as_ref() {
        "" | "quanpin" => Some(SchemeType::Quanpin),
        "shuangpin" => Some(SchemeType::Shuangpin),
        "wubi" => Some(SchemeType::Wubi),
        "japanese" => Some(SchemeType::JapaneseRomaji),
        _ => None,
    }
}

fn scheme_name(scheme: SchemeType) -> &'static str {
    match scheme {
        SchemeType::Shuangpin => "shuangpin",
        SchemeType::Wubi => "wubi",
        SchemeType::JapaneseRomaji => "japanese",
        SchemeType::Quanpin => "quanpin",
    }
}

fn append_json_string(out: &mut String, text: &str) {
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

fn path_of(text: *const c_char) -> PathBuf {
    // SAFETY: 调用方传的是以 0 结尾的 UTF-8 字符串。
    PathBuf::from(unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned())
}

#[no_mangle]
pub extern "C" fn msime_abi_version() -> c_int {
    ABI_VERSION
}

/// # Safety
/// `options` 指向一个按约定填好的 `msime_options`；`error` 至少有 `error_len` 字节（或为空）。
#[no_mangle]
pub unsafe extern "C" fn msime_session_create(options: *const MsimeOptions, error: *mut c_char, error_len: usize) -> *mut MsimeSession {
    let Some(options) = options.as_ref() else {
        write_error(error, error_len, "resources_dir、user_data_dir、cache_dir 都必须提供");
        return std::ptr::null_mut();
    };
    if options.resources_dir.is_null() || options.user_data_dir.is_null() || options.cache_dir.is_null() {
        write_error(error, error_len, "resources_dir、user_data_dir、cache_dir 都必须提供");
        return std::ptr::null_mut();
    }
    let Some(scheme) = parse_scheme(options.scheme) else {
        write_error(error, error_len, "不认识的输入方案");
        return std::ptr::null_mut();
    };
    let user_data = path_of(options.user_data_dir);
    let roots = RuntimeRoots { resources: path_of(options.resources_dir), user_data: user_data.clone(), cache: path_of(options.cache_dir), dictionaries: user_data };
    let mut open_options = OpenOptions::new(roots);
    open_options.scheme = scheme;
    open_options.helpcode = options.helpcode != 0;
    open_options.learning = options.learning != 0;
    open_options.fuzzy_pinyin = options.fuzzy_rules;
    open_options.autocorrect_types = options.autocorrect_types;
    let opened = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| open(&open_options)));
    match opened {
        Ok(Ok(session)) => Box::into_raw(Box::new(MsimeSession { session, commit: CString::default(), snapshot: CString::default() })),
        Ok(Err(message)) => {
            write_error(error, error_len, &message);
            std::ptr::null_mut()
        }
        Err(_) => {
            write_error(error, error_len, "创建会话时发生未知错误");
            std::ptr::null_mut()
        }
    }
}

/// # Safety
/// `session` 是 `msime_session_create` 返回的句柄或空指针，且只销毁一次。
#[no_mangle]
pub unsafe extern "C" fn msime_session_destroy(session: *mut MsimeSession) {
    if !session.is_null() {
        drop(Box::from_raw(session));
    }
}

/// # Safety
/// `session` 是有效句柄或空指针。
#[no_mangle]
pub unsafe extern "C" fn msime_character(session: *mut MsimeSession, value: c_char, shift_only: c_int) -> c_int {
    let Some(s) = session.as_mut() else { return 0 };
    let result = s.session.character(value as u8 as char, shift_only != 0);
    s.record(&result)
}

/// # Safety
/// `session` 是有效句柄或空指针。
#[no_mangle]
pub unsafe extern "C" fn msime_command(session: *mut MsimeSession, command: c_int) -> c_int {
    let Some(s) = session.as_mut() else { return 0 };
    let command = match command {
        0 => Command::Backspace,
        1 => Command::CommitCandidate,
        2 => Command::CommitRaw,
        3 => Command::Cancel,
        4 => Command::MoveLeft,
        5 => Command::MoveRight,
        6 => Command::MoveHome,
        7 => Command::MoveEnd,
        8 => Command::DeleteForward,
        _ => return 0,
    };
    let result = s.session.command(command);
    s.record(&result)
}

/// # Safety
/// `session` 是有效句柄或空指针。
#[no_mangle]
pub unsafe extern "C" fn msime_candidate_key(session: *mut MsimeSession, value: c_char) -> c_int {
    let Some(s) = session.as_mut() else { return 0 };
    let result = s.session.candidate_key(value as u8 as char);
    s.record(&result)
}

/// # Safety
/// `session` 是有效句柄或空指针。
#[no_mangle]
pub unsafe extern "C" fn msime_punctuation(session: *mut MsimeSession, value: c_char) -> c_int {
    let Some(s) = session.as_mut() else { return 0 };
    let result = s.session.punctuation(value as u8 as char);
    s.record(&result)
}

/// # Safety
/// `session` 是有效句柄或空指针。
#[no_mangle]
pub unsafe extern "C" fn msime_select(session: *mut MsimeSession, index: usize) -> c_int {
    let Some(s) = session.as_mut() else { return 0 };
    let result = s.session.select(index);
    s.record(&result)
}

/// # Safety
/// `session` 是有效句柄或空指针。
#[no_mangle]
pub unsafe extern "C" fn msime_finish(session: *mut MsimeSession) -> c_int {
    let Some(s) = session.as_mut() else { return 0 };
    let result = s.session.finish(0);
    s.record(&result)
}

/// # Safety
/// `session` 是有效句柄或空指针。
#[no_mangle]
pub unsafe extern "C" fn msime_commit_text(session: *const MsimeSession) -> *const c_char {
    match session.as_ref() {
        Some(s) => s.commit.as_ptr(),
        None => c"".as_ptr(),
    }
}

/// # Safety
/// `session` 是有效句柄或空指针。
#[no_mangle]
pub unsafe extern "C" fn msime_snapshot_json(session: *mut MsimeSession) -> *const c_char {
    let Some(s) = session.as_mut() else { return c"{}".as_ptr() };
    let snapshot = s.session.snapshot();
    let mut out = String::from("{\"scheme\":\"");
    out.push_str(scheme_name(snapshot.scheme));
    out.push_str("\",\"preedit\":");
    append_json_string(&mut out, &snapshot.preedit);
    out.push_str(",\"raw\":");
    append_json_string(&mut out, &snapshot.raw_segmentation);
    out.push_str(",\"normalized\":");
    append_json_string(&mut out, &snapshot.normalized_segmentation);
    out.push_str(&format!(",\"caret\":{},\"candidates\":[", snapshot.caret_position));
    for (at, item) in snapshot.candidates.iter().enumerate() {
        if at > 0 {
            out.push(',');
        }
        out.push_str("{\"text\":");
        append_json_string(&mut out, &item.word);
        out.push_str(",\"pinyin\":");
        append_json_string(&mut out, &item.pinyin);
        out.push_str(&format!(",\"weight\":{},\"fixed\":{},\"fuzzy\":{}}}", item.weight, item.fixed_position, if item.fuzzy { "true" } else { "false" }));
    }
    out.push_str("]}");
    s.snapshot = c_string(&out);
    s.snapshot.as_ptr()
}
